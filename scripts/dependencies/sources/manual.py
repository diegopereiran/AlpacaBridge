"""manual provider: no automatic update source exists; always requires human review."""
from __future__ import annotations


def latest_version(dep: dict) -> dict:
    reason = dep["updates"].get("reason", "No automatic update source configured.")
    return {"status": "manual-check-required", "latest_version": None, "reason": reason}
