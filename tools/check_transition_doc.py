#!/usr/bin/env python3
"""传输状态机与文档的一致性检查。

PROTOCOL.md §8.2 用表格定义了"哪些 (状态, 事件) 组合合法"，代码里
core/include/relayer/core/transfer/StateMachine.h 的 kTransitions 是同一份
信息的另一份拷贝。两份手写的东西一定会漂移：改了文档忘了改表，或者反过来。

单元测试已经拿"手抄的文档表"和 kTransitions 对比过，但那份手抄表本身也可能
抄错。本脚本直接解析 PROTOCOL.md，把关口推到真正的文档上。

检查项：

  T1  文档 §8.2 表里的每一行都在 kTransitions 里，且目标状态一致
  T2  kTransitions 的每一项都在文档表里（无多写的转移）
  T3  文档 §8.1 的每个状态都在 State 枚举里，且名称完全一致
  T4  State 枚举的每一项都在文档 §8.1 里
  T5  文档表里的事件名都在 Event 枚举里，且 Event 枚举没有文档外的事件

用法
----
    python3 tools/check_transition_doc.py       # 检查，不一致则退出码 1
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEADER = ROOT / "core" / "include" / "relayer" / "core" / "transfer" / "StateMachine.h"
DOC = ROOT / "docs" / "PROTOCOL.md"

failures: list[str] = []


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"error: 无法读取 {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def fail(msg: str) -> None:
    failures.append(msg)


def section(doc: str, heading: str) -> str:
    """取 `heading` 开头的一节，到下一个同级或更高级标题为止。"""
    pattern = re.compile(rf"^{re.escape(heading)}\s*$(.*?)^(##|###)\s", re.S | re.M)
    m = pattern.search(doc)
    if not m:
        # 最后一节可能没有后继标题
        pattern2 = re.compile(rf"^{re.escape(heading)}\s*$(.*)", re.S | re.M)
        m = pattern2.search(doc)
        if not m:
            print(f"error: 文档里找不到 {heading} 一节", file=sys.stderr)
            sys.exit(2)
    return m.group(1)


# ---------------------------------------------------------------------------
# 代码侧解析
# ---------------------------------------------------------------------------


def parse_enum(header: str, name: str) -> list[str]:
    """从 `enum class <name>` 里取枚举项名称列表（按出现顺序）。"""
    m = re.search(rf"enum class {name}\s*\{{(.*?)\}};", header, re.S)
    if not m:
        print(f"error: StateMachine.h 里找不到 enum class {name}", file=sys.stderr)
        sys.exit(2)
    body = m.group(1)
    # 去掉行注释与块注释，避免注释里的 kXxx 被误认
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out: list[str] = []
    for mm in re.finditer(r"^\s*(k\w+)", body, re.M):
        out.append(mm.group(1))
    return out


def parse_transitions(header: str) -> list[tuple[str, str, str]]:
    """从 kTransitions 数组里取 [(from, event, to)]。"""
    m = re.search(r"inline constexpr Transition kTransitions\[\]\s*=\s*\{(.*?)\n\};", header, re.S)
    if not m:
        print("error: StateMachine.h 里找不到 kTransitions 数组", file=sys.stderr)
        sys.exit(2)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    out: list[tuple[str, str, str]] = []
    for mm in re.finditer(r"\{\s*State::(k\w+)\s*,\s*Event::(k\w+)\s*,\s*State::(k\w+)\s*\}", body):
        out.append((mm.group(1), mm.group(2), mm.group(3)))
    return out


# ---------------------------------------------------------------------------
# 文档侧解析
# ---------------------------------------------------------------------------


def parse_doc_states(doc: str) -> list[str]:
    """§8.1 的状态表：| `Idle` | 连接已建立... |"""
    body = section(doc, "### 8.1 状态")
    out: list[str] = []
    for line in body.splitlines():
        m = re.match(r"\|\s*`(\w+)`\s*\|", line)
        if m:
            out.append("k" + m.group(1))
    return out


def parse_doc_transitions(doc: str) -> list[tuple[str, str, str]]:
    """§8.2 的转移表：| `Idle` | `StartHandshake` | `Handshaking` |

    目标状态列的**反引号里**是状态名，反引号之后可能还有注解
    （例如 `Idle`（已认证，可开业务会话）），所以要允许尾随内容。
    """
    body = section(doc, "### 8.2 事件与转移")
    out: list[tuple[str, str, str]] = []
    for line in body.splitlines():
        m = re.match(r"\|\s*`(\w+)`\s*\|\s*`(\w+)`\s*\|\s*`(\w+)`", line)
        if m:
            out.append(("k" + m.group(1), "k" + m.group(2), "k" + m.group(3)))
    return out


# ---------------------------------------------------------------------------
# 检查
# ---------------------------------------------------------------------------


def main() -> int:
    header = read(HEADER)
    doc = read(DOC)

    code_states = parse_enum(header, "State")
    code_events = parse_enum(header, "Event")
    code_trans = parse_transitions(header)

    doc_states = parse_doc_states(doc)
    doc_trans = parse_doc_transitions(doc)

    if not code_trans:
        fail("kTransitions 解析为空——检查解析规则是否与源码格式脱节")
    if not doc_trans:
        fail("§8.2 转移表解析为空——检查解析规则是否与文档格式脱节")

    # T1 文档每一行都在代码里，且目标一致
    code_map = {(f, e): t for (f, e, t) in code_trans}
    for (f, e, t) in doc_trans:
        key = (f, e)
        if key not in code_map:
            fail(f"T1 文档有、代码无：{f[1:]} --{e[1:]}--> {t[1:]}")
        elif code_map[key] != t:
            fail(f"T1 目标状态不一致：{f[1:]} --{e[1:]}--> 文档={t[1:]} 代码={code_map[key][1:]}")

    # T2 代码每一行都在文档里
    doc_map = {(f, e): t for (f, e, t) in doc_trans}
    for (f, e, t) in code_trans:
        key = (f, e)
        if key not in doc_map:
            fail(f"T2 代码有、文档无：{f[1:]} --{e[1:]}--> {t[1:]}")

    # T3/T4 状态集合一致
    for s in doc_states:
        if s not in code_states:
            fail(f"T3 文档有、代码无的状态：{s[1:]}")
    for s in code_states:
        if s not in doc_states:
            fail(f"T4 代码有、文档无的状态：{s[1:]}")

    # T5 事件集合一致（文档里的事件只能通过转移表体现）
    doc_events = {e for (_, e, _) in doc_trans}
    for e in doc_events:
        if e not in code_events:
            fail(f"T5 文档有、代码无的事件：{e[1:]}")
    for e in code_events:
        if e not in doc_events:
            fail(f"T5 代码有、文档无的事件：{e[1:]}")

    # 额外的健全性：转移表里引用到的状态/事件都必须在枚举里
    for (f, e, t) in code_trans:
        if f not in code_states:
            fail(f"kTransitions 引用了未定义的状态 {f[1:]}")
        if t not in code_states:
            fail(f"kTransitions 引用了未定义的状态 {t[1:]}")
        if e not in code_events:
            fail(f"kTransitions 引用了未定义的事件 {e[1:]}")

    if failures:
        print("转移表与文档不一致：", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(f"ok: {len(doc_trans)} 条转移、{len(doc_states)} 个状态、{len(doc_events)} 个事件，文档与代码一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
