"""wordpress-acf provider: a WordPress ACF-backed JSON endpoint listing several
SDK downloads in one collection (ZWO's product-sdk endpoint). Matched entries
are compared against the locally installed version by check_updates.py, which
also drives the archive download / hash / ARM64-library verification -- this
module only knows how to fetch and parse the JSON.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request


def _dig(payload, path: str):
    node = payload
    for segment in path.split("."):
        if segment.isdigit():
            node = node[int(segment)]
        else:
            node = node[segment]
    return node


def fetch_entry(dep: dict) -> dict:
    """Fetch and return the matched software_downloads entry, or a status dict."""
    updates = dep["updates"]
    req = urllib.request.Request(updates["endpoint"], headers={"User-Agent": "alpacabridge-dependency-mapper"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            payload = json.loads(resp.read())
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return {"status": "source-unreachable"}
    except json.JSONDecodeError:
        return {"status": "source-format-changed", "detail": "response was not valid JSON"}
    except Exception:
        return {"status": "source-unreachable"}

    try:
        entries = _dig(payload, updates["collection"])
    except (KeyError, IndexError, TypeError):
        return {"status": "source-format-changed", "detail": f"path {updates['collection']} not found in response"}
    if not isinstance(entries, list):
        return {"status": "source-format-changed", "detail": f"{updates['collection']} is not a list"}

    match_field, match_value = next(iter(updates["match"].items()))
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        if entry.get(match_field) == match_value:
            required = (
                updates["version_field"],
                updates["download_url_field"],
            )
            if not all(k in entry for k in required):
                return {"status": "source-format-changed", "detail": f"entry missing expected fields {required}"}
            return {
                "status": "ok",
                "version": str(entry[updates["version_field"]]).lstrip("Vv"),
                "release_date": entry.get(updates.get("release_date_field", "")),
                "size": entry.get(updates.get("size_field", "")),
                "download_url": entry[updates["download_url_field"]],
                "release_notes_url": entry.get(updates.get("release_notes_field", "")),
            }
    return {"status": "source-format-changed", "detail": f"no entry with {match_field}={match_value!r}"}
