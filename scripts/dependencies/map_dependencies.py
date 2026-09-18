#!/usr/bin/env python3
"""Regenerate dependencies/inventory.json from dependencies/sources.yml.

Repository-aware, not a generic SBOM scanner: each vendor SDK directory in
dependencies/sources.yml is treated as ONE component for build/update
purposes, but every file under it is recursively scanned for risk (hashes,
ELF metadata, firmware, licenses, nested third-party libraries). Nested
example-project references (Qt, OpenCV, INDI, ...) are recorded as SDK
content only -- never promoted to AlpacaBridge dependencies.

Usage:
  python3 scripts/dependencies/map_dependencies.py --check   # CI: fail on drift/stale/unacked risk
  python3 scripts/dependencies/map_dependencies.py --write   # regenerate dependencies/inventory.json

Run from the repository root.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML is required: pip install -r scripts/dependencies/requirements.txt\n")
    sys.exit(1)

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCES_PATH = REPO_ROOT / "dependencies" / "sources.yml"
INVENTORY_PATH = REPO_ROOT / "dependencies" / "inventory.json"

# Filenames that indicate a bundled third-party library separate from the
# Debian package of the same name (e.g. SVBONY shipping its own libusb).
NESTED_THIRD_PARTY_PATTERNS = [
    re.compile(r"libusb"),
    re.compile(r"libjpeg"),
    re.compile(r"libpng"),
    re.compile(r"libz\.so"),
    re.compile(r"libudev"),
    re.compile(r"libftdi"),
]
LICENSE_NAME_RE = re.compile(r"^(LICENSE|COPYING|EULA|LICENCE)", re.IGNORECASE)
FIRMWARE_DIR_RE = re.compile(r"firmware", re.IGNORECASE)
EXAMPLE_DIR_RE = re.compile(r"(example|demo|sample)", re.IGNORECASE)


def load_sources() -> dict:
    return yaml.safe_load(SOURCES_PATH.read_text())


def resolve_local_version(dep: dict) -> str | None:
    """Resolve a dependency's installed version from disk instead of trusting a
    static sources.yml value -- used for SDKs whose version lives only in a
    SONAME (e.g. libASICamera2.so.1.41). Returns None if no file matches."""
    version_from = dep.get("local", {}).get("version_from")
    if not version_from or version_from.get("type") != "filename":
        return None
    matches = sorted(REPO_ROOT.glob(version_from["glob"]))
    if not matches:
        return None
    pattern = re.compile(version_from["regex"])
    for path in matches:
        m = pattern.search(path.name)
        if m:
            return m.group(1)
    return None


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_type(path: Path) -> str | None:
    if not shutil.which("file"):
        return None
    try:
        out = subprocess.run(["file", "-b", str(path)], capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or None
    except Exception:
        return None


def elf_info(path: Path, ftype: str | None) -> dict | None:
    if not ftype or "ELF" not in ftype or not shutil.which("readelf"):
        return None
    try:
        out = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True, timeout=10)
    except Exception:
        return None
    if out.returncode != 0:
        return None
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", out.stdout)
    soname = re.search(r"\(SONAME\)\s+Library soname: \[(.+?)\]", out.stdout)
    rpath = re.search(r"\((?:RPATH|RUNPATH)\)\s+Library (?:rpath|runpath): \[(.+?)\]", out.stdout)
    return {
        "needed": needed,
        "soname": soname.group(1) if soname else None,
        "rpath": rpath.group(1) if rpath else None,
    }


def scan_vendor_tree(scan_path: Path) -> dict:
    """Recursively inspect every file under a vendor SDK directory."""
    libraries, executables, archives, licenses = [], [], [], []
    firmware_files: list[Path] = []
    file_count = 0
    total_size = 0
    example_present = False
    example_used_by_project = False

    for p in scan_path.rglob("*"):
        if p.is_dir():
            if EXAMPLE_DIR_RE.search(p.name):
                example_present = True
            continue
        if p.is_symlink() and not p.exists():
            continue  # dangling symlink target outside tree; recorded by parent walk
        if not p.is_file():
            continue
        file_count += 1
        rel = str(p.relative_to(scan_path))
        try:
            size = p.stat().st_size
        except OSError:
            continue
        total_size += size

        if LICENSE_NAME_RE.match(p.name):
            licenses.append(rel)
        if FIRMWARE_DIR_RE.search(str(p.parent)):
            firmware_files.append(p)

        ftype = file_type(p)
        is_elf = bool(ftype and "ELF" in ftype)
        is_exec = bool(p.stat().st_mode & 0o111)

        record = {"path": rel, "sha256": sha256_file(p), "size": size}
        if is_elf:
            info = elf_info(p, ftype)
            if info:
                record.update(info)
            libraries.append(record)
        elif is_exec and ".so" not in p.name:
            executables.append(record)

        for pat in NESTED_THIRD_PARTY_PATTERNS:
            if pat.search(p.name) and "libqhyccd" not in p.name and "libSVBCameraSDK" not in p.name \
               and "libtoupcam" not in p.name and "libASICamera" not in p.name and "libPlayerOne" not in p.name:
                record.setdefault("_nested_third_party", True)

        if p.suffix in (".tar", ".gz", ".xz", ".zip", ".bz2", ".tgz"):
            archives.append(rel)

    firmware = {
        "count": len(firmware_files),
        "aggregate_sha256": hashlib.sha256(
            b"".join(sha256_file(f).encode() for f in sorted(firmware_files))
        ).hexdigest() if firmware_files else None,
    }

    return {
        "file_count": file_count,
        "total_size_bytes": total_size,
        "libraries": libraries,
        "executables": executables,
        "firmware": firmware,
        "archives": archives,
        "licenses": licenses,
        "examples": {"present": example_present, "used_by_project": example_used_by_project},
    }


def compute_risks(contents: dict) -> list[dict]:
    risks = []
    if not contents["licenses"]:
        risks.append({"code": "no-sdk-license", "severity": "high"})
    if contents["libraries"]:
        risks.append({"code": "precompiled-binary", "severity": "medium"})
    if contents["firmware"]["count"]:
        risks.append({"code": "firmware-without-source", "severity": "medium"})
    risks.append({"code": "upstream-signature-unavailable", "severity": "medium"})
    nested = {lib["path"] for lib in contents["libraries"] if lib.get("_nested_third_party")}
    for path in nested:
        risks.append({"code": "nested-third-party-component", "severity": "medium", "detail": path})
    for lib in contents["libraries"]:
        lib.pop("_nested_third_party", None)
    return risks


def apply_acknowledgements(dep_id: str, risks: list[dict], acknowledged: list[dict], today: datetime.date) -> None:
    for risk in risks:
        match = next((a for a in acknowledged if a["component"] == dep_id and a["code"] == risk["code"]), None)
        if match is None:
            risk["acknowledged"] = False
            risk["review_after"] = None
            continue
        review_after = datetime.date.fromisoformat(match["review_after"])
        risk["acknowledged"] = review_after >= today
        risk["review_after"] = match["review_after"]


def check_drift(dep: dict) -> list[str]:
    failures = []
    for rule in dep.get("drift", []):
        target = REPO_ROOT / rule["file"]
        if not target.exists():
            failures.append(f"{dep['id']}: drift target {rule['file']} does not exist")
            continue
        text = target.read_text(errors="ignore")
        for needle in rule["must_contain"]:
            if needle not in text:
                failures.append(f"{dep['id']}: expected '{needle}' in {rule['file']}, not found (drift)")
    return failures


def check_checkout_annotation_drift() -> list[str]:
    """actions/checkout may be pinned to one SHA with disagreeing version comments across a workflow."""
    failures = []
    workflows_dir = REPO_ROOT / ".github" / "workflows"
    if not workflows_dir.is_dir():
        return failures
    sha_to_comments: dict[str, set[str]] = {}
    pattern = re.compile(r"actions/checkout@([0-9a-f]{40})\s*(#.*)?")
    for wf in sorted(workflows_dir.glob("*.yml")):
        for line in wf.read_text(errors="ignore").splitlines():
            m = pattern.search(line)
            if not m:
                continue
            sha, comment = m.group(1), (m.group(2) or "").strip()
            if comment:
                sha_to_comments.setdefault(sha, set()).add(comment)
    for sha, comments in sha_to_comments.items():
        if len(comments) > 1:
            failures.append(
                f"actions/checkout@{sha} has disagreeing version comments across workflows: {sorted(comments)}"
            )
    return failures


def check_binary_hash_drift(dep_id: str, contents: dict, previous: dict | None) -> list[str]:
    """A vendor SDK binary changed without a sources.yml version bump: always fails, no acknowledgement."""
    if previous is None:
        return []
    prev_hashes = {lib["path"]: lib["sha256"] for lib in previous.get("libraries", [])}
    failures = []
    for lib in contents["libraries"]:
        prev = prev_hashes.get(lib["path"])
        if prev is not None and prev != lib["sha256"]:
            failures.append(f"{dep_id}: {lib['path']} hash changed without a sources.yml version bump")
    new_paths = {lib["path"] for lib in contents["libraries"]} - set(prev_hashes)
    if new_paths and previous.get("libraries"):
        failures.append(f"{dep_id}: new binaries appeared: {sorted(new_paths)}")
    return failures


def build_inventory(data: dict, previous: dict | None) -> tuple[dict, list[str]]:
    today = datetime.date.today()
    acknowledged = data.get("acknowledged_risks", [])
    dependencies_out = []
    drift_failures: list[str] = []
    hard_failures: list[str] = []

    prev_by_id = {d["id"]: d for d in (previous or {}).get("dependencies", [])} if previous else {}

    for dep in data["dependencies"]:
        drift_failures.extend(check_drift(dep))
        current = dict(dep["current"])
        resolved_version = resolve_local_version(dep)
        if resolved_version is not None:
            current["version"] = resolved_version
        elif dep.get("local", {}).get("version_from"):
            drift_failures.append(f"{dep['id']}: no file matched local.version_from glob; cannot resolve installed version")

        out = {
            "id": dep["id"],
            "kind": dep["kind"],
            "relationship": dep["relationship"],
            "current": current,
            "usage": dep.get("usage", {}),
        }
        if dep["kind"] == "vendor-sdk":
            scan_path = REPO_ROOT / dep["scan_path"]
            if not scan_path.is_dir():
                drift_failures.append(f"{dep['id']}: scan_path {dep['scan_path']} does not exist")
            else:
                contents = scan_vendor_tree(scan_path)
                out["contents"] = contents
                risks = compute_risks(contents)
                apply_acknowledgements(dep["id"], risks, acknowledged, today)
                out["risks"] = risks
                for r in risks:
                    if not r["acknowledged"]:
                        hard_failures.append(f"{dep['id']}: unacknowledged risk {r['code']}")
                previous_contents = prev_by_id.get(dep["id"], {}).get("contents")
                hard_failures.extend(check_binary_hash_drift(dep["id"], contents, previous_contents))
        dependencies_out.append(out)

    drift_failures.extend(check_checkout_annotation_drift())

    inventory = {
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source_commit": subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], capture_output=True, text=True
        ).stdout.strip(),
        "dependencies": dependencies_out,
        "external_references": data.get("external_references", []),
        "drift_failures": [{"component": "repository", "message": m} for m in drift_failures],
    }
    return inventory, drift_failures + hard_failures


def normalize_for_diff(inventory: dict) -> dict:
    clone = json.loads(json.dumps(inventory))
    clone.pop("generated_at", None)
    clone.pop("source_commit", None)
    return clone


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="fail if inventory.json is stale or drift/risk failures exist")
    mode.add_argument("--write", action="store_true", help="regenerate dependencies/inventory.json")
    args = parser.parse_args()

    data = load_sources()
    previous = json.loads(INVENTORY_PATH.read_text()) if INVENTORY_PATH.exists() else None
    inventory, failures = build_inventory(data, previous)

    if args.write:
        INVENTORY_PATH.write_text(json.dumps(inventory, indent=2, sort_keys=False) + "\n")
        print(f"wrote {INVENTORY_PATH.relative_to(REPO_ROOT)}")
        if failures:
            for f in failures:
                print(f"WARNING: {f}", file=sys.stderr)
        return 0

    # --check
    stale = previous is None or normalize_for_diff(inventory) != normalize_for_diff(previous)
    if stale:
        failures = [f"{INVENTORY_PATH.relative_to(REPO_ROOT)} is stale; run with --write and commit the result"] + failures
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        print(f"{len(failures)} dependency-inventory check(s) failed", file=sys.stderr)
        return 1
    print("dependency inventory: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
