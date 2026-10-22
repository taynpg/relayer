#!/usr/bin/env python3
"""协议文档与代码的一致性检查。

docs/PROTOCOL.md 声称是"唯一权威说明"，那就必须和代码里的枚举保持同步。
口头约定靠不住——加一个帧类型忘了改文档，读者就会按旧文档实现，出问题
的时候还要靠翻 git log 才能发现。

本脚本检查：

  D1  FrameType 的每个枚举值都在文档的类型表里，且数值一致
  D2  文档里列出的每个类型号都在代码里有定义（防止文档写了没实现的类型）
  D3  帧头布局表里声明的固定头大小与 Frame.h 里的 kFixedHeaderSize 一致
  D4  分块段的起始号与代码一致

用法
----
    python3 tools/check_protocol_doc.py          # 检查，不一致则退出码 1
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEADER = ROOT / "core" / "include" / "relayer" / "core" / "protocol" / "Frame.h"
DOC = ROOT / "docs" / "PROTOCOL.md"


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"error: 无法读取 {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def parse_code_types(header: str) -> dict[str, int]:
    """从 enum class FrameType 里取 {名称: 数值}。"""
    m = re.search(r"enum class FrameType[^{]*\{(.*?)\};", header, re.S)
    if not m:
        print("error: Frame.h 里找不到 enum class FrameType", file=sys.stderr)
        sys.exit(2)
    out: dict[str, int] = {}
    for mm in re.finditer(r"^\s*(k\w+)\s*=\s*(\d+)", m.group(1), re.M):
        out[mm.group(1)] = int(mm.group(2))
    return out


def parse_doc_types(doc: str) -> dict[str, int]:
    """从文档「4. 帧类型」这一章里的表格取 {名称: 数值}。

    只解析 `## 4.` 到下一个 `## ` 之间的内容：标志位表（§3）、status 表（§7.2）
    也是 `| 数值 | kName |` 的形状，不限定范围会把它们误认成帧类型。
    """
    m = re.search(r"^##\s*4\.\s*帧类型\s*$(.*?)^##\s", doc, re.S | re.M)
    if not m:
        print("error: docs/PROTOCOL.md 里找不到「## 4. 帧类型」章节", file=sys.stderr)
        sys.exit(2)

    out: dict[str, int] = {}
    for mm in re.finditer(r"^\|\s*(\d+)\s*\|\s*`(k\w+)`", m.group(1), re.M):
        out[mm.group(2)] = int(mm.group(1))
    return out


def parse_code_const(header: str, name: str) -> int | None:
    m = re.search(rf"constexpr\s+std::\w+\s+{name}\s*=\s*([0-9][0-9'u]*)", header)
    if not m:
        return None
    return int(m.group(1).replace("'", "").rstrip("uU"))


def main() -> int:
    header = read(HEADER)
    doc = read(DOC)

    code = parse_code_types(header)
    documented = parse_doc_types(doc)

    violations: list[str] = []

    # D1: 代码里的每个类型都要在文档里，且数值一致
    for name, value in sorted(code.items(), key=lambda kv: kv[1]):
        if name not in documented:
            violations.append(f"D1 类型 {name}(={value}) 未在 docs/PROTOCOL.md 的类型表中出现")
        elif documented[name] != value:
            violations.append(f"D1 类型 {name} 数值不一致：代码 {value}，文档 {documented[name]}")

    # D2: 文档里有的，代码里也必须有
    for name, value in sorted(documented.items(), key=lambda kv: kv[1]):
        if name not in code:
            violations.append(f"D2 文档里的 {name}(={value}) 在 Frame.h 中没有对应枚举")

    # D3: 固定头大小
    header_size = parse_code_const(header, "kFixedHeaderSize")
    if header_size is None:
        violations.append("D3 无法从 Frame.h 解析 kFixedHeaderSize")
    else:
        m = re.search(r"###\s*2\.1\s*固定头（(\d+)\s*字节）", doc)
        if not m:
            violations.append("D3 文档里找不到「固定头（N 字节）」小节标题")
        elif int(m.group(1)) != header_size:
            violations.append(f"D3 固定头大小不一致：代码 {header_size}，文档 {m.group(1)}")

    # D4: 分块段起始号
    chunk_begin = parse_code_const(header, "kChunkBegin")
    if chunk_begin is None:
        violations.append("D4 无法从 Frame.h 解析 kChunkBegin")
    else:
        m = re.search(r"\|\s*`(\d+)\s*\.\.\s*65535`\s*\|\s*分块数据与确认", doc)
        if not m:
            violations.append("D4 文档里找不到「分块数据与确认」号段行")
        elif int(m.group(1)) != chunk_begin:
            violations.append(f"D4 分块段起始号不一致：代码 {chunk_begin}，文档 {m.group(1)}")

    if violations:
        print("协议文档一致性检查失败：\n")
        for v in violations:
            print("  " + v)
        print(f"\n共 {len(violations)} 处不一致。请同步更新 docs/PROTOCOL.md 与 Frame.h。")
        return 1

    print(f"协议文档一致性检查通过（{len(code)} 个帧类型与文档完全对应）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
