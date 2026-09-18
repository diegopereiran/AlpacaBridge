"""vendor-page provider: scrape a vendor download/changelog page for a version string."""
from __future__ import annotations

import re
import urllib.error
import urllib.request


def _version_key(v: str) -> tuple:
    return tuple(int(p) if p.isdigit() else p for p in re.split(r"[.\-]", v))


def latest_version(dep: dict) -> dict:
    updates = dep["updates"]
    url = updates.get("url")
    pattern = updates.get("version_pattern")
    if not url or not pattern:
        return {
            "status": "manual-check-required",
            "latest_version": None,
            "reason": updates.get("reason", "No machine-readable version pattern configured."),
        }

    req = urllib.request.Request(url, headers={"User-Agent": "alpacabridge-dependency-mapper"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            text = resp.read().decode("utf-8", errors="ignore")
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return {"status": "source-unreachable", "latest_version": None}
    except Exception:
        return {"status": "source-unreachable", "latest_version": None}

    matches = re.findall(pattern, text)
    if not matches:
        return {"status": "version-unknown", "latest_version": None}
    try:
        latest = max(set(matches), key=_version_key)
    except (TypeError, ValueError):
        latest = matches[0]
    return {"status": "ok", "latest_version": latest}
