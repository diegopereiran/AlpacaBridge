#!/usr/bin/env python3
"""Validate dependencies/inventory.json against dependencies/inventory.schema.json.

Stdlib-only structural check (required keys, types, enums) -- not a full
JSON Schema implementation. Sufficient to catch a malformed or hand-edited
inventory.json before it reaches map_dependencies.py's staleness check.

Usage: python3 scripts/dependencies/validate_inventory.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
INVENTORY_PATH = REPO_ROOT / "dependencies" / "inventory.json"
SCHEMA_PATH = REPO_ROOT / "dependencies" / "inventory.schema.json"

VALID_KINDS = {
    "vendor-sdk", "vendored-source", "cmake-dependency",
    "test-dependency", "ci-tool", "system-package", "managed-elsewhere",
    "external-specification", "externally-derived-protocol",
}
VALID_RELATIONSHIPS = {"direct", "indirect"}
VALID_SEVERITIES = {"low", "medium", "high"}


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def validate(inventory: dict) -> list[str]:
    errors: list[str] = []
    for key in ("generated_at", "source_commit", "dependencies", "external_references", "drift_failures"):
        if key not in inventory:
            fail(errors, f"missing top-level key: {key}")

    for i, dep in enumerate(inventory.get("dependencies", [])):
        prefix = f"dependencies[{i}] ({dep.get('id', '?')})"
        for key in ("id", "kind", "relationship", "current"):
            if key not in dep:
                fail(errors, f"{prefix}: missing key {key}")
        if dep.get("kind") not in VALID_KINDS:
            fail(errors, f"{prefix}: invalid kind {dep.get('kind')!r}")
        if dep.get("relationship") not in VALID_RELATIONSHIPS:
            fail(errors, f"{prefix}: invalid relationship {dep.get('relationship')!r}")
        for risk in dep.get("risks", []):
            for key in ("code", "severity", "acknowledged"):
                if key not in risk:
                    fail(errors, f"{prefix}: risk missing key {key}")
            if risk.get("severity") not in VALID_SEVERITIES:
                fail(errors, f"{prefix}: risk has invalid severity {risk.get('severity')!r}")

    for i, ref in enumerate(inventory.get("external_references", [])):
        for key in ("id", "path", "provenance"):
            if key not in ref:
                fail(errors, f"external_references[{i}]: missing key {key}")

    for i, failure in enumerate(inventory.get("drift_failures", [])):
        for key in ("component", "message"):
            if key not in failure:
                fail(errors, f"drift_failures[{i}]: missing key {key}")

    return errors


def main() -> int:
    if not INVENTORY_PATH.exists():
        print(f"FAIL: {INVENTORY_PATH.relative_to(REPO_ROOT)} does not exist", file=sys.stderr)
        return 1
    inventory = json.loads(INVENTORY_PATH.read_text())
    errors = validate(inventory)
    if errors:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        print(f"{len(errors)} validation error(s)", file=sys.stderr)
        return 1
    print(f"{INVENTORY_PATH.relative_to(REPO_ROOT)}: OK ({len(inventory['dependencies'])} dependencies, "
          f"{len(inventory['external_references'])} external references)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
