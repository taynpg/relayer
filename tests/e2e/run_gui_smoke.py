#!/usr/bin/env python3
"""GUI 桥接层的冒烟测试（不需要显示器、不需要点鼠标）。

跑的是一个无窗口的可执行文件 `gui_smoke`，它用 `app::CoreService` 真连一次
中继，然后**自投**列举一次自己的根目录。覆盖的链路是：

    startReceive → 控制连接认证 → 数据连接建立并 claim 名字
                 → 控制连接上的并发请求 → 应答按请求号配对

为什么这条值得单独进 CTest：GUI 那一层（`CoreService` 的线程边界、Qt 字符串
转换、事件投递）在此之前**一条自动化用例都没有**。协议与传输有 400 多个单元
用例、21 个三进程场景，而"界面到底连不连得上"只能靠手点——那意味着改了
`CoreService` 之后没有任何东西会告诉你坏了。

只依赖标准库：不引 pytest，与 run_e2e.py 一致。
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def wait_listen(port: int, timeout: float = 20.0) -> None:
    """等中继真的开始监听。

    不能只 sleep：机器忙的时候 200ms 都可能不够，而"连不上"在这种测试里
    看起来会像客户端的问题。
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.3)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise SystemExit(f"中继在 {timeout:.0f}s 内没开始监听 {port}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="可执行文件目录")
    args = ap.parse_args()

    bin_dir = Path(args.bin).resolve()
    server = bin_dir / "relayerd.exe"
    smoke = bin_dir / "gui_smoke.exe"
    if not server.exists():
        server = bin_dir / "relayerd"
    if not smoke.exists():
        smoke = bin_dir / "gui_smoke"
    for p in (server, smoke):
        if not p.exists():
            print(f"缺少可执行文件：{p}", file=sys.stderr)
            return 2

    tmp = Path(tempfile.mkdtemp(prefix="relayer-gui-smoke-"))
    recv = tmp / "recv"
    recv.mkdir()
    port = free_port()

    server_log = open(tmp / "server.log", "wb")
    proc = subprocess.Popen(
        [str(server), "--port", str(port), "--allow-no-auth"],
        stdout=server_log,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
    )
    try:
        wait_listen(port)
        r = subprocess.run(
            [str(smoke), "--port", str(port), "--dir", str(recv), "--id", "gui-smoke"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        out = (r.stdout or "") + (r.stderr or "")
        print(out.replace("\r\n", "\n").strip())

        if r.returncode != 0:
            print(f"gui_smoke 退出码 {r.returncode}", file=sys.stderr)
            return 1
        # 断言放在这里而不是只信退出码：那个程序有多个 exit 点，退出码
        # 只能说明"没走异常分支"，不能说明"真的列举成功了"。
        if "SMOKE_ASSIGNED=gui-smoke" not in out:
            print("没拿到声明后的身份（认证或 claim 失败）", file=sys.stderr)
            return 1
        if "SMOKE_BROWSE ok=1" not in out:
            print("列举没有成功", file=sys.stderr)
            return 1
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        server_log.close()


if __name__ == "__main__":
    sys.exit(main())
