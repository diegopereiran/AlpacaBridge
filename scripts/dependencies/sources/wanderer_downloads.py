"""wanderer-downloads provider: WandererAstro's downloads page lists many
files, each with a title, a size, and a direct (uncaptcha'd, unlike SVBONY)
download URL in a `data-url` attribute. Unlike Sky-Watcher's page, the
publish date is embedded directly in the filename/title
(e.g. "Serial protocol for WandererCover V4-EC (20250506).pdf") rather than
in a separate field, so matching uses a regex with a date-capturing group
against a stable title prefix instead of an exact title string -- an exact
match would stop matching the moment the vendor ships a dated update.
"""
from __future__ import annotations

import html
import re

from . import _http

SIZE_RE = r'file_item_size">\s*([\d.]+\s?[KMG]B)\s*</div>'
URL_RE = r'data-url="([^"]+)"'
ENTRY_WINDOW = 900


def fetch_entry(dep: dict) -> dict:
    updates = dep["updates"]
    page_html, _ = _http.get_text(updates["page"])
    if page_html is None:
        return {"status": "source-unreachable"}

    title_re = re.compile(updates["title_pattern"])
    m = title_re.search(page_html)
    if not m:
        return {"status": "source-format-changed", "detail": f"no title matching {updates['title_pattern']!r}"}

    window = page_html[m.start():m.start() + ENTRY_WINDOW]
    size_m = re.search(SIZE_RE, window)
    url_m = re.search(URL_RE, window)
    if not size_m or not url_m:
        return {"status": "source-format-changed", "detail": "title found but size/download-link layout changed"}

    download_url = html.unescape(url_m.group(1))
    if download_url.startswith("//"):
        download_url = "https:" + download_url

    return {
        "status": "ok",
        "title": m.group(0),
        "published_date": m.group(1),
        "size": size_m.group(1),
        "download_url": download_url,
    }
