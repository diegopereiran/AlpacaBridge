"""static-download-page provider: a vendor page listing several downloadable
documents (PDFs, manuals, spec sheets) in static HTML, each with a title, a
direct download URL, a file size, and a DD-MM-YYYY publication date next to
it (Sky-Watcher's application-development page is the first user). Matched
by exact title text, since the same page lists several documents and a URL
alone isn't guaranteed to be title-stable.
"""
from __future__ import annotations

import html as html_entities
import re

from . import _http

ENTRY_WINDOW = 900
SIZE_DATE_RE = re.compile(r"Size:&nbsp;(\d+)\s?KB&nbsp;&nbsp;\|&nbsp;&nbsp;(\d{2}-\d{2}-\d{4})")
HREF_RE = re.compile(r'href="([^"]+)"\s+target="_blank">Download</a>')


def fetch_entry(dep: dict) -> dict:
    updates = dep["updates"]
    page_html, _ = _http.get_text(updates["page"])
    if page_html is None:
        return {"status": "source-unreachable"}

    title = updates["title"]
    idx = page_html.find(title)
    if idx == -1:
        return {"status": "source-format-changed", "detail": f"title {title!r} not found on page"}

    window = page_html[idx:idx + ENTRY_WINDOW]
    size_date_m = SIZE_DATE_RE.search(window)
    href_m = HREF_RE.search(window)
    if not size_date_m or not href_m:
        return {"status": "source-format-changed", "detail": "entry found but size/date/download-link layout changed"}

    return {
        "status": "ok",
        "title": title,
        "download_url": html_entities.unescape(href_m.group(1)),
        "size_kb": int(size_date_m.group(1)),
        "published_date": size_date_m.group(2),
    }
