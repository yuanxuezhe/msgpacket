#!/usr/bin/env python3
"""
api_bindings_check.py — 扫描 msgpacket binding 中的已删除 API 残留

用法：
    python3 api_bindings_check.py [--fix]

已删除的 API（v1.1+）：
    - msg_code 字段（从 msg_header_t 中移除）
    - msg_get_code() 函数
    - msg_set_code() 函数
    - MSG_CODE_SUCCESS / MSG_CODE_ERROR / MSG_CODE_TIMEOUT 常量
    - HEAD_CODE_POS / HEAD_CODE_LENGTH 宏
"""

import os
import re
import sys
from pathlib import Path

# ================================================================
# 已删除的 API 列表
# ================================================================

DELETED_SYMBOLS = {
    # 函数
    "msg_get_code", "msg_set_code",
    # 废弃常量
    "MSG_CODE_SUCCESS", "MSG_CODE_ERROR", "MSG_CODE_TIMEOUT",
    # 结构体废弃字段偏移量宏
    "HEAD_CODE_POS", "HEAD_CODE_LENGTH",
}

# 文件扩展名 → 注释风格
COMMENT_STYLES = {
    ".py": "#",
    ".c": "//",
    ".cpp": "//",
    ".rs": "//",
    ".h": "//",
    ".go": "//",
    ".java": "//",
}

def scan_file(filepath: Path) -> list:
    """扫描单个文件，返回残留引用列表"""
    issues = []
    ext = filepath.suffix
    comment = COMMENT_STYLES.get(ext, "#")

    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            original = line.rstrip()

            # 跳过纯注释行（减少误报）
            stripped = line.strip()
            if stripped.startswith(comment) and not stripped.startswith(f"{comment} msg_code"):
                continue

            for sym in DELETED_SYMBOLS:
                # 匹配：sym(  /  sym)  /  "sym"  /  sym 紧跟字母数字下划线不算
                # 用 word boundary 避免 "msg_get_code_safe" 误报
                patterns = [
                    rf'\b{re.escape(sym)}\b',  # 函数名/常量名
                ]
                for pat in patterns:
                    if re.search(pat, line):
                        issues.append((lineno, sym, original))
                        break

    return issues


def main():
    base_dir = Path(__file__).parent.parent
    demo_dir = base_dir / "demo"
    binding_files = list(demo_dir.rglob("*.py")) + \
                    list(demo_dir.rglob("*.c")) + \
                    list(demo_dir.rglob("*.cpp")) + \
                    list(demo_dir.rglob("*.rs")) + \
                    list(demo_dir.rglob("*.h"))

    fix = "--fix" in sys.argv

    total_issues = 0
    files_with_issues = set()

    print("=== msgpacket API Binding Check ===\n")
    print(f"扫描目录: {demo_dir}")
    print(f"已删除 API: {', '.join(sorted(DELETED_SYMBOLS))}\n")

    for fp in sorted(binding_files):
        issues = scan_file(fp)
        if issues:
            files_with_issues.add(fp)
            rel = fp.relative_to(base_dir)
            print(f"❌ {rel}:")
            for lineno, sym, line in issues:
                print(f"   L{lineno}: [{sym}]  {line[:80]}")
            print()
            total_issues += len(issues)

    if total_issues == 0:
        print("✅ 所有 binding 无残留引用")
        return 0
    else:
        print(f"⚠️  共 {total_issues} 处残留引用，涉及 {len(files_with_issues)} 个文件")
        if fix:
            print("\n[fix 模式暂未实现，请手动清理]")
        return 1


if __name__ == "__main__":
    sys.exit(main())
