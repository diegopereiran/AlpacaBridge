"""svbony provider: SVBONY publishes the Linux SDK version directly in the
download entry's filename on a static (non-JS-rendered) page, unlike ZWO's
JSON endpoint or QHY's markdown changelog. The page currently lists more than
one Linux SDK entry (an old v1.9.7 alongside the current v1.13.4), so the
highest semantic version must be selected explicitly rather than the first
match. Downloads are gated behind a UUID + "restricted" flag (email/CAPTCHA
on SVBONY's side) -- this module only detects and reports that, it never
attempts to fetch the file.
"""
from __future__ import annotations

import re

from . import _http

DOWNLOAD_PAGE = "https://www.svbony.com/downloads/software-driver"
ENTRY_RE = re.compile(r"linux[-_]SVBCameraSDK[-_]v(\d+\.\d+\.\d+)", re.IGNORECASE)
UUID_RE = re.compile(r'data-fileUUID="([0-9a-f]+)"', re.IGNORECASE)
RESTRICTED_RE = re.compile(r'data-fileRestricted="(true|false)"', re.IGNORECASE)
DATE_RE = re.compile(r'<div class="date">([\d-]+)</div>')


def fetch_upstream(url: str = DOWNLOAD_PAGE) -> dict:
    page_html, _ = _http.get_text(url)
    if page_html is None:
        return {"status": "source-unreachable"}

    matches = ENTRY_RE.findall(page_html)
    if not matches:
        return {"status": "source-format-changed", "detail": "no linux-SVBCameraSDK-vX.Y.Z entry found"}

    def vtuple(v: str) -> tuple:
        return tuple(int(p) for p in v.split("."))

    best_version = max(set(matches), key=vtuple)

    # Best-effort: pull the UUID/restricted flag/date from the same entry
    # block as the winning version string, not just the first of each on
    # the page (the page lists several SDK entries).
    idx = page_html.find(f"linux-SVBCameraSDK-v{best_version}")
    if idx == -1:
        idx = page_html.lower().find(f"linux-svbcamerasdk-v{best_version}".lower())
    window = page_html[idx:idx + 1500] if idx != -1 else ""

    uuid_m = UUID_RE.search(window)
    restricted_m = RESTRICTED_RE.search(window)
    date_m = DATE_RE.search(page_html[max(0, idx - 400):idx + 100]) if idx != -1 else None

    return {
        "status": "ok",
        "version": best_version,
        "file_uuid": uuid_m.group(1) if uuid_m else None,
        "restricted": (restricted_m.group(1) == "true") if restricted_m else None,
        "page_date": date_m.group(1) if date_m else None,
    }
