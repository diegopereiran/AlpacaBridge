"""github-source-reference provider: for a driver implemented from a
third-party open-source reference (e.g. AlpacaBridge's iOptron iEAF/iEFW
serial protocol, implemented from INDI's ieaffocus.cpp / ioptron_wheel.cpp,
with no vendor SDK or published spec at all) rather than a vendor SDK or
spec document. Uses the GitHub commits API to find the latest commit that
touched the reference file -- cheaper and more informative (author, date,
message) than fetching and hashing the raw file on every run. Advisory
only: a changed reference means "worth a human look", not "broken".
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request


def fetch_latest_commit(dep: dict) -> dict:
    updates = dep["updates"]
    repo = updates["repository"]
    path = updates["path"]
    url = f"https://api.github.com/repos/{repo}/commits?path={path}&per_page=1"
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "AlpacaBridge dependency monitor"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            payload = json.loads(resp.read())
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return {"status": "source-unreachable"}
    except Exception:
        return {"status": "source-unreachable"}

    if isinstance(payload, dict):  # GitHub error shape, e.g. rate limit
        return {"status": "source-unreachable", "detail": payload.get("message", "unknown API error")}
    if not payload:
        return {"status": "source-format-changed", "detail": f"no commits found for {path}"}

    commit = payload[0]
    return {
        "status": "ok",
        "sha": commit["sha"],
        "date": commit["commit"]["author"]["date"],
        "message": commit["commit"]["message"].splitlines()[0],
    }
