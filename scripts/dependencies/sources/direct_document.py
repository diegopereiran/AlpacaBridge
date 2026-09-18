"""direct-document provider: a single document served directly at a stable
URL with real HTTP caching headers (ETag, Last-Modified, Content-Length) --
e.g. iOptron's RS-232 Command Language PDF. Cheaper and more precise than
scraping a listing page: an HTTP HEAD request is enough to detect a change
without downloading the file at all; only a genuine header change (or an
explicit verify_download pass) triggers a full download for hashing.
"""
from __future__ import annotations

import urllib.error
import urllib.request


def fetch_metadata(dep: dict) -> dict:
    url = dep["updates"]["url"]
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "AlpacaBridge dependency monitor"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            headers = resp.headers
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return {"status": "source-unreachable"}
    except Exception:
        return {"status": "source-unreachable"}

    return {
        "status": "ok",
        "content_type": headers.get("Content-Type"),
        "content_length": headers.get("Content-Length"),
        "etag": headers.get("ETag"),
        "last_modified": headers.get("Last-Modified"),
    }
