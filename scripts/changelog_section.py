#!/usr/bin/env python3
"""Print the CHANGELOG.md body for one version, for use as GitHub Release notes.

Handles both heading forms used in this repo: the expanded top section
(``## [X.Y.Z] - YYYY-MM-DD``) and collapsed released sections
(``<summary><strong>[X.Y.Z] - YYYY-MM-DD</strong></summary>`` inside
``<details>``). Fails if the section is missing or still UNRELEASED, so a tag
can never publish notes for an uncut release.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HEADING_RE = re.compile(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*$")
SUMMARY_HEADING_RE = re.compile(
    r"^<summary><strong>\[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}|UNRELEASED))?\s*</strong></summary>\s*$"
)


def extract(text: str, version: str) -> tuple[str | None, list[str]]:
    """Return (date, body_lines) for ``version``; date is None if not found."""
    date: str | None = None
    body: list[str] = []
    capturing = False
    for line in text.splitlines():
        m = HEADING_RE.match(line) or SUMMARY_HEADING_RE.match(line)
        if m:
            if capturing:
                break
            if m.group(1) == version:
                capturing = True
                date = m.group(2) or ""
            continue
        if not capturing:
            continue
        stripped = line.strip()
        if stripped.startswith("<details") or stripped.startswith("</details"):
            continue
        body.append(line.rstrip())
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return date, body


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="bare version, e.g. 3.6.0 (no v prefix)")
    parser.add_argument("--changelog", default="CHANGELOG.md", type=Path)
    args = parser.parse_args()

    date, body = extract(args.changelog.read_text(encoding="utf-8"), args.version)
    if date is None:
        print(f"ERROR: no CHANGELOG section for [{args.version}]", file=sys.stderr)
        return 1
    if date in ("", "UNRELEASED"):
        print(f"ERROR: CHANGELOG section [{args.version}] is not dated (still UNRELEASED)", file=sys.stderr)
        return 1
    if not body:
        print(f"ERROR: CHANGELOG section [{args.version}] is empty", file=sys.stderr)
        return 1
    print("\n".join(body))
    return 0


if __name__ == "__main__":
    sys.exit(main())
