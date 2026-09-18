"""github-release provider: latest release/tag of a GitHub repository."""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request


def latest_version(dep: dict) -> dict:
    repo = dep["updates"]["repository"]
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "alpacabridge-dependency-mapper"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"

    for url in (f"https://api.github.com/repos/{repo}/releases/latest",
                f"https://api.github.com/repos/{repo}/tags"):
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                payload = json.loads(resp.read())
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
            continue
        except Exception:
            continue
        if isinstance(payload, dict) and payload.get("tag_name"):
            return {"status": "ok", "latest_version": payload["tag_name"].lstrip("v")}
        if isinstance(payload, list) and payload:
            return {"status": "ok", "latest_version": payload[0]["name"].lstrip("v")}

    return {"status": "source-unreachable", "latest_version": None}
