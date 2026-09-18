"""debian provider: query the Debian Sources API for a package's version in a suite."""
from __future__ import annotations

import json
import urllib.error
import urllib.request


def _query_package(package: str) -> list[dict] | None:
    """Return the package's version list, or None if unreachable/not found.
    `package` must be a Debian SOURCE package name (sources.yml documents this),
    since the Sources API 404s (with HTTP 200 and an {"error": 404} body) for a
    binary/-dev package name that differs from its source package."""
    url = f"https://sources.debian.org/api/src/{package}/"
    req = urllib.request.Request(url, headers={"User-Agent": "alpacabridge-dependency-mapper"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            payload = json.loads(resp.read())
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return None
    except Exception:
        return None
    if "error" in payload:
        return None
    return payload.get("versions")


def latest_version(dep: dict) -> dict:
    updates = dep["updates"]
    release = updates.get("release", "trixie")
    packages = dep["current"].get("packages") or ([updates["package"]] if updates.get("package") else [])
    if not packages:
        return {"status": "manual-check-required", "latest_version": None}

    results = {}
    for package in packages:
        versions = _query_package(package)
        if versions is None:
            results[package] = {"status": "source-unreachable", "latest_version": None}
            continue
        in_release = [v for v in versions if release in v.get("suites", [])]
        chosen = in_release or versions
        if not chosen:
            results[package] = {"status": "version-unknown", "latest_version": None}
            continue
        results[package] = {"status": "ok", "latest_version": chosen[0]["version"]}

    statuses = {r["status"] for r in results.values()}
    overall = "ok" if statuses == {"ok"} else ("source-unreachable" if "source-unreachable" in statuses else "version-unknown")
    return {"status": overall, "latest_version": None, "per_package": results}
