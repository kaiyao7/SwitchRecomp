#!/usr/bin/env python3
"""
Static dependency check for SwitchRecomp.

Verifies that #include directives in src/ and include/ respect the module
dependency matrix defined in docs/architecture/modules.md.

Exit code 0 = pass, non-zero = violations found.
"""

import os
import re
import sys
from pathlib import Path

# Allowed includes: module -> set of modules it may include from include/switchrecomp/
ALLOWED = {
    "common":     set(),
    "platform":   {"common"},
    "ir":         {"common", "platform"},
    "translator": {"common", "ir"},
    "codegen":    {"common", "ir"},
    "runtime":    {"common", "platform", "ir"},
    "gpu":        {"common", "platform", "runtime"},
}

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_RE = re.compile(r'#include\s*[<"](switchrecomp/([^/]+)/[^">]+)[>"]')


def module_of(path: Path) -> str | None:
    parts = path.parts
    for i, p in enumerate(parts):
        if p in ALLOWED:
            return p
    return None


def check_file(path: Path) -> list[str]:
    mod = module_of(path)
    if mod is None:
        return []
    violations = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in INCLUDE_RE.finditer(text):
        dep_mod = m.group(2)
        if dep_mod not in ALLOWED:
            continue
        if dep_mod != mod and dep_mod not in ALLOWED[mod]:
            violations.append(
                f"{path}: module '{mod}' may not include '{m.group(1)}' "
                f"(forbidden dependency '{dep_mod}')"
            )
    return violations


def main() -> int:
    violations: list[str] = []
    for search in ("src", "include"):
        base = ROOT / search
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() in (".h", ".hpp", ".cpp", ".cc", ".c", ".inl"):
                violations.extend(check_file(path))

    if violations:
        print("Dependency violations:")
        for v in violations:
            print(f"  {v}")
        return 1

    print("Dependency check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
