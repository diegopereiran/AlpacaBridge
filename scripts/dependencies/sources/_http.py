"""Shared HTTP helper for the provider adapters.

Python's urllib does not auto-decompress a response the way curl or
`requests` does -- a server that returns `Content-Encoding: gzip` (observed
live on wandererastro.com, which gzips even without an explicit
`Accept-Encoding` request header) hands back raw gzip bytes, and decoding
them as UTF-8 text silently produces garbage rather than an error. Every
provider that fetches an HTML/text page goes through this helper instead of
calling urlopen directly, so this class of bug is fixed once, not per
provider.
"""
from __future__ import annotations

import gzip
import urllib.error
import urllib.request

USER_AGENT = "AlpacaBridge dependency monitor"


def get_text(url: str, timeout: int = 30, method: str | None = None) -> tuple[str | None, object]:
    """Fetch url as decoded text. Returns (text, response) on success, or
    (None, None) on any network/HTTP error -- callers check for None."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT}, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            headers = resp.headers
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return None, None
    except Exception:
        return None, None

    if headers.get("Content-Encoding", "").lower() == "gzip":
        try:
            raw = gzip.decompress(raw)
        except OSError:
            pass  # not actually gzip despite the header; fall through and try to decode as-is

    return raw.decode("utf-8", errors="replace"), headers
