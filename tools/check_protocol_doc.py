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
    """解析 `constexpr <type> NAME = <整数字面量表达式>;` 的数值。

    支持 `4u * 1024 * 1024` 这样的乘法表达式——代码里写 4 MiB 时自然会这么写，
    而只认单个整数字面量的正则会把它读成 4，然后误报"不一致"。
    只处理乘法和数字，够用即可；遇到不认识的形状返回 None 让它报出来，
    而不是猜一个值。
    """
    m = re.search(rf"constexpr\s+std::\w+\s+{name}\s*=\s*([^;]+);", header)
    if not m:
        return None

    expr = m.group(1).strip()
    total = 1
    for term in expr.split("*"):
        t = term.strip().replace("'", "").replace("u", "").replace("U", "").replace("L", "").strip()
        if not t.isdigit():
            return None
        total *= int(t)
    return total


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

    # D5: 传输层的常量与状态码
    violations.extend(check_transfer_consts())

    if violations:
        print("协议文档一致性检查失败：\n")
        for v in violations:
            print("  " + v)
        print(f"\n共 {len(violations)} 处不一致。请同步更新 docs/PROTOCOL.md 与对应头文件。")
        return 1

    print(f"协议文档一致性检查通过（{len(code)} 个帧类型与文档完全对应）。")
    return 0


# ---------------------------------------------------------------------------
# D5：传输层常量与状态码
# ---------------------------------------------------------------------------
#
# 这些数值是线格式的一部分，改一个就会让两端对不上，而两边各自的单测
# 都还是绿的（因为测试读的是同一个常量）。文档里写死一份、代码里写死
# 一份，由这里比对，是唯一能让"改了却没人发现"变成红灯的办法。

MERKLE_H = ROOT / "core" / "include" / "relayer" / "core" / "transfer" / "MerkleTree.h"
PAYLOAD_H = ROOT / "core" / "include" / "relayer" / "core" / "transfer" / "TransferPayload.h"


def check_transfer_consts() -> list[str]:
    out: list[str] = []
    doc = read(DOC)
    merkle = read(MERKLE_H)
    payload = read(PAYLOAD_H)

    # 块大小上下限：文档 §9.4.1 写的是 "1024 <= chunkSize <= 4 MiB"。
    m = re.search(r"(`?\d+`?)\s*<=\s*chunkSize\s*<=\s*(\d+)\s*MiB", doc)
    if not m:
        out.append("D5 文档里找不到「chunkSize 范围」的说明（§9.4.1）")
    else:
        doc_min = int(re.sub(r"[`\s]", "", m.group(1)))
        doc_max = int(m.group(2)) * 1024 * 1024
        code_min = parse_code_const(merkle, "kMinChunkSize")
        code_max = parse_code_const(merkle, "kMaxChunkSize")
        if code_min is not None and code_min != doc_min:
            out.append(f"D5 块大小下限不一致：代码 {code_min}，文档 {doc_min}")
        if code_max is not None and code_max != doc_max:
            out.append(f"D5 块大小上限不一致：代码 {code_max}，文档 {doc_max}")

    # 状态码：§9.5 的 AnswerStatus 表、§9.8 的 CompleteStatus 表。
    out.extend(_check_enum_table(doc, payload, r"###\s*9\.5\s", "AnswerStatus", "kAnswer"))
    out.extend(_check_enum_table(doc, payload, r"###\s*9\.8\s", "CompleteStatus", "kComplete"))

    return out


def _check_enum_table(doc: str, header: str, section: str, enum_name: str, tag: str) -> list[str]:
    """比对某一节的 `| 值 | 含义 |` 表与代码里的 enum 数值。"""
    m = re.search(section + r".*?(?=^###\s|\Z)", doc, re.S | re.M)
    if not m:
        return [f"D5 文档里找不到「{section}」小节"]

    documented: dict[int, str] = {}
    for mm in re.finditer(r"^\|\s*(\d+)\s*\|\s*([^|]+)\|", m.group(0), re.M):
        documented[int(mm.group(1))] = mm.group(2).strip()

    m2 = re.search(rf"enum class {enum_name}\s*:\s*std::uint8_t\s*\{{(.*?)\}};", header, re.S)
    if not m2:
        return [f"D5 头文件里找不到 enum class {enum_name}"]

    coded: dict[int, str] = {}
    for mm in re.finditer(r"^\s*(k\w+)\s*=\s*(\d+)", m2.group(1), re.M):
        coded[int(mm.group(2))] = mm.group(1)

    out: list[str] = []
    for value, name in coded.items():
        if value not in documented:
            out.append(f"D5 {enum_name}::{name}(={value}) 未在文档 {tag} 表中出现")
    for value in documented:
        if value not in coded:
            out.append(f"D5 文档 {tag} 表里的状态码 {value} 在 {enum_name} 中没有定义")
    return out


if __name__ == "__main__":
    sys.exit(main())
