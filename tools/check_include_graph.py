#!/usr/bin/env python3
"""Fails when a module includes a project module it did not declare (specification 19.1).

Reads build/include_graph.json written by CMake from every mwb_module() call, then scans
src/<module>/ and include/mwb/<module>/ for #include <mwb/<other>/...> or "mwb/<other>/...".
"""
import argparse
import json
import re
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]mwb/([a-z_]+)/', re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", required=True)
    parser.add_argument("--root", required=True)
    arguments = parser.parse_args()
    graph = json.loads(Path(arguments.graph).read_text(encoding="utf-8"))
    root = Path(arguments.root)
    problems = []
    checked = 0
    for module, allowed in graph.items():
        permitted = set(allowed) | {module}
        for directory in (root / "src" / module, root / "include" / "mwb" / module):
            if not directory.is_dir():
                continue
            for path in sorted(directory.rglob("*")):
                if path.suffix not in (".cpp", ".hpp", ".h"):
                    continue
                checked += 1
                text = path.read_text(encoding="utf-8", errors="replace")
                for match in INCLUDE.finditer(text):
                    target = match.group(1)
                    if target == "build_info":
                        continue
                    if target not in permitted:
                        line = text.count("\n", 0, match.start()) + 1
                        problems.append(f"{path.relative_to(root)}:{line}: module '{module}' includes "
                                        f"'{target}' which it does not declare")
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"include-graph: {checked} files checked, {len(problems)} violation(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
