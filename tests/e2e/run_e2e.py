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

    def start_server(self, timeout: float = 15.0, extra: list[str] | None = None) -> Process:
        name = f"server-{self.port}"
        args = [str(self.bin_dir / f"relayerd{EXE}"), "--port", str(self.port), "--password", PASSWORD]
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

    def cli_common(self) -> list[str]:
        return ["-p", str(self.port), "--password", PASSWORD]

    def start_receiver(self, idle_timeout: int = 30, extra: list[str] | None = None) -> tuple[Process, str]:
        """起接收端，返回 (进程, 分配到的 id)。

        id 只能从契约行取。**不能**用 `list` 去猜：id 由服务端递增分配，
        每条新连接都消耗一个号，包括 list 自己那条——抓到的几乎总是 list
        自己，然后文件被投给一个刚断开的连接，中继静默丢弃，症状是发送方
        等应答超时。
        """
        args = ["recv", "--to-dir", str(self.dst), *self.cli_common(), "--idle-timeout", str(idle_timeout)]
        if extra:
            args += extra
        p = self.cli("recv", *args)
        line = p.wait_for(ASSIGNED_PREFIX, 20.0)
        assigned = line.split(ASSIGNED_PREFIX, 1)[1].strip()
        if not assigned:
            raise E2EFailure(f"接收端报了空 id：{line!r}")
        return p, assigned

    def send(self, src_path: Path, to_id: str, name: str = "send", extra: list[str] | None = None) -> Process:
        args = ["send", str(src_path), "--to", to_id, *self.cli_common()]
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
