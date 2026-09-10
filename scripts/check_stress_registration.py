#!/usr/bin/env python3
"""Fail when a vendor driver has no `[stress]` concurrency-suite coverage.

AGENTS.md requires every new or substantially-changed driver to register a
`[stress]` TEST_CASE with the ThreadSanitizer concurrency suite
(`AlpacaCore/tests/concurrency_stress.h`, wired into the `sanitizers-tsan` CI
job). Registration is manual today, and nothing failed when it was skipped.

Run from the repo root:  python3 scripts/check_stress_registration.py
Regex regression guard (no repo state needed):  python3 scripts/check_stress_registration.py --self-test

Coverage is tracked per (vendor, Alpaca device type) pair, not per vendor.
A vendor-level check (does `test_<vendor>_concurrency_stress.cpp` exist at
all?) would pass ZWO or ToupTek in full the moment any one of their drivers
is registered -- while the ZWO rotator, focuser and dew-heater switch
and the ToupTek focuser were still unregistered, that would have hidden
them behind a green check. Keying on device type as well catches those.

Known gap: the pair is the finest key the gate has, so a registered driver
masks every other driver of the same vendor and type. The two ZWO ASIAIR
switch drivers (`zwo_asiair_switch_driver.cpp`, `zwo_asiair_plus_switch_driver.cpp`,
libgpiod, no fake seam) have no [stress] case and are hidden behind the ZWO
dew-heater switch registration; they are covered by code review only.

The device type for a driver file is read from its own
`get_device_type() const override { return DeviceType::X; }` rather than
guessed from the filename: `gemini_flatpanel_driver.cpp` actually returns
DeviceType::CoverCalibrator, so a filename-based guess would be wrong.

New driver, no stress test yet? Either add the `[stress]` TEST_CASE (see
`test_touptek_concurrency_stress.cpp` for the shape: one factory + one
operate callback), or add the (vendor, device type) pair to ALLOWLIST below
with a comment. The allow-list is meant to shrink, not grow -- an entry left
in place after coverage is added will itself fail the check (see below), so
there is nothing to remember to clean up by hand.
"""

import re
import subprocess
import sys

VENDORS_PREFIX = "AlpacaCore/src/vendors/"
STRESS_TEST_GLOB_PREFIX = "AlpacaCore/tests/test_"
STRESS_TEST_GLOB_SUFFIX = "_concurrency_stress.cpp"

# Anchored to the actual override, not just any DeviceType:: mention in the
# file -- a driver that referenced a different DeviceType::X earlier (a
# comment, a switch/comparison, a helper) before its own override would
# otherwise be silently miscategorized by a plain first-match search.
DEVICE_TYPE_OVERRIDE_RE = re.compile(
    r"get_device_type\s*\(\s*\)\s*const\s+override\s*\{\s*return\s+DeviceType::([A-Za-z]+)\s*;")
# The description is matched as one or more adjacent string literals
# (escapes allowed, `"a" "b"` concatenation allowed) so a comma inside it
# cannot cut the match short and silently drop the tags.
TEST_CASE_TAGS_RE = re.compile(
    r'TEST_CASE\s*\(\s*(?:"(?:[^"\\]|\\.)*"\s*)+,\s*"((?:\[[^\]]+\])+)"')
TAG_RE = re.compile(r"\[([^\]]+)\]")

# (vendor, device type) pairs with no [stress] TEST_CASE yet. Seeded from the
# gap found when this check was introduced (2026-09) so the check starts
# green; each line is a driver this repo already knows is uncovered.
#
# Remove an entry the same PR that adds its [stress] coverage -- a
# still-covered entry left behind is itself a failure (see main()), so
# nothing here can silently go stale.
ALLOWLIST = {
    ("astroasis", "focuser"),
    ("gemini", "covercalibrator"),
    ("gemini", "focuser"),
    ("gemini", "switch"),
    ("qhy", "camera"),
    ("qhy", "filterwheel"),
    ("wandererastro", "covercalibrator"),
    ("wandererastro", "filterwheel"),
    ("wandererastro", "rotator"),
    ("wandererastro", "switch"),
    ("weewx", "observingconditions"),
}


def tracked_files(pattern):
    out = subprocess.run(
        ["git", "ls-files", pattern],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout
    return [p for p in out.splitlines() if p]


def device_types_in_text(text):
    """The distinct device types text's get_device_type() override(s)
    return, lowercased. A file with two driver classes that both return the
    same type (e.g. gemini_flatpanel_driver.cpp) yields one value; text with
    no matching override at all yields none. Split out from
    driver_device_types() (file I/O) so a self-test can exercise this
    directly against synthetic snippets.
    """
    return {v.lower() for v in DEVICE_TYPE_OVERRIDE_RE.findall(text)}


def driver_device_types(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return device_types_in_text(fh.read())


def find_drivers():
    """({(vendor, device_type): [driver file paths]}, [ambiguous findings])"""
    drivers = {}
    ambiguous = []
    for path in tracked_files(VENDORS_PREFIX + "*_driver.cpp"):
        # AlpacaCore/src/vendors/<vendor>/<name>_driver.cpp
        parts = path[len(VENDORS_PREFIX):].split("/")
        if len(parts) != 2:
            continue
        vendor = parts[0]
        types = driver_device_types(path)
        if len(types) > 1:
            ambiguous.append(
                "AMBIGUOUS DEVICE TYPE: %s defines get_device_type() overrides "
                "returning disagreeing values %s -- fix the driver or this "
                "check's DEVICE_TYPE_OVERRIDE_RE, don't guess" % (path, sorted(types))
            )
            continue
        if not types:
            print("WARNING: could not determine device type for %s "
                  "(no get_device_type() override matched) -- treating as uncovered" % path)
            device_type = "unknown"
        else:
            device_type = next(iter(types))
        drivers.setdefault((vendor, device_type), []).append(path)
    return drivers, ambiguous


def stress_tag_sets_in_text(text):
    """[{tag, tag, ...}, ...] for every TEST_CASE in text tagged [stress].

    Split out from find_registered_pairs() (which also needs the known
    vendor/device-type vocabulary) so a self-test can exercise the raw
    TEST_CASE_TAGS_RE/TAG_RE extraction against synthetic snippets.
    """
    tag_sets = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = {t.lower() for t in TAG_RE.findall(m.group(1))}
        if "stress" in tags:
            tag_sets.append(tags)
    return tag_sets


def find_registered_pairs(known_vendors, known_device_types):
    """{(vendor, device_type)} covered by at least one [stress] TEST_CASE.

    Matches tags against the vendor/device-type vocabulary the driver scan
    itself found, rather than assuming a fixed tag order -- a TEST_CASE is
    tagged [vendor][device_type][stress] plus sometimes more (e.g.
    [round-4]), and this only needs to find the two tags that are actually a
    known vendor and a known device type.
    """
    registered = set()
    for path in tracked_files(STRESS_TEST_GLOB_PREFIX + "*" + STRESS_TEST_GLOB_SUFFIX):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for tags in stress_tag_sets_in_text(text):
            vendor_tags = [t for t in tags if t in known_vendors]
            dtype_tags = [t for t in tags if t in known_device_types]
            for vendor in vendor_tags:
                for dtype in dtype_tags:
                    registered.add((vendor, dtype))
    return registered


def main():
    drivers, failures = find_drivers()
    failures = list(failures)  # find_drivers' own ambiguity findings, if any
    known_vendors = {v for v, _ in drivers}
    known_device_types = {d for _, d in drivers}
    registered = find_registered_pairs(known_vendors, known_device_types)

    for (vendor, dtype), paths in sorted(drivers.items()):
        covered = (vendor, dtype) in registered
        allowed = (vendor, dtype) in ALLOWLIST
        if not covered and not allowed:
            failures.append(
                "MISSING: %s/%s has no [stress] TEST_CASE and is not in "
                "ALLOWLIST (%s): %s"
                % (vendor, dtype, __file__, ", ".join(paths))
            )
        if covered and allowed:
            failures.append(
                "STALE ALLOWLIST ENTRY: %s/%s now has [stress] coverage -- "
                "remove ('%s', '%s') from ALLOWLIST in %s"
                % (vendor, dtype, vendor, dtype, __file__)
            )

    driver_pairs = set(drivers)
    for vendor, dtype in sorted(ALLOWLIST - driver_pairs):
        failures.append(
            "STALE ALLOWLIST ENTRY: %s/%s has no matching driver anymore -- "
            "remove ('%s', '%s') from ALLOWLIST in %s"
            % (vendor, dtype, vendor, dtype, __file__)
        )

    if failures:
        print("Stress-test registration check failed:\n")
        for f in failures:
            print("  " + f)
        print("\n%d finding(s)." % len(failures))
        return 1

    print("Stress-test registration OK -- %d driver/device-type pairs checked, "
          "%d allow-listed as not-yet-covered." % (len(drivers), len(ALLOWLIST)))
    return 0


def self_test():
    """Regression guard for this script's own regexes, run with --self-test.

    Exercises device_types_in_text() and stress_tag_sets_in_text() against
    synthetic snippets so a future edit to either regex gets caught here
    instead of only showing up as a silently wrong (vendor, device_type)
    pair. Not run as part of the normal check (no repo state needed).
    """
    checks = []

    def check(name, condition):
        checks.append((name, condition))

    # A DeviceType:: mention earlier in the file (a comment, here) must not
    # be picked up ahead of the actual override.
    decoy = """
        // Historically this was DeviceType::Camera before a refactor.
        class Foo : public FocuserDriver {
        public:
            DeviceType get_device_type() const override { return DeviceType::Focuser; }
        };
    """
    check("decoy DeviceType:: mention is ignored", device_types_in_text(decoy) == {"focuser"})

    # Two classes in one file agreeing is fine (the real gemini_flatpanel_driver.cpp shape).
    agree = """
        class A : public CoverCalibratorDriver {
            DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }
        };
        class B : public CoverCalibratorDriver {
            DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }
        };
    """
    check("two classes agreeing yields one value", device_types_in_text(agree) == {"covercalibrator"})

    # Two classes disagreeing must be flagged as ambiguous (len > 1), not
    # resolved by picking whichever comes first.
    disagree = """
        class A : public FocuserDriver {
            DeviceType get_device_type() const override { return DeviceType::Focuser; }
        };
        class B : public SwitchDriver {
            DeviceType get_device_type() const override { return DeviceType::Switch; }
        };
    """
    check("disagreeing overrides are detected as ambiguous", len(device_types_in_text(disagree)) > 1)

    # No override at all.
    check("no override yields no types", device_types_in_text("// nothing here") == set())

    # A comma inside the TEST_CASE description must not cut the tag match
    # short (the exact bug fixed for #269's non-blocking review note).
    comma_desc = 'TEST_CASE("Foo, bar - baz", "[vendor][focuser][stress]") {}'
    check("a comma in the description doesn't drop the tags",
          stress_tag_sets_in_text(comma_desc) == [{"vendor", "focuser", "stress"}])

    # String-literal concatenation ("a" "b") in the description.
    concat_desc = 'TEST_CASE("Foo" " - bar", "[vendor][focuser][stress]") {}'
    check("concatenated string literals in the description still match",
          stress_tag_sets_in_text(concat_desc) == [{"vendor", "focuser", "stress"}])

    # A TEST_CASE with no [stress] tag must not be picked up.
    non_stress = 'TEST_CASE("Foo", "[vendor][focuser][unit]") {}'
    check("a non-[stress] TEST_CASE is excluded", stress_tag_sets_in_text(non_stress) == [])

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
    if failed:
        print("\n%d/%d self-test(s) failed." % (len(failed), len(checks)))
        return 1
    print("\nAll %d self-test(s) passed." % len(checks))
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
