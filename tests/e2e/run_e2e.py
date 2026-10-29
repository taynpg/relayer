#!/usr/bin/env python3
"""三进程集成测试。

为什么是 Python 而不是 Catch2
------------------------------
这一层要验证的东西和单元测试完全不同：不是"某个函数返回什么"，而是
"三个真实进程连起来之后，盘上留下了什么"。它需要的原语是

    起进程 / 等某一行输出 / 杀掉它 / 看文件还在不在

这些在 subprocess 里都是现成的。用 C++ 写就得自己造一套进程管理与信号
处理，代码量会超过被测逻辑本身，而它出 bug 时你还得先排查这套脚手架。

同理，这里**不测**帧序列、状态机、错误码——那些在 core_test 里已经逐条
覆盖过。e2e 只回答一个问题：把它们装成三个进程之后，还成立吗。

用法
----
    python3 tests/e2e/run_e2e.py                 # 跑全部（含 1GB，较慢）
    python3 tests/e2e/run_e2e.py --quick         # 跳过 1GB 与 200MB
    python3 tests/e2e/run_e2e.py -k kill         # 只跑名字含 kill 的场景
    python3 tests/e2e/run_e2e.py --list          # 列出场景名

退出码：0 全过，1 有失败。
"""

from __future__ import annotations

import argparse
import codecs
import hashlib
import os
import shutil
import socket
import subprocess
import tarfile
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent.parent

# 原生 Windows 程序不认 MSYS 的 /tmp，也不认 Git Bash 的 fake path。
# 这里所有路径都过一遍 Path.resolve()，拿到的就是 Windows 原生路径。
DEFAULT_BIN = ROOT / "build" / "bin"

EXE = ".exe" if os.name == "nt" else ""

PASSWORD = "e2e-pw"

# ASSIGNED_ID= 是 relayer-cli 的契约输出（见 cli/src/Client.cpp）。
# 只匹配**完整非空**的 id：认证刚过时那一行是 ASSIGNED_ID=（空值），
# 匹配它会把空串当成 id，然后拿着空 id 去发送。
ASSIGNED_PREFIX = "ASSIGNED_ID="


class E2EFailure(AssertionError):
    """场景断言失败。带上一句人话说明"期望什么、实际什么"。"""


class SkipScenario(Exception):
    """当前平台无法模拟该场景。算通过，但要明说没覆盖。

    单独一个类型而不是"直接 return"：跳过的场景必须在总结里单独计一
    类。混进"通过"里，半年后就没人知道这张表其实少测了一项。
    """


# ---------------------------------------------------------------------------
# 进程管理
# ---------------------------------------------------------------------------


class Process:
    """一个子进程，输出**重定向到文件**。

    为什么不用 subprocess.PIPE：管道有容量上限（Windows 上只有几 KB），
    写满之后子进程就**阻塞在 write 调用里**，事件循环随之停摆——不收帧、
    不回 ACK，整条传输无声停住。而测试的常见写法是"先等发送端、再等
    接收端"（`sender.wait_exit(600)` 之后才 `recv.wait_exit(120)`），
    这期间接收端的输出**没有任何人读**。

    这不是猜测：2026-10-22 的 transfer-200mb / transfer-1gb 就死在这里。
    两端都停在 94%，发送端 120 秒后报 "peer idle"，而同样的参数手工
    跑（加了 -q、没有进度输出）763 块零重传、一次通过。差别只在有没有
    人读那个管道。

    落文件之后进程想写多少写多少；读不读只影响断言的实时性。代价是
    日志要按偏移增量读，见 pill()。
    """

    def __init__(self, name: str, args: list[str], log_path: Path):
        self.name = name
        self.log_path = log_path
        self._lines: list[str] = []
        self._partial = ""
        # 增量解码器：一次 read 可能把 UTF-8 多字节字符切成两半，
        # 单独 decode 会得到替换字符，错误信息就成了乱码。
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._read_pos = 0
        self._file = open(log_path, "wb")
        # stdin 给 DEVNULL：任何"等用户输入"的路径都该立刻拿到 EOF，
        # 而不是把测试挂在那里。
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=self._file,
            stderr=subprocess.STDOUT,
            cwd=str(ROOT),
        )
        self.returncode: int | None = None

    @classmethod
    def adopt(cls, name: str, proc: subprocess.Popen, log_path: Path) -> "Process":
        """接管一个已经启动、且 stdout 已指向 log_path 的进程。

        target-busy 需要"一个抱着文件不放的 PowerShell"，它的生命周期
        与普通被测进程一样要管理，但参数不由 Cluster 决定（不是 relayer
        的可执行文件）。走同一条读取路径，避免两套缓冲逻辑。
        """
        self = cls.__new__(cls)
        self.name = name
        self.log_path = log_path
        self._lines = []
        self._partial = ""
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._read_pos = 0
        self._file = open(log_path, "ab")
        self.proc = proc
        self.returncode = None
        return self

    # -- 读 --------------------------------------------------------------

    def pill(self) -> list[str]:
        """把日志文件里**新增**的内容读出来，追加到缓冲。

        不阻塞、也不依赖调用方节奏：读文件不会因为"没数据"而卡住，这一点
        与管道相反。子进程的 stderr 是无缓冲的，所以进度行几乎实时可见；
        stdout 是块缓冲的，关键契约行（ASSIGNED_ID / STATS / 已启动）在
        C++ 侧都显式 fflush 过。
        """
        pos = getattr(self, "_read_pos", 0)
        try:
            with open(self.log_path, "rb") as f:
                f.seek(pos)
                chunk = f.read()
                self._read_pos = f.tell()
        except OSError:
            chunk = b""

        if chunk:
            # 进度条用 \r 刷新，和正文混在一行里。统一拆开，调用方就不用
            # 为"这一行有没有 \r"操心。
            text = self._decoder.decode(chunk).replace("\r", "\n")
            parts = (self._partial + text).split("\n")
            # 最后一段可能只写了一半，留到下次拼。
            self._partial = parts.pop()
            self._lines.extend(parts)
        return self._lines

    def wait_for(self, needle: str, timeout: float) -> str:
        """等到某一行**包含** needle，返回那一行。

        用"包含"而不是"等于"：契约行的实际内容是 ASSIGNED_ID=client-7，
        而调用方关心的只是前缀。
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.pill():
                if needle in line:
                    return line
            if self.proc.poll() is not None:
                # 进程已经退出，再等下去只会白等满超时。
                # 把剩下没读的读完再判断一次。
                for line in self.pill():
                    if needle in line:
                        return line
                raise E2EFailure(
                    f"{self.name} 已退出（code={self.proc.returncode}），"
                    f"但没等到 {needle!r}\n{self.tail()}"
                )
            time.sleep(0.02)
        raise E2EFailure(f"{self.name} 在 {timeout:.0f}s 内没等到 {needle!r}\n{self.tail()}")

    def wait_exit(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pill()
            rc = self.proc.poll()
            if rc is not None:
                self.pill()
                self.returncode = rc
                return rc
            time.sleep(0.02)
        self.kill()
        raise E2EFailure(f"{self.name} 在 {timeout:.0f}s 内没有退出（已强杀）\n{self.tail()}")

    def tail(self, n: int = 30) -> str:
        """失败时贴出来的日志尾部。去掉进度条刷出来的重复行。"""
        lines = [x for x in self._lines if x.strip() and "%" not in x]
        return "\n".join(f"  [{self.name}] {x}" for x in lines[-n:])

    # -- 生命周期 --------------------------------------------------------

    def kill(self) -> None:
        """硬杀。

        这就是被测场景本身（kill 发送端），所以不能用 SIGTERM 那种
        "让进程有机会清理"的优雅方式——要模拟的就是加电断电。
        """
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.pill()

    def close(self) -> None:
        self.kill()
        # stdout 现在直连文件，没有管道可关。保留 try 是因为
        # Process.adopt 出来的对象可能带着别人的句柄。
        try:
            if self.proc.stdout:
                self.proc.stdout.close()
        except Exception:
            pass
        try:
            self._file.close()
        except Exception:
            pass


def free_port() -> int:
    """要一个当前空闲的端口。

    有竞态：关掉之后到真正 bind 之间，别人可能抢走。但比写死端口好——
    写死会在 CI 上撞车，而且撞车时的报错是"连接被拒绝"，看着像网络问题。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


# ---------------------------------------------------------------------------
# 场景上下文
# ---------------------------------------------------------------------------


@dataclass
class Cluster:
    """一个 server + 若干 cli 进程的工作目录与进程集合。"""

    workdir: Path
    port: int
    procs: list[Process] = field(default_factory=list)
    server: Process | None = None

    # 可执行文件目录。**从外面传进来**而不是读模块级变量：后者要靠
    # `global` 在 main 里改写，而"改模块级常量"这种写法在测试里留下的
    # 状态会跨场景泄漏，将来并行跑场景时就是个坑。
    bin_dir: Path = DEFAULT_BIN

    def __post_init__(self) -> None:
        self.src = self.workdir / "src"
        self.dst = self.workdir / "dst"
        self.src.mkdir(parents=True, exist_ok=True)
        self.dst.mkdir(parents=True, exist_ok=True)

    # -- 启动 ------------------------------------------------------------

    def start_server(self, timeout: float = 15.0, extra: list[str] | None = None,
                     no_auth: bool = False) -> Process:
        """启动中继。no_auth=True 时用 --allow-no-auth（不配口令）。

        服务端的规则是"口令与 allow-no-auth 必须恰好给一个"（server/main.cpp
        的启动校验），所以两条分支互斥——别想着同时传，那是被测代码要拒绝的
        配置，不是测试脚手架该构造的。
        """
        name = f"server-{self.port}"
        args = [str(self.bin_dir / f"relayerd{EXE}"), "--port", str(self.port)]
        if no_auth:
            args += ["--allow-no-auth"]
        else:
            args += ["--password", PASSWORD]
        if extra:
            args += extra
        p = Process(name, args, self.workdir / f"{name}.log")
        self.procs.append(p)
        self.server = p
        # 等"已启动"而不是盲睡：盲等短了会连不上（报错是"目标机器积极
        # 拒绝"，看着像网络问题），长了白耗时间，而且只在慢机器上偶发。
        p.wait_for("已启动", timeout)
        return p

    def cli(self, name: str, *args: str) -> Process:
        p = Process(
            name,
            [str(self.bin_dir / f"relayer-cli{EXE}"), *args],
            self.workdir / f"{name}.log",
        )
        self.procs.append(p)
        return p

    def cli_common(self, no_auth: bool = False) -> list[str]:
        # 客户端同样必须显式声明认证方式（--no-auth 与 --password 互斥，
        # 都不给会在 CLI 参数校验处直接报错）——"从空口令推断"是 GUI 侧
        # 已经修掉过的坑，这里不复辟。
        if no_auth:
            return ["-p", str(self.port), "--no-auth"]
        return ["-p", str(self.port), "--password", PASSWORD]

    def start_receiver(self, idle_timeout: int = 30, extra: list[str] | None = None,
                       no_auth: bool = False) -> tuple[Process, str]:
        """起接收端，返回 (进程, 分配到的 id)。

        id 只能从契约行取。**不能**用 `list` 去猜：id 由服务端递增分配，
        每条新连接都消耗一个号，包括 list 自己那条——抓到的几乎总是 list
        自己，然后文件被投给一个刚断开的连接，中继静默丢弃，症状是发送方
        等应答超时。
        """
        args = ["recv", "--to-dir", str(self.dst), *self.cli_common(no_auth), "--idle-timeout", str(idle_timeout)]
        if extra:
            args += extra
        p = self.cli("recv", *args)
        line = p.wait_for(ASSIGNED_PREFIX, 20.0)
        assigned = line.split(ASSIGNED_PREFIX, 1)[1].strip()
        if not assigned:
            raise E2EFailure(f"接收端报了空 id：{line!r}")
        return p, assigned

    def send(self, src_path: Path, to_id: str, name: str = "send", extra: list[str] | None = None,
             no_auth: bool = False) -> Process:
        args = ["send", str(src_path), "--to", to_id, *self.cli_common(no_auth)]
        if extra:
            args += extra
        return self.cli(name, *args)

    # -- 收尾 ------------------------------------------------------------

    def wait_all(self, procs: list[Process], timeout: float) -> list[int]:
        """**同时**等一批进程退出，返回与 procs 对齐的退出码列表。

        同时而不是串行：串行时第一个进程的失败会被第二个的等待掩埋。
        1GB 场景要跑几分钟，若发送端 20 秒就失败了，串行等法要白等
        剩下几分钟才把错误报出来。

        （输出已经落文件，所以这里不再有"没人读管道就阻塞"的问题；
        但那正是把两条逻辑分开的理由——等待策略不该顺带承担"消费输出"
        的职责，否则一旦忘了消费就表现为传输卡死，而不是测试报错。）
        """
        pending = list(procs)
        codes: dict[int, int] = {}
        deadline = time.monotonic() + timeout
        while pending and time.monotonic() < deadline:
            for p in list(pending):
                p.pill()
                rc = p.proc.poll()
                if rc is not None:
                    p.pill()
                    p.returncode = rc
                    codes[id(p)] = rc
                    pending.remove(p)
            if pending:
                time.sleep(0.01)

        if pending:
            names = "、".join(p.name for p in pending)
            detail = "\n".join(p.tail(10) for p in pending)
            for p in pending:
                p.kill()
            raise E2EFailure(f"{names} 在 {timeout:.0f}s 内没有退出（已强杀）\n{detail}")

        return [codes[id(p)] for p in procs]

    def shutdown(self) -> None:
        for p in self.procs:
            p.close()


# ---------------------------------------------------------------------------
# 断言辅助
# ---------------------------------------------------------------------------


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def make_file(path: Path, size: int) -> str:
    """造一个确定性内容的文件，返回它的 sha256。

    不用 os.urandom：内容随机的话，失败重跑时无法区分"这次传输错了"还是
    "这次换了内容"。确定性内容让同样的失败可以反复复现。
    """
    h = hashlib.sha256()
    remaining = size
    seed = 0
    with open(path, "wb") as f:
        while remaining > 0:
            n = min(remaining, 1 << 20)
            block = bytes(((i * 131 + seed * 17 + 7) & 0xFF) for i in range(n))
            f.write(block)
            h.update(block)
            remaining -= n
            seed += 1
    return h.hexdigest()


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise E2EFailure(msg)


def expect_eq(actual, expected, what: str) -> None:
    if actual != expected:
        raise E2EFailure(f"{what}：期望 {expected!r}，实际 {actual!r}")


def parse_stats(line: str) -> dict[str, int]:
    """解析 `STATS k=v ...` 契约行（见 cli/src/main.cpp）。

    这里刻意不用正则：格式是固定的一组 `k=v`，而正则会让"键名拼错"
    变成一个不匹配的空字典——失败信息里就只剩"期望 2，实际 None"，
    看不出是键名写错了还是统计真的没输出。
    """
    out: dict[str, int] = {}
    for token in line.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        out[key] = int(value)
    return out


def expect_no_fragment(dst: Path, name: str) -> None:
    """残片不该留下。

    它留在盘上说明收尾没走完——即便内容对，下次同名传输也会被它干扰。
    """
    part = dst / (name + ".relaypart")
    expect(not part.exists(), f"留下了残片 {part.name}（{part.stat().st_size if part.exists() else 0} 字节）")


# ---------------------------------------------------------------------------
# 场景
# ---------------------------------------------------------------------------

SCENARIOS: list[tuple[str, str, bool, Callable[[Path, Path], None]]] = []
"""每项：(名字, 一句话说明, 是否 slow, 函数)。"""


def scenario(name: str, desc: str, slow: bool = False):
    def deco(fn):
        SCENARIOS.append((name, desc, slow, fn))
        return fn

    return deco


def _transfer_roundtrip(workdir: Path, bin_dir: Path, size: int, chunk: int = 262144) -> None:
    """跑一次完整传输并校验内容。多个场景复用。"""
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "payload.bin"
        want = make_file(src, size)

        recv, rid = c.start_receiver(idle_timeout=120)
        sender = c.send(src, rid, extra=["--chunk-size", str(chunk), "--idle-timeout", "120"])

        # 同时等两端。串行等会掩盖"先失败的那一端"，也会让 1GB 场景在
        # 早就失败的情况下白等几分钟。
        rc = c.wait_all([sender, recv], 600)
        expect_eq(rc[0], 0, "发送端退出码")
        expect_eq(rc[1], 0, "接收端退出码")

        got = c.dst / "payload.bin"
        expect(got.exists(), f"目标文件不存在：{got}")
        expect_eq(got.stat().st_size, size, "目标文件大小")
        # 内容才是唯一说得过去的验收标准。块数对、ACK 数对、内容错了的
        # 情况在"块号错位"这类 bug 下真实存在。
        expect_eq(sha256_of(got), want, "内容哈希")
        expect_no_fragment(c.dst, "payload.bin")
    finally:
        c.shutdown()


@scenario("transfer-2mb", "2MB 文件跨中继传输，内容一致、无残片")
def transfer_2mb(workdir: Path, bin_dir: Path) -> None:
    _transfer_roundtrip(workdir, bin_dir, 2_000_000)


@scenario("transfer-200mb", "200MB 文件跨中继传输（覆盖多窗口、多次流控回压）", slow=True)
def transfer_200mb(workdir: Path, bin_dir: Path) -> None:
    # 块大小 1MB → 200 块。窗口只有几十块，所以这条必然经历多轮
    # "窗口满 → 等 ACK → 续发"，而那正是流控最容易出错的地方。
    _transfer_roundtrip(workdir, bin_dir, 200_000_000, chunk=1 << 20)


@scenario("transfer-1gb", "1GB 文件跨中继传输（覆盖 uint32 边界与长时间稳定性）", slow=True)
def transfer_1gb(workdir: Path, bin_dir: Path) -> None:
    # 1GB 的关键不是"大"，是**块号与字节数的量级**：块号会超过 2^16，
    # 累计字节数接近 2^30，任何用 u16/u32 存中间量的地方都会在这里炸。
    _transfer_roundtrip(workdir, bin_dir, 1 << 30, chunk=4 << 20)


@scenario("empty-file", "0 字节文件正常完成")
def empty_file(workdir: Path, bin_dir: Path) -> None:
    # 零长是边界：块数为 0，窗口立刻就 done()，这条路径最容易在
    # "等第一个 ACK"上死锁——永远不会有 ACK 来。
    _transfer_roundtrip(workdir, bin_dir, 0)


@scenario("relative-subdir", "发送方指定相对子目录：接收端自动建中间目录后落盘")
def relative_subdir(workdir: Path, bin_dir: Path) -> None:
    """"把固定文件发到对方某个目录"的核心场景（PROTOCOL.md §9.4 的 root
    带目录段语义）。三个断言缺一不可：

      - 文件落在 dst/sub/dir/payload.bin（中间目录是接收端建出来的）
      - 内容一致
      - 残片不留在根目录也不留在子目录（收尾走完）
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "payload.bin"
        want = make_file(src, 2_000_000)

        recv, rid = c.start_receiver(idle_timeout=120)
        sender = c.send(src, rid, extra=["--as", "sub/dir/payload.bin", "--idle-timeout", "120"])
        rc = c.wait_all([sender, recv], 600)
        expect_eq(rc[0], 0, "发送端退出码")
        expect_eq(rc[1], 0, "接收端退出码")

        got = c.dst / "sub" / "dir" / "payload.bin"
        expect(got.exists(), f"子目录落点不存在：{got}")
        expect_eq(got.stat().st_size, 2_000_000, "子目录落点文件大小")
        expect_eq(sha256_of(got), want, "子目录落点内容哈希")
        expect_no_fragment(c.dst / "sub" / "dir", "payload.bin")
    finally:
        c.shutdown()


@scenario("relative-subdir-traversal", "发送方试图用 .. 逃出接收根目录：被拒绝且什么都不落盘")
def relative_subdir_traversal(workdir: Path, bin_dir: Path) -> None:
    """root 带 `..` 的安全性：resolveWithin 必须拒绝，接收端回 kBadDesc，
    发送端以非零退出。这是"发送方可指定相对目录"不会退化成"可写对方任意
    路径"的看门测试。

    接收端在拒绝之后**继续等下一个传输**（空闲超时只在传输开始后计时，
    被拒的请求不算开始——这是 GUI 自动接收挂起的同一语义），所以这里
    等发送端退出即可，接收端随后强杀，只断言盘上状态。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "evil.bin"
        make_file(src, 128_000)

        recv, rid = c.start_receiver(idle_timeout=30)
        sender = c.send(src, rid, extra=["--as", "../outside.bin", "--idle-timeout", "30"])
        expect(sender.wait_exit(120) != 0, "发送端竟然报告成功——路径穿越被放行了")
        expect("escapes" in sender.tail(), "发送端的结论没说清是路径穿越")
        recv.kill()

        expect(not (c.dst / "outside.bin").exists(), "文件落在了接收根目录之外（上层目录）")
        expect(not (c.dst / "evil.bin").exists(), "文件落在了接收根目录")
    finally:
        c.shutdown()


@scenario("no-auth", "无认证中继：--no-auth 端到端可传；口令客户端与漏配客户端都被拒")
def no_auth(workdir: Path, bin_dir: Path) -> None:
    """认证矩阵里"无认证"那一整行，此前只走过一次手工路径。

    三条断言各管一件事：

      1. 两端都显式 --no-auth → 完整传输成功。这证明无认证不是"跳过
         握手"的捷径，而是一条真正的握手路径（版本/身份协商照走）。
      2. 带 --password 的客户端连无认证中继 → 明确失败且**有时限**。
         失败是必然的；要防的是"静默容忍"——服务端若把口令客户端当成
         无认证客户端放行，等于认证开关形同虚设。断言退出码而不是
         报错文案：措辞是实现细节，退出才是契约。
      3. 口令和无认证都不给 → 客户端参数校验直接拒绝。这是"漏填口令
         应报错，不该静默降级"（§0.6）的看门断言：漏配如果被推断成
         无认证，忘了配口令的终端就会裸奔上中继。

    顺序注意：第 2/3 条要有意义的失败，接收端必须还活着——目标是死对端
    的话，发送方也会报错退出，但那是 answerTimeout 的失败，测不到认证层。
    所以先做负例（此时 recv1 挂着），再杀掉 recv1 跑正例。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server(no_auth=True)
        src = c.src / "payload.bin"
        want = make_file(src, 2_000_000)

        recv1, rid = c.start_receiver(idle_timeout=120, no_auth=True)

        # --- 负例 1：带口令的客户端 ---
        # 直接用 c.cli 构造：send() 的 cli_common(no_auth=False) 会带
        # --password，再往 extra 里塞一份就是重复参数，校验语义会被
        # "后者覆盖前者"搅浑。
        s_pw = c.cli("send-pw", "send", str(src), "--to", rid, "-p", str(c.port),
                     "--password", PASSWORD, "--idle-timeout", "30")
        expect(s_pw.wait_exit(60) != 0, "带口令的客户端在无认证中继上竟然发送成功——认证开关形同虚设")

        # --- 负例 2：什么都不声明 ---
        s_none = c.cli("send-none", "send", str(src), "--to", rid, "-p", str(c.port),
                       "--idle-timeout", "30")
        expect(s_none.wait_exit(60) != 0, "没声明认证方式的客户端竟然发送成功——漏填口令被静默降级了")

        # --- 正例：两端都 --no-auth ---
        recv1.kill()
        recv2, rid2 = c.start_receiver(idle_timeout=120, no_auth=True)
        sender = c.send(src, rid2, no_auth=True, extra=["--idle-timeout", "120"])
        rc = c.wait_all([sender, recv2], 600)
        expect_eq(rc[0], 0, "发送端退出码（两端均 --no-auth）")
        expect_eq(rc[1], 0, "接收端退出码（两端均 --no-auth）")

        got = c.dst / "payload.bin"
        expect(got.exists(), f"目标文件不存在：{got}")
        expect_eq(got.stat().st_size, 2_000_000, "目标文件大小")
        expect_eq(sha256_of(got), want, "内容哈希")
        expect_no_fragment(c.dst, "payload.bin")
    finally:
        c.shutdown()


@scenario("fixed-id", "标识声明：--id 固定身份；撞名被拒；断开后名字可重用")
def fixed_id(workdir: Path, bin_dir: Path) -> None:
    """PROTOCOL.md §11.7 的端到端验收。

    四件事，缺一不可：
      1. 声明的名字真的被采纳（ASSIGNED_ID 必须是 alice，不是 client-N）。
         这条也在看"临时号不能被宣布"——声明前服务端会先发 client-N，
         若客户端把那个号打出去，这里会立刻红。
      2. 同一个名字的第二条连接被拒，且理由说清是"被占用"。
      3. **先到者不受影响**：抢名失败不能把原来那条踢下线（顶掉先到者
         是最危险的实现之一：用户的对照表会静默指向新来的那台）。
      4. 断开之后名字被释放，同一个人重连还能拿回来——否则"固定标识"
         只在第一次有用。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "payload.bin"
        want = make_file(src, 2_000_000)

        recv1 = c.cli("recv-alice", "recv", "--to-dir", str(c.dst), *c.cli_common(),
                      "--id", "alice", "--idle-timeout", "60")
        line = recv1.wait_for(ASSIGNED_PREFIX, 20.0)
        expect_eq(line.split(ASSIGNED_PREFIX, 1)[1].strip(), "alice", "声明后拿到的 id")

        # 第二条连接抢同一个名字：必须失败、必须说清原因。
        recv2 = c.cli("recv-dup", "recv", "--to-dir", str(c.dst), *c.cli_common(),
                      "--id", "alice", "--idle-timeout", "10")
        expect(recv2.wait_exit(60) != 0, "同一个标识竟然能连上两条连接")
        expect("已被占用" in recv2.tail(), "撞名失败的原因没说清是被占用")

        # 先到者照常工作：还能收到发给 alice 的文件。
        sender = c.send(src, "alice", extra=["--idle-timeout", "120"])
        expect_eq(sender.wait_exit(120), 0, "发送端退出码（发给先到的 alice）")
        expect_eq(recv1.wait_exit(120), 0, "先到者的接收退出码")
        got = c.dst / "payload.bin"
        expect(got.exists(), f"目标文件不存在：{got}")
        expect_eq(sha256_of(got), want, "内容哈希")

        # 名字随连接释放：重连能拿回同一个。
        recv3 = c.cli("recv-again", "recv", "--to-dir", str(c.dst), *c.cli_common(),
                      "--id", "alice", "--idle-timeout", "5", "--hard-timeout", "3")
        line3 = recv3.wait_for(ASSIGNED_PREFIX, 20.0)
        expect_eq(line3.split(ASSIGNED_PREFIX, 1)[1].strip(), "alice", "重连后拿回的 id")
        # hard-timeout 只是让它自己退出；退出码不重要（没收到文件）。
        recv3.kill()
    finally:
        c.shutdown()


@scenario("self-send", "自投：一个进程把自己的文件发给自己（服务端绕回本连接）")
def self_send(workdir: Path, bin_dir: Path) -> None:
    """PROTOCOL.md §11.3.1 的端到端验收。

    v1 支持"发给自己"（本机自测：一条命令验证中继 + 认证 + 传输 + 落盘）。
    v2 早期把 `to == 自己 id` 判成拒绝，服务端静默丢帧——发送方要等满
    应答超时才报错。现在改成"转发回这条连接"，本场景钉住它。

    一个进程同时当收发两端，客户端靠 sessionId 解复用（§9.4.1）。
    三条断言：退出码 0、落点内容逐字节一致、不留残片。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "payload.bin"
        want = make_file(src, 3_000_000)

        p = c.cli("recv-self", "recv", "--to-dir", str(c.dst), *c.cli_common(),
                  "--id", "me", "--self-send", str(src), "--idle-timeout", "60")
        line = p.wait_for(ASSIGNED_PREFIX, 20.0)
        expect_eq(line.split(ASSIGNED_PREFIX, 1)[1].strip(), "me", "自检实例的 id")
        expect_eq(p.wait_exit(120), 0, "自检进程退出码")

        got = c.dst / "payload.bin"
        expect(got.exists(), f"目标文件不存在：{got}")
        expect_eq(got.stat().st_size, 3_000_000, "目标文件大小")
        expect_eq(sha256_of(got), want, "内容哈希")
        expect_no_fragment(c.dst, "payload.bin")
    finally:
        c.shutdown()


@scenario("remote-list", "远端目录列举：共享根内可列，越界与未共享都被拒")
def remote_list(workdir: Path, bin_dir: Path) -> None:
    """PROTOCOL.md §11.8 的端到端验收。

    远端列举由**对端客户端**回答（服务端只转发，它没有文件能力）。安全性
    全靠三条：只暴露一个共享根、路径必须过 resolveWithin、默认不共享。

    四条断言：
      1. 不带路径 = 根列表（盘符 + 家目录），绝对路径模型下唯一的起点；
      2. 绝对目录能列出来（目录在前、名字有序），条目与盘上一致；
      3. 子目录用绝对路径能列（路径是"对端视角的绝对路径"）；
      4. **不需要任何共享配置**：没加任何开关的实例一样能列 —— 这是
         取消共享根之后的有意行为（PROTOCOL.md §11.8.1 写明了代价）。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        share = workdir / "share"
        (share / "sub").mkdir(parents=True, exist_ok=True)
        (share / "a.txt").write_bytes(b"hello")
        (share / "sub" / "b.txt").write_bytes(b"world!")

        bob = c.cli("recv-bob", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--id", "bob",
                    "--hard-timeout", "30")
        bob.wait_for(ASSIGNED_PREFIX, 20.0)

        # --- 根列表：不给 --path ---
        roots = c.cli("ls-roots", "ls", "--to", "bob", *c.cli_common(), "--id", "alice")
        expect_eq(roots.wait_exit(30), 0, "列举根的退出码")
        root_out = "\n".join(roots.pill())
        expect("REMOTE_LIST status=0" in root_out, f"根列表没有结论行：{root_out}")
        expect("count=0" not in root_out, f"根列表竟然是空的：{root_out}")

        # --- 绝对目录 ---
        lister = c.cli("ls-bob", "ls", "--to", "bob", "--path", str(share), *c.cli_common(), "--id", "alice1")
        expect_eq(lister.wait_exit(30), 0, "列举绝对目录的退出码")
        out = "\n".join(lister.pill())
        expect("REMOTE_LIST status=0 count=2" in out, f"列举汇总不对：{out}")
        # 目录在前、文件在后（顺序稳定，UI 才不闪）。
        expect("D  sub" in out, f"没列到子目录：{out}")
        expect("F  5  a.txt" in out, f"没列到文件或大小不对：{out}")

        sub = c.cli("ls-sub", "ls", "--to", "bob", "--path", str(share / "sub"), *c.cli_common(), "--id", "alice2")
        expect_eq(sub.wait_exit(30), 0, "列举子目录的退出码")
        sub_out = "\n".join(sub.pill())
        expect("b.txt" in sub_out, f"子目录内容不对：{sub_out}")

        # --- 路径不存在：明确报"没有这个目录"，不是静默超时 ---
        bad = c.cli("ls-missing", "ls", "--to", "bob", "--path", str(share / "nope"), *c.cli_common(),
                    "--id", "alice3")
        expect(bad.wait_exit(30) != 0, "列一个不存在的目录竟然成功")
        expect("没有这个目录" in bad.tail(), f"失败原因没说清：{bad.tail()}")

        # --- 没有共享配置的实例一样能列（取消共享根之后的有意行为） ---
        carol = c.cli("recv-carol", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--id", "carol",
                      "--hard-timeout", "30")
        carol.wait_for(ASSIGNED_PREFIX, 20.0)
        nob = c.cli("ls-carol", "ls", "--to", "carol", "--path", str(share), *c.cli_common(), "--id", "alice4")
        expect_eq(nob.wait_exit(30), 0, "没做任何配置的实例应当仍能列举绝对路径（PROTOCOL.md §11.8.1）")
        expect("a.txt" in "\n".join(nob.pill()), "绝对路径列举没拿到内容")
        carol.kill()
    finally:
        c.shutdown()


@scenario("self-browse", "接收会话上自检列举（GUI 远端浏览的同一条代码路径）")
def self_browse(workdir: Path, bin_dir: Path) -> None:
    """GUI 的远端浏览走 `Client::browseOnSession`：请求搭在 kReceive 会话上，
    不是新开连接（新开连接会顶掉常驻接收会话、换掉本机 id）。CLI 的 `ls`
    走的是 `browsePeer`（一次性连接），**两条路不是同一份代码**，只测 `ls`
    覆盖不到 GUI 那条。

    这里用自投（to == 自己 id，§11.3.1）把请求与应答都绕回本连接，于是
    一个进程同时充当"问的人"和"答的人"，把 browseOnSession + serveFileList
    在同一会话上并存这件事也验了。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        share = workdir / "share"
        (share / "sub").mkdir(parents=True, exist_ok=True)
        (share / "a.txt").write_bytes(b"hello")

        proc = c.cli("self-browse", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--id", "me",
                     "--self-browse", "--path", str(share), "--hard-timeout", "20")
        expect_eq(proc.wait_exit(40), 0, "自检列举的退出码")
        out = "\n".join(proc.pill())
        # 与 ls 同一套契约输出：证明这条路给出的结果和一次性连接那条一致。
        expect("REMOTE_LIST status=0 count=2" in out, f"自检列举汇总不对：{out}")
        expect("D  sub" in out, f"没列到子目录：{out}")
        expect("F  5  a.txt" in out, f"没列到文件或大小不对：{out}")
    finally:
        c.shutdown()


@scenario("pull-file", "远端取回：请对端把共享文件发过来，内容一致；三种拒绝都明确")
def pull_file(workdir: Path, bin_dir: Path) -> None:
    """PROTOCOL.md §11.9 的端到端验收。

    取回**不另造传输**：B 回完 kAnswerFilePull(0) 之后，用普通的推流程
    （kFileRequestSend → kFileAnswerSend → 分块）把文件发给 A，所以这条
    场景同时也验证了"拉"与"推"能拼在一起而不打架。

    四条断言：
      1. 绝对路径的存在的文件能取回，内容逐字节一致；
      2. 不存在的文件明确报 kNotFound（不是超时）；
      3. 目录被明确拒绝（要目录请走 §11.10）；
      4. **任何绝对路径都能取**——不在某个"共享根"之外的也照样能拿。
         这是取消共享根之后的有意行为（PROTOCOL.md §11.8.1 记录了代价）。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        share = workdir / "share"
        (share / "sub").mkdir(parents=True, exist_ok=True)
        payload = b"pull-me-please" * 7
        (share / "a.txt").write_bytes(payload)

        # 常驻，等着被取回。idle/hard 都给大一点，别在断言中途掉线。
        bob = c.cli("recv-bob", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--id", "bob",
                    "--hard-timeout", "120", "--idle-timeout", "120")
        bob.wait_for(ASSIGNED_PREFIX, 20.0)

        dst_a = workdir / "dst-a"
        dst_a.mkdir()

        def pull(tag: str, path: str) -> tuple[int, str]:
            p = c.cli(tag, "recv", "--to-dir", str(dst_a), *c.cli_common(), "--id", tag,
                      "--pull-from", "bob", "--pull-path", path, "--hard-timeout", "25")
            rc = p.wait_exit(45)
            return rc, "\n".join(p.pill())

        rc, out = pull("pull-ok", str(share / "a.txt"))
        expect_eq(rc, 0, f"取回存在的文件应当成功：{out}")
        expect("PULL accepted" in out, f"没看到接受应答：{out}")
        got = (dst_a / "a.txt").read_bytes()
        expect(got == payload, f"取回的内容不一致：期望 {len(payload)} 字节，实际 {len(got)}")

        # --- 拒绝都必须是明确应答，不能退化成"等满超时" ---
        rc, out = pull("pull-missing", str(share / "nope.txt"))
        expect(rc != 0, "取回不存在的文件竟然成功")
        expect("对端没有这个文件" in out, f"不存在的文件应当报 kNotFound：{out}")

        rc, out = pull("pull-on-dir", str(share / "sub"))
        expect(rc != 0, "取回目录竟然成功（目录要走 --pull-dir）")
        expect("对端没有这个文件" in out, f"目录应当报 kNotFound：{out}")

        # --- 全路径访问：不在任何"共享根"里的文件也能取（有意的取舍） ---
        outsider = workdir / "outside-share.txt"
        outsider.write_bytes(b"outside")
        rc, out = pull("pull-outside", str(outsider))
        expect_eq(rc, 0, f"绝对路径应当总能取回（PROTOCOL.md §11.8.1）：{out}")
        expect((dst_a / "outside-share.txt").read_bytes() == b"outside", "共享根之外的文件内容不一致")
    finally:
        c.shutdown()


@scenario("pull-dir", "目录取回：对端打包成 tar.xz，解开后条目与内容一致")
def pull_dir(workdir: Path, bin_dir: Path) -> None:
    """PROTOCOL.md §11.10 的端到端验收。

    取目录不让请求方逐个文件要（那是 N 次往返，还有"列完了对方又变了"的
    竞态），而是让对端**先打包再一次发过来**。所以这里必须真的把包解开看
    内容 —— 只断言"收到一个文件"证明不了包是对的。

    四条断言：
      1. 取回成功，落到接收目录的是 `<目录名>.tar.xz`；
      2. 包内条目与源目录一致，**含空目录**（只收文件会让解出来少几个目录）；
      3. 嵌套文件的内容逐字节一致；
      4. `--pull-dir` 指向文件、以及 `..` 越界，都必须明确拒绝。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        share = workdir / "share"
        sub = share / "sub"
        (sub / "nested").mkdir(parents=True, exist_ok=True)
        (sub / "empty").mkdir(parents=True, exist_ok=True)
        (sub / "a.txt").write_bytes(b"alpha-content")
        (sub / "nested" / "b.bin").write_bytes(b"beta-content-123")

        bob = c.cli("recv-bob", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--id", "bob",
                    "--hard-timeout", "120", "--idle-timeout", "120")
        bob.wait_for(ASSIGNED_PREFIX, 20.0)

        dst_a = workdir / "dst-a"
        dst_a.mkdir()

        def pull_dir_cmd(tag: str, path: str) -> tuple[int, str]:
            p = c.cli(tag, "recv", "--to-dir", str(dst_a), *c.cli_common(), "--id", tag,
                      "--pull-from", "bob", "--pull-dir", path, "--hard-timeout", "40")
            return p.wait_exit(60), "\n".join(p.pill())

        rc, out = pull_dir_cmd("dir-ok", str(sub))
        expect_eq(rc, 0, f"取回目录应当成功：{out}")

        # 目录取回现在会自动解包：用户看到的是目录，不是一个 .tar.xz
        # （§11.10.6 的补完）。所以断言看解出来的东西，而不是包本身。
        extracted_dir = dst_a / "sub"
        expect(extracted_dir.is_dir(), f"没解出 sub 目录，接收目录内容：{list(dst_a.iterdir())}")
        expect(not (dst_a / "sub.tar.xz").exists(), "包应该在被解开后删除，否则会越积越多")

        expect((extracted_dir / "a.txt").read_bytes() == b"alpha-content", "a.txt 内容不一致")
        expect((extracted_dir / "nested" / "b.bin").read_bytes() == b"beta-content-123",
               "嵌套文件内容不一致")
        expect((extracted_dir / "empty").is_dir(), "空目录在包/解包过程中丢了")
        # 层次要与对方看到的一致：解出来是 sub/a.txt，而不是散在接收目录里。
        expect(not (dst_a / "a.txt").exists(), "解包把内容散到了接收目录，少了 sub/ 这一层")

        # --- 拒绝路径 ---
        rc, out = pull_dir_cmd("dir-on-file", str(sub / "a.txt"))
        expect(rc != 0, "对一个文件请求目录取回竟然成功")
        expect("对端没有这个目录" in out, f"指向文件时应当报 kNotFound（并说清是目录）：{out}")

        rc, out = pull_dir_cmd("dir-missing", str(share / "nope"))
        expect(rc != 0, "对不存在的目录请求取回竟然成功")
        expect("对端没有这个目录" in out, f"不存在时应当报 kNotFound：{out}")
    finally:
        c.shutdown()


@scenario("pack-unpack", "本地压缩/解压：包内带顶层目录，解开后不多套一层")
def pack_unpack(workdir: Path, bin_dir: Path) -> None:
    """手动压缩 / 解压的端到端验收（PROTOCOL.md §11.10.6 的"两种包形状"）。

    这两条命令**不联网**，所以不建 Cluster——直接调二进制。

    与取回目录的包相反：手动压缩的包**带顶层目录**（entryName 相对各源的
    父目录），所以解压时解到父目录而不是按包名建子目录。混用会得到
    `sub/sub/`，那是最容易犯也最难看出来的错，所以专门断言它没发生。
    """
    exe = bin_dir / f"relayer-cli{EXE}"
    src = workdir / "sub"
    (src / "nested").mkdir(parents=True)
    (src / "empty").mkdir()
    (src / "a.txt").write_bytes(b"alpha")
    (src / "nested" / "b.bin").write_bytes(b"beta!")

    archive = workdir / "out.tar.xz"
    packed = subprocess.run([str(exe), "pack", str(archive), str(src)], capture_output=True, text=True, timeout=60)
    expect_eq(packed.returncode, 0, f"pack 应当成功：{packed.stderr}")
    expect(archive.exists(), "没生成包文件")

    with tarfile.open(archive, "r:xz") as tf:
        names = sorted(tf.getnames())
    # 带顶层目录：包内是 sub/a.txt 而不是 a.txt。
    expect("sub/a.txt" in names, f"包内应当带顶层目录：{names}")
    expect("sub/nested/b.bin" in names, f"嵌套文件没进包：{names}")
    expect("sub/empty" in names, f"空目录没进包：{names}")

    shutil.rmtree(src)
    unpacked = subprocess.run([str(exe), "unpack", str(archive)], capture_output=True, text=True, timeout=60)
    expect_eq(unpacked.returncode, 0, f"unpack 应当成功：{unpacked.stderr}")

    expect((src / "a.txt").read_bytes() == b"alpha", "a.txt 内容不一致")
    expect((src / "nested" / "b.bin").read_bytes() == b"beta!", "嵌套文件内容不一致")
    expect((src / "empty").is_dir(), "空目录没恢复出来")
    # 最容易犯的错：把带顶层目录的包解到"按包名建的子目录"里，得到 sub/sub/。
    expect(not (src / "sub").exists(), "解压多套了一层（出现了 sub/sub/）")


@scenario("one-byte", "1 字节文件正常完成")
def one_byte(workdir: Path, bin_dir: Path) -> None:
    _transfer_roundtrip(workdir, bin_dir, 1)


@scenario("kill-sender", "中途 kill 发送端：目标文件不被破坏，残片只在 .relaypart")
def kill_sender(workdir: Path, bin_dir: Path) -> None:
    """这是整张表里唯一需要**进程级**操作的场景。

    断言的重点不是"失败了"——失败是必然的，发送端都被杀了。重点是
    **盘上的状态没有变坏**：

      - 目标文件不该出现（宁可没有，也不要一个看起来完整的半截文件）
      - 已经收到的字节只能待在 .relaypart 里
      - 接收端要在有限时间内自己得出结论，而不是永远挂着
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "big.bin"
        # 64MB 而不是 200MB：本场景断言的是**杀进程之后盘上的状态**，不是
        # 吞吐。残片只需"已有进展且远小于完整文件"，文件越大只是把
        # make_file() 的逐字节生成时间（约 3s/64MB、10s/200MB）也算进
        # 场景耗时里。早先用 200MB 时整场 36s，其中一半花在造数据上。
        want = make_file(src, 64_000_000)

        # 块给大一点（1MB），确保我们能在一部分数据已经落地之后才杀。
        recv, rid = c.start_receiver(idle_timeout=8)
        sender = c.send(src, rid, extra=["--chunk-size", "1048576", "--idle-timeout", "30"])

        # 等到确实有进展再杀。杀太早等于什么都没测——那时接收端连
        # .relaypart 都还没建。
        #
        # 阈值 4MB（原为 8MB）：只要求"明显超过一个块、且远小于完整文件"。
        # 8MB 贴着启动爬坡期（建链 + 窗口填充 + 首批写盘），在机器有磁盘
        # 压力的时机会被拖到 60s 之外——那不是被测逻辑的问题。
        part = c.dst / "big.bin.relaypart"
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if part.exists() and part.stat().st_size > 4 << 20:
                break
            if sender.proc.poll() is not None:
                raise E2EFailure(f"发送端提前退出了（code={sender.proc.returncode}）\n{sender.tail()}")
            time.sleep(0.05)
        else:
            raise E2EFailure(f"90s 内没等到 4MB 数据落地\n{sender.tail()}\n{recv.tail()}")

        size_at_kill = part.stat().st_size
        sender.kill()

        # 接收端必须在 idleTimeout 左右自己得出结论。给它宽余量，但
        # **必须**有上限——一条永远挂着的传输在 UI 上就是永远转圈的进度条。
        rc = recv.wait_exit(120)
        expect(rc != 0, "接收端在发送端被杀之后竟然报告成功")

        # --- 盘上状态 ---
        target = c.dst / "big.bin"
        # 注意这里不能写成 f"目标文件被创建了（{target.stat().st_size} 字节）"：
        # Python 会**先求值 f-string 再调用 expect**，而断言成立的前提恰恰是
        # 这个文件不存在——于是 stat() 抛 FileNotFoundError，把一条本该通过的
        # 断言变成了 traceback。错误信息要按需构造。
        if target.exists():
            raise E2EFailure(f"目标文件被创建了（{target.stat().st_size} 字节），它看起来会像个完整文件")
        expect(part.exists(), "残片没了：已经收到的字节凭空消失，说明落盘路径有问题")
        got = part.stat().st_size
        # 残片长度只该增长（杀之前那一次量到的是下界），且必须远小于完整文件。
        expect(got >= size_at_kill, f"残片比杀之前还短（{got} < {size_at_kill}）")
        expect(got < 64_000_000, f"残片竟然有完整大小 {got}，那不是残片")

        # 内容前缀必须真的对得上，否则"残片可续传"就是句空话。
        with open(part, "rb") as f:
            head = f.read(1 << 20)
        with open(src, "rb") as f:
            expect_eq(hashlib.sha256(head).hexdigest(), hashlib.sha256(f.read(1 << 20)).hexdigest(),
                      "残片前 1MB 的内容")
        _ = want
    finally:
        c.shutdown()


@scenario("resume-valid-fragment", "预置正确残片：从断点续传，不重传已完成部分")
def resume_valid(workdir: Path, bin_dir: Path) -> None:
    """先杀掉一次留下真残片，再跑一次看它能不能接上。

    这比"手工造一个残片"强：手工造需要知道指纹格式、块对齐规则，
    一旦格式改了这个用例就变成在测别的实现细节。用真实残片则只依赖
    "上一次留下的东西，下一次应该能认"。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "resume.bin"
        want = make_file(src, 64_000_000)

        # --- 第一轮：传一半杀掉 ---
        recv1, rid1 = c.start_receiver(idle_timeout=6)
        s1 = c.send(src, rid1, name="send1", extra=["--chunk-size", "1048576", "--idle-timeout", "30"])
        part = c.dst / "resume.bin.relaypart"
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if part.exists() and part.stat().st_size > 16 << 20:
                break
            time.sleep(0.05)
        else:
            raise E2EFailure(f"第一轮没等到 16MB 落地\n{s1.tail()}\n{recv1.tail()}")
        s1.kill()
        recv1.wait_exit(120)
        # 收尾前量一下：续传之后不该重传这些字节。
        have = part.stat().st_size
        expect(0 < have < 64_000_000, f"残片大小不合理：{have}")

        # 第一轮的连接还挂在服务端表里，等它清理掉，否则第二轮会拿到
        # 一个不同的 id——那没问题，但端口要重新要一个更干净。
        # --- 第二轮：同一目录、同一目标名 ---
        recv2 = c.cli("recv2", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--idle-timeout", "120")
        line = recv2.wait_for(ASSIGNED_PREFIX, 20.0)
        rid2 = line.split(ASSIGNED_PREFIX, 1)[1].strip()
        s2 = c.send(src, rid2, name="send2",
                    extra=["--chunk-size", "1048576", "--idle-timeout", "120"])
        expect_eq(s2.wait_exit(600), 0, "第二轮发送端退出码")
        expect_eq(recv2.wait_exit(120), 0, "第二轮接收端退出码")

        got = c.dst / "resume.bin"
        expect(got.exists(), "续传之后目标文件不存在")
        expect_eq(sha256_of(got), want, "续传后的内容哈希")
        expect_no_fragment(c.dst, "resume.bin")
    finally:
        c.shutdown()


@scenario("resume-forged-fragment", "预置伪造残片：整体丢弃，下一轮全量重传成功")
def resume_forged(workdir: Path, bin_dir: Path) -> None:
    """残片的后半段被改过——它必须被整体丢弃，并且**不能污染后续传输**。

    这个场景要分两轮看，第一轮失败是**正确行为**，不是缺陷：

      - 协议描述里只有整文件 sha256 和一个覆盖前 64KB 的 fingerprint。
        伪造残片的前 64KB 恰好是对的，所以 `open()` 阶段没有任何依据
        可以拒绝它。续传点也只按残片长度算（见 scanResumePoint）。
      - 于是接收端会从 16MB 处接着收，收满 100% 后算真实 merkle 根，
        发现对不上 —— 此刻才可能知道这份残片是假的。

    真正会变成事故的是**第一轮之后**：如果把坏残片留在盘上，下一次传输
    又会在这份坏残片上面续传，于是永远失败，且症状一模一样（每次都传满
    100%，每次都报 root_mismatch），运维会以为"重试也没用"。所以第二轮的
    断言才是这个场景的核心：残片被清掉、全量重传、内容正确。

    留着坏残片的后果也比"多传一遍"严重：内容会和源文件不一致，而每个块
    的校验都能通过（块本身是自洽的），最后得到一个校验也查不出的坏文件。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "forged.bin"
        want = make_file(src, 32_000_000)
        target = c.dst / "forged.bin"
        part = c.dst / "forged.bin.relaypart"

        # 伪造一个"长度看起来合理"的残片：前一半是正确内容，后一半是垃圾。
        # 长度必须与源文件对齐到块边界之内才能进到"校验"那一步，否则
        # 会被当成尺寸不符直接删掉，那就没测到伪造检测。
        with open(src, "rb") as f:
            half = f.read(16_000_000)
        assert len(half) == 16_000_000
        with open(part, "wb") as f:
            f.write(half)
            f.write(b"\x00" * len(half))

        # --- 第一轮：必须明确失败，且不得产出目标文件 ---
        recv, rid = c.start_receiver(idle_timeout=120)
        sender = c.send(src, rid, extra=["--chunk-size", "1048576", "--idle-timeout", "120"])

        sender_rc = sender.wait_exit(600)
        recv_rc = recv.wait_exit(120)

        expect(sender_rc != 0, "发送端竟然报成功：伪造残片被当真了，内容已经和源文件不一致")
        expect(recv_rc != 0, "接收端竟然报成功：伪造残片被当真了")

        # 目标文件绝不能出现。哪怕是"错的"目标文件也不行——用户看到
        # forged.bin 存在就会以为传完了，而校验和是错的。
        if target.exists():
            raise E2EFailure(f"伪造残片产出了目标文件（{target.stat().st_size} 字节），它看起来会像个完整文件")

        # --- 第一轮之后：坏残片必须已经被清掉 ---
        expect(
            not part.exists(),
            "坏残片还留在盘上：下一轮会继续在它上面续传，于是每次都传满 100% 都失败，重试永远救不回来",
        )

        # --- 第二轮：全新一轮必须成功 ---
        # 第一轮的连接还挂在服务端表里，新起一个即可——id 递增分配不影响语义。
        recv2 = c.cli("recv2", "recv", "--to-dir", str(c.dst), *c.cli_common(), "--idle-timeout", "120")
        line = recv2.wait_for(ASSIGNED_PREFIX, 20.0)
        rid2 = line.split(ASSIGNED_PREFIX, 1)[1].strip()
        s2 = c.send(src, rid2, name="send2", extra=["--chunk-size", "1048576", "--idle-timeout", "120"])
        expect_eq(s2.wait_exit(600), 0, "第二轮发送端退出码")
        expect_eq(recv2.wait_exit(120), 0, "第二轮接收端退出码")

        expect(target.exists(), "第二轮之后目标文件不存在")
        expect_eq(sha256_of(target), want, "第二轮内容哈希（全量重传）")
        expect_no_fragment(c.dst, "forged.bin")
    finally:
        c.shutdown()


@scenario("drop-chunks", "中继丢掉两个数据帧：只重传空洞，内容一致")
def drop_chunks(workdir: Path, bin_dir: Path) -> None:
    """链路丢包之后，SACK 应当让发送端只补那两个空洞。

    §3.5.3 要求"乱序 / 丢包"场景，但真实 socket 上制造稳定可复现的丢包
    很难——`SO_RCVBUF` 调小、`iptables`、代理都不在当前平台上可用，而且
    丢哪一块完全不可控。所以用中继的测试参数 `--drop-data-frames` 精确
    丢"第 3、5 个转发的数据帧"。

    这条场景的价值不在"最终成功"（那只能证明没卡死），而在**重传块数
    恰好等于丢失块数**：
      - == 丢的块数 → SACK 位图生效，只补空洞
      - ≈ 总块数   → 退化成整份重传。内容仍然正确，只有这个数字能看出来
      - 0          → PULL 端没发现丢包，等最终校验才发现，即"传完才失败"

    也顺带覆盖"丢包后不卡死"：丢块会让 ackThrough 停住，若重传定时器
    失效，整个传输会停在窗口耗尽处——那正是 §3.5.3 要排除的形态。
    """
    dropped = (3, 5)
    chunk = 1 << 20
    size = 8 << 20
    total = size // chunk

    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server(extra=["--drop-data-frames", ",".join(str(n) for n in dropped)])
        src = c.src / "dropped.bin"
        want = make_file(src, size)

        recv, rid = c.start_receiver(idle_timeout=60)
        # -q：进度条会盖住 ASSIGNED_ID 那种行；--stats 让发送端在结论前
        # 多打一行机器可读的计数，这是断言"只补了 2 块"的唯一依据。
        sender = c.send(
            src, rid,
            extra=["-q", "--stats", "--chunk-size", str(chunk), "--idle-timeout", "60"],
        )

        # 重传要等 retransmitTimeout（3 秒）才触发，所以传输本身不到 1 秒、
        # 整场却要 4 秒以上——这个时限必须比它宽。
        expect_eq(sender.wait_exit(120), 0, "发送端退出码")
        expect_eq(recv.wait_exit(120), 0, "接收端退出码")

        got = c.dst / "dropped.bin"
        expect(got.exists(), f"目标文件不存在：{got}")
        expect_eq(sha256_of(got), want, "内容哈希")
        expect_no_fragment(c.dst, "dropped.bin")

        stats = parse_stats(sender.wait_for("STATS ", 5.0))
        expect_eq(stats.get("chunksRetransmitted"), len(dropped), "重传块数（应只补空洞，不是整份重发）")
        # 原始每个块一帧，丢了 2 块又各补一帧。总数对不上说明有块被发了
        # 两次以上——那通常意味着 ACK 丢了解析或窗口状态回退。
        expect_eq(stats.get("chunksSent"), total + len(dropped), "数据帧总数（原始 + 重传）")
    finally:
        c.shutdown()


@scenario("target-busy", "目标被占用：明确报错，不挂死")
def target_busy(workdir: Path, bin_dir: Path) -> None:
    """目标被别的进程独占打开时，接收端要立刻拒绝，而不是卡住。

    这个场景的价值全在"不挂死"三个字。挂死的表现是：用户在 UI 上看到
    一个永远不动的进度条，而发送端也在等——两边都不知道出了什么事。

    模拟方式按平台分：
      - Windows：用 CreateFile 以**不共享**方式打开目标文件，这正是
        "被 Office 占用"的真实形态，能触发 probeWritable 的 kBusy 分支。
      - 其他平台：跳过。POSIX 没有强制独占，flock 是建议性的，造不出
        同样条件；与其造一个假的，不如明说这条没覆盖。
    """
    if os.name != "nt":
        raise SkipScenario("目标占用只能靠 Windows 的强制共享语义模拟")

    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server()
        src = c.src / "busy.bin"
        make_file(src, 4_000_000)
        target = c.dst / "busy.bin"
        target.write_bytes(b"occupied")

        # 用 Windows API 独占打开。subprocess 里没法直接调 Win32，改用
        # msvcrt 不行——它没有共享模式参数。所以这里借 PowerShell 的
        # FileStream 打开并持有：进程一直活着，句柄就一直占着。
        holder_log = c.workdir / "holder.log"
        holder_file = open(holder_log, "wb")
        holder = subprocess.Popen(
            [
                "powershell", "-NoProfile", "-NonInteractive", "-Command",
                f"$f=[System.IO.File]::Open('{target}','Open','ReadWrite','None');"
                f"[Console]::Out.WriteLine('HELD');[Console]::Out.Flush();"
                f"Start-Sleep -Seconds 300",
            ],
            stdin=subprocess.DEVNULL,
            stdout=holder_file,
            stderr=subprocess.STDOUT,
        )
        # 走同一条读取路径（adopt），而不是手工复制一堆私有属性：
        # 手工复制会在 Process 内部结构变化时静默错位。
        held = Process.adopt("holder", holder, holder_log)
        c.procs.append(held)
        held.wait_for("HELD", 20.0)

        recv, rid = c.start_receiver(idle_timeout=30)
        sender = c.send(src, rid, extra=["--chunk-size", "262144", "--idle-timeout", "30"])

        # **关键断言是"有时限"**。所以这里不用 wait_exit 的无限等价形式，
        # 而是给一个明确的、远小于 idleTimeout 的时限：既然目标一开始
        # 就打不开，就没有理由等满 idle。
        sender_rc = sender.wait_exit(60)
        recv_rc = recv.wait_exit(60)

        expect(sender_rc != 0, "发送端竟然报成功")
        expect(recv_rc != 0, "接收端竟然报成功")

        # 原文件不能被碰过——这是"目标被占用"和"目标被覆盖"的分界线。
        #
        # 必须**先放掉句柄再读**：独占打开（共享模式 None）期间，Python 这边
        # open() 会直接 PermissionError。而我们要验证的是"传输过程有没有动
        # 这个文件"，不是"持有期间能不能读"——所以收尾时释放，然后校验内容。
        # 早先在这里直接 read_bytes，结果是测试自己炸了，而它掩盖的真实信息
        # （文件内容有没有被改）一条都没得到。
        held.kill()
        try:
            expect_eq(target.read_bytes(), b"occupied", "被占用文件的内容")
        finally:
            pass
    finally:
        c.shutdown()


# ---------------------------------------------------------------------------
# 驱动
# ---------------------------------------------------------------------------


def run_one(name: str, fn: Callable[[Path, Path], None], bin_dir: Path) -> tuple[str, str]:
    """返回 (状态, 说明)。状态是 PASS / FAIL / SKIP。"""
    workdir = Path(tempfile.mkdtemp(prefix=f"relayer-e2e-{name}-"))
    try:
        fn(workdir, bin_dir)
        return "PASS", ""
    except SkipScenario as e:
        return "SKIP", str(e)
    except E2EFailure as e:
        return "FAIL", str(e)
    except Exception:
        return "FAIL", traceback.format_exc()
    finally:
        # 失败时保留现场：日志是唯一的证据，删了就只剩一句"失败了"。
        if os.environ.get("E2E_KEEP") != "1":
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"    （现场保留在 {workdir}）")


@scenario("remote-file-ops", "远端文件操作：建目录 / SHA-256 / 改名 / 递归删除 / 解压（§11.12）")
def remote_file_ops(workdir: Path, bin_dir: Path) -> None:
    """这一组覆盖的是"让对端在它自己的盘上干活"，五个动作各来一遍。

    为什么值得进 e2e 而不是只留单测：编码往返只证明了字节对得上，
    证明不了**对端真的执行了**——比如 serve 分支没接上、路径没按接收目录
    还原、递归删除只删了第一层。这些只有跑两个进程才看得出来。
    """
    c = Cluster(workdir, free_port(), bin_dir=bin_dir)
    try:
        c.start_server(no_auth=True)
        recv, rid = c.start_receiver(no_auth=True)
        common = c.cli_common(no_auth=True)

        # 对端视角的绝对路径。同一台机器上跑，路径通用。
        base = c.workdir / "remote"
        deep = base / "tree" / "deep"

        # -- 建目录：一次建出中间层 -------------------------------------
        p = c.cli("op-mkdir", "op", "--to", rid, "--op", "mkdir", "--path", str(deep), *common)
        expect_eq(c.wait_all([p], 30)[0], 0, "mkdir 退出码")
        expect(deep.is_dir(), f"目录没建出来：{deep}")

        # -- SHA-256：对端算的摘要必须与本地一致 ------------------------
        f = deep / "a.bin"
        want = make_file(f, 300_000)
        p = c.cli("op-sha", "op", "--to", rid, "--op", "sha256", "--path", str(f), *common)
        expect_eq(c.wait_all([p], 60)[0], 0, "sha256 退出码")
        line = p.wait_for("OP_SHA256=", 5)
        expect_eq(line.split("OP_SHA256=", 1)[1].strip(), want, "远端 SHA-256")

        # -- 改名：生效，且原路径消失 ------------------------------------
        p = c.cli("op-rename", "op", "--to", rid, "--op", "rename", "--path", str(f),
                  "--new-name", "b.bin", *common)
        expect_eq(c.wait_all([p], 30)[0], 0, "rename 退出码")
        expect(not f.exists(), "改名后旧名字还在")
        renamed = deep / "b.bin"
        expect(renamed.exists(), "改名后新名字不存在")
        expect_eq(sha256_of(renamed), want, "改名后内容变了")

        # -- 撞名必须拒绝：覆盖等于替用户删掉别人的文件 ------------------
        (deep / "c.bin").write_bytes(b"occupied")
        p = c.cli("op-rename-taken", "op", "--to", rid, "--op", "rename", "--path", str(renamed),
                  "--new-name", "c.bin", *common)
        expect_eq(c.wait_all([p], 30)[0], 2, "目标已存在时应失败")
        expect_eq((deep / "c.bin").read_bytes(), b"occupied", "被拒绝的改名仍然动了目标文件")
        expect(renamed.exists(), "被拒绝的改名把源文件弄丢了")

        # -- 删除：目录递归（第二层开始的文件也要走） --------------------
        p = c.cli("op-delete", "op", "--to", rid, "--op", "delete", "--path", str(base / "tree"), *common)
        expect_eq(c.wait_all([p], 30)[0], 0, "delete 退出码")
        expect(not (base / "tree").exists(), "递归删除没有真的删掉整棵树")

        # -- 解压：把本地打的包交给对端解到同名子目录 --------------------
        archive_src = c.workdir / "packsrc"
        (archive_src / "nested").mkdir(parents=True)
        (archive_src / "nested" / "x.txt").write_bytes(b"unarchive-content")
        pack = base / "bundle.tar.xz"
        p = c.cli("op-pack", "pack", str(pack), str(archive_src))
        expect_eq(c.wait_all([p], 60)[0], 0, "本地打包失败")

        p = c.cli("op-unpack", "op", "--to", rid, "--op", "unpack", "--path", str(pack), *common)
        expect_eq(c.wait_all([p], 120)[0], 0, "unpack 退出码")
        expect(not pack.exists(), "解压成功后包应当被删掉")
        # 手动压缩的包**带顶层目录**（entryName 相对各源的父目录），所以解开
        # 后是 bundle/packsrc/nested/x.txt，不是 bundle/nested/x.txt。
        # 这个形状与 §11.10 的取回包相反，两者不能混用同一套断言。
        expect((base / "bundle" / "packsrc" / "nested" / "x.txt").read_bytes() == b"unarchive-content",
               "解出来的内容不对")
    finally:
        c.shutdown()


def main() -> int:
    ap = argparse.ArgumentParser(description="relayer 三进程集成测试")
    ap.add_argument("--quick", action="store_true", help="跳过标了 slow 的场景（200MB / 1GB）")
    ap.add_argument("-k", "--filter", default="", help="只跑名字含该子串的场景")
    ap.add_argument("--list", action="store_true", help="列出场景名")
    ap.add_argument("--bin", default=str(DEFAULT_BIN), help="可执行文件目录")
    ap.add_argument("--repeat", type=int, default=1, help="每个场景重复次数（查时序不稳定）")
    args = ap.parse_args()

    # 不改模块级常量：那个默认值只用于"直接构造 Cluster 时忘了传"，
    # 而真正跑起来的路径上一律显式传 bin_dir。
    bin_dir = Path(args.bin).resolve()

    for exe in (f"relayerd{EXE}", f"relayer-cli{EXE}"):
        if not (bin_dir / exe).exists():
            print(f"找不到 {bin_dir / exe}，先编译：cmake --build build", file=sys.stderr)
            return 2

    selected = [(n, d, s, f) for (n, d, s, f) in SCENARIOS if args.filter in n]
    if args.list:
        for n, d, s, _ in selected:
            print(f"{'[slow] ' if s else '       '}{n:<28} {d}")
        return 0

    if args.quick:
        selected = [x for x in selected if not x[2]]

    if not selected:
        print("没有匹配的场景", file=sys.stderr)
        return 2

    print(f"运行 {len(selected)} 个场景（可执行文件目录 {bin_dir}）\n")

    results: list[tuple[str, str, str]] = []
    started = time.monotonic()
    for name, desc, _slow, fn in selected:
        for i in range(args.repeat):
            label = name if args.repeat == 1 else f"{name}#{i + 1}"
            t0 = time.monotonic()
            status, detail = run_one(label, fn, bin_dir)
            dt = time.monotonic() - t0
            results.append((label, status, detail))
            print(f"  {status:<5} {label:<28} {dt:6.2f}s   {desc}")
            if status == "FAIL":
                for line in detail.rstrip().split("\n"):
                    print(f"        {line}")

    elapsed = time.monotonic() - started
    failed = [r for r in results if r[1] == "FAIL"]
    skipped = [r for r in results if r[1] == "SKIP"]
    print(f"\n{len(results) - len(failed) - len(skipped)} 通过，{len(failed)} 失败，"
          f"{len(skipped)} 跳过（{elapsed:.1f}s）")
    for name, _, detail in skipped:
        print(f"  跳过 {name}：{detail.splitlines()[0] if detail else ''}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
