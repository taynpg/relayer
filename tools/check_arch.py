#!/usr/bin/env python3
"""架构约束检查。

这个脚本把 docs/DESIGN.md §2.2 的"依赖方向"变成可执行的检查，
任何一个违反都会让 CI 失败。

规则
----
R1  core/ 不得包含 Qt 头文件（保持零 Qt，服务端才能不链接 Qt）
R2  core/ 不得包含 filestore 的具体实现，只能包含 IFileStore.h
R3  filestore/ 不得包含 core/ 的任何头文件
R4  server/ 不得包含 Qt 头文件，也不得包含 filestore/
R5  app/ 以外的地方不得包含 Qt 头文件
R6  common/ 只能依赖标准库与第三方库，不得包含 filestore/ core/ app/ server/ cli/

用法
----
    python3 tools/check_arch.py            # 检查，违反则退出码 1
    python3 tools/check_arch.py --list     # 只列出扫描到的文件
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".c"}

QT_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](Q[A-Z][A-Za-z0-9_]*|Qt[A-Za-z0-9_/]*)(\.h)?[>"]')
# Q_OBJECT / Q_PROPERTY 之类的宏同样意味着依赖了 moc
QT_MACRO = re.compile(r"^\s*(Q_OBJECT|Q_GADGET|Q_PROPERTY|Q_INVOKABLE)\b")


def iter_sources(*parts: str):
    for part in parts:
        base = ROOT / part
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def read_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:  # pragma: no cover - 只在磁盘异常时触发
        print(f"warning: 无法读取 {rel(path)}: {exc}", file=sys.stderr)
        return []


def check_qt_free(paths, rule: str, violations: list[str]) -> None:
    for path in paths:
        for lineno, line in enumerate(read_lines(path), 1):
            if QT_INCLUDE.match(line) or QT_MACRO.match(line):
                violations.append(f"{rule}: {rel(path)}:{lineno}: {line.strip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="只列出扫描到的源文件，不做检查")
    args = parser.parse_args()

    if args.list:
        for path in sorted(iter_sources("core", "filestore", "app", "server", "cli", "tests")):
            print(rel(path))
        return 0

    violations: list[str] = []

    common_files = list(iter_sources("common"))
    core_files = list(iter_sources("core"))
    filestore_files = list(iter_sources("filestore"))
    server_files = list(iter_sources("server"))
    cli_files = list(iter_sources("cli"))
    tests_files = list(iter_sources("tests"))

    # R1: core 必须零 Qt
    check_qt_free(core_files, "R1 core-must-be-qt-free", violations)

    # R2: core 只能用 IFileStore.h
    # relayer/common/ 是更底层的模块，core 依赖它是允许的（所以不在禁止之列）。
    for path in core_files:
        for lineno, line in enumerate(read_lines(path), 1):
            m = re.match(r'^\s*#\s*include\s*[<"](.+?)[>"]', line)
            if not m:
                continue
            inc = m.group(1).replace("\\", "/")
            if inc.startswith("relayer/filestore/") and not inc.endswith("IFileStore.h"):
                violations.append(f"R2 core-may-only-see-IFileStore: {rel(path)}:{lineno}: {inc}")
            if inc.startswith("../") and "filestore" in inc and not inc.endswith("IFileStore.h"):
                violations.append(f"R2 core-may-only-see-IFileStore: {rel(path)}:{lineno}: {inc}")

    # R3: filestore 不得反过来依赖 core
    for path in filestore_files:
        for lineno, line in enumerate(read_lines(path), 1):
            if re.search(r'#\s*include\s*[<"](relayer/)?core/', line):
                violations.append(f"R3 filestore-must-not-see-core: {rel(path)}:{lineno}")

    # R4: server 零 Qt、零 filestore
    check_qt_free(server_files, "R4 server-must-be-qt-free", violations)
    for path in server_files:
        for lineno, line in enumerate(read_lines(path), 1):
            if re.search(r'#\s*include\s*[<"](relayer/)?filestore/', line):
                violations.append(f"R4 server-must-not-see-filestore: {rel(path)}:{lineno}")

    # R5: 只有 app 能碰 Qt
    check_qt_free(cli_files, "R5 only-app-may-use-qt", violations)
    check_qt_free(tests_files, "R5 only-app-may-use-qt", violations)

    # R6: common 是最底层，只能往上被依赖，不能自己往上依赖
    check_qt_free(common_files, "R6 common-must-be-qt-free", violations)
    for path in common_files:
        for lineno, line in enumerate(read_lines(path), 1):
            if re.search(r'#\s*include\s*[<"](relayer/)?(filestore|core|app|server|cli)/', line):
                violations.append(f"R6 common-must-not-see-upper-layers: {rel(path)}:{lineno}")

    if violations:
        print("架构约束检查失败：\n")
        for v in violations:
            print("  " + v)
        print(f"\n共 {len(violations)} 处违反。规则说明见 docs/DESIGN.md §2.2。")
        return 1

    total = (
        len(common_files)
        + len(core_files)
        + len(filestore_files)
        + len(server_files)
        + len(cli_files)
        + len(tests_files)
    )
    print(f"架构约束检查通过（扫描 {total} 个源文件）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
