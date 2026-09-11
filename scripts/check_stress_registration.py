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
# Every place a TEST_CASE can compile into alpacacore_tests. Headers are
# included because the tests/ helpers (fake_mount_server.h and friends) are
# #included by several TUs, so a TEST_CASE added to one would compile in and
# re-inflate the vendor threshold while slipping past a *.cpp-only scan.
#
# The .cpp glob is deliberately *.cpp and not test_*.cpp: AGENTS.md states the
# rule as "a [stress] case anywhere else under AlpacaCore/tests/", and a
# narrower glob would leave a file not named test_* outside the check while the
# prose said otherwise. Registration files are excluded by path in
# find_stray_stress_cases(), not by failing to match here.
TEST_GLOBS = ("AlpacaCore/tests/*.cpp", "AlpacaCore/tests/*.h")

# Anchored to the actual override, not just any DeviceType:: mention in the
# file -- a driver that referenced a different DeviceType::X earlier (a
# comment, a switch/comparison, a helper) before its own override would
# otherwise be silently miscategorized by a plain first-match search.
DEVICE_TYPE_OVERRIDE_RE = re.compile(
    r"get_device_type\s*\(\s*\)\s*const\s+override\s*\{\s*return\s+DeviceType::([A-Za-z]+)\s*;")
# The description is matched as one or more adjacent string literals
# (escapes allowed, `"a" "b"` concatenation allowed) so a comma inside it
# cannot cut the match short and silently drop the tags.
#
# Whitespace BETWEEN tags is allowed because Catch2 allows it: "[a][b]" and
# "[a] [b]" are the same two tags to Catch2, but a pattern demanding one
# unbroken run of brackets sees only the first form. That gap silently
# un-registers a case from this gate -- and from the [stress-guard] rejection
# below -- for a purely cosmetic difference in how someone typed the tags.
TEST_CASE_TAGS_RE = re.compile(
    r'TEST_CASE\s*\(\s*(?:"(?:[^"\\]|\\.)*"\s*)+,\s*"(\s*(?:\[[^\]]+\]\s*)+)"')
TAG_RE = re.compile(r"\[([^\]]+)\]")

# (vendor, device type) pairs with no [stress] TEST_CASE yet. Seeded from the
# gap found when this check was introduced (2026-09) so the check starts
# green; each line is a driver this repo already knows is uncovered.
#
# Remove an entry the same PR that adds its [stress] coverage -- a
# still-covered entry left behind is itself a failure (see main()), so
# nothing here can silently go stale.
ALLOWLIST = {
    ("gemini", "covercalibrator"),
    ("gemini", "focuser"),
    ("gemini", "switch"),
    ("playerone", "switch"),
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
    """({(vendor, device_type)} covered by a [stress] TEST_CASE, [guard findings]).

    Matches tags against the vendor/device-type vocabulary the driver scan
    itself found, rather than assuming a fixed tag order -- a TEST_CASE is
    tagged [vendor][device_type][stress] plus sometimes more (e.g.
    [round-4]), and this only needs to find the two tags that are actually a
    known vendor and a known device type.
    """
    registered = set()
    guard_failures = []
    # One pass over each file: the [stress-guard] rejection below needs the
    # same text, so reading it twice only costs I/O.
    for path in tracked_files(STRESS_TEST_GLOB_PREFIX + "*" + STRESS_TEST_GLOB_SUFFIX):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for tags in stress_tag_sets_in_text(text):
            vendor_tags = [t for t in tags if t in known_vendors]
            dtype_tags = [t for t in tags if t in known_device_types]
            for vendor in vendor_tags:
                for dtype in dtype_tags:
                    registered.add((vendor, dtype))
        for case in guard_tagged_cases_in_text(text):
            guard_failures.append(
                "[stress-guard] IN A REGISTRATION FILE: %s uses [stress-guard] "
                "without [stress], so it does not count toward vendor coverage "
                "here. Use [stress] for a vendor registration; [stress-guard] is "
                "for harness self-tests only: %s" % (path, case)
            )
    return registered, guard_failures


def guard_tagged_cases_in_text(text):
    """The matched TEST_CASE(...) headers tagged [stress-guard] but NOT [stress].

    Each entry is the whole matched prefix (name + tag string), not just the
    description -- that is what the failure message quotes, so a reader can
    see which tags were actually written.

    [stress-guard] exists for harness self-tests that need ThreadSanitizer but
    are not vendor registrations (issue #322); it runs under its own TSan
    invocation, deliberately outside the vendor-coverage count this script
    gates. A registration file that reached for it INSTEAD of [stress] would
    still get TSan, still look registered to a reader, and silently drop out
    of that count -- so the pair would go uncovered without appearing in
    ALLOWLIST. Reject the tag in these files; the harness self-test that owns
    it lives in test_stress_call_guard.cpp, which this glob never scans.
    """
    found = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = {t.lower() for t in TAG_RE.findall(m.group(1))}
        if "stress-guard" in tags and "stress" not in tags:
            found.append(m.group(0))
    return found


def stray_stress_cases_in_text(text):
    """The matched TEST_CASE(...) headers tagged [stress] -- for files OUTSIDE
    the *_concurrency_stress.cpp glob, where that tag does not belong.

    This is the direction that actually went wrong: test_async_connectable.cpp
    carried [stress] from the unconditional TEST_SOURCES block, so with every
    vendor target absent `alpacacore_tests "[stress]"` still matched one case,
    printed "All tests passed (... in 1 test case)", and the CI zero-coverage
    grep accepted it -- sanitizers-tsan went green with no vendor concurrency
    coverage at all. The tag was moved to [stress-guard]; this check is what
    stops the next one being added.

    Core/harness self-tests that need TSan use [stress-guard], which has its
    own invocation and its own zero-test grep.
    """
    found = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = {t.lower() for t in TAG_RE.findall(m.group(1))}
        if "stress" in tags:
            found.append(m.group(0))
    return found


def find_stray_stress_cases():
    """[findings] for [stress]-tagged TEST_CASEs outside the registration glob."""
    registrations = set(
        tracked_files(STRESS_TEST_GLOB_PREFIX + "*" + STRESS_TEST_GLOB_SUFFIX))
    failures = []
    candidates = []
    for glob in TEST_GLOBS:
        candidates.extend(tracked_files(glob))
    for path in sorted(set(candidates)):
        if path in registrations:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for case in stray_stress_cases_in_text(text):
            failures.append(
                "[stress] OUTSIDE A REGISTRATION FILE: %s tags a TEST_CASE "
                "[stress], but that tag is reserved for vendor driver "
                "registrations in %s*%s -- this file compiles unconditionally, "
                "so the case alone satisfies CI's vendor zero-coverage grep and "
                "makes it vacuous. Use [stress-guard] for a core/harness "
                "self-test that needs TSan: %s"
                % (path, STRESS_TEST_GLOB_PREFIX, STRESS_TEST_GLOB_SUFFIX, case)
            )
    return failures


def main():
    drivers, failures = find_drivers()
    failures = list(failures)  # find_drivers' own ambiguity findings, if any
    known_vendors = {v for v, _ in drivers}
    known_device_types = {d for _, d in drivers}
    registered, guard_failures = find_registered_pairs(known_vendors, known_device_types)
    failures.extend(guard_failures)
    failures.extend(find_stray_stress_cases())

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

    It also drives main() end to end over temp files with tracked_files()
    patched, which covers the two tag predicates AND their wiring: deleting
    either `failures.extend(...)` line in main() fails a check here rather
    than passing silently. Keep that property when adding cases -- a predicate
    tested only through its own function leaves the wiring unpinned.
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

    # [stress-guard] must not stand in for [stress] in a registration file:
    # it gets its own TSan invocation but is deliberately outside the vendor
    # coverage count, so a registration wearing it would silently go
    # uncovered while still looking registered.
    guard_only = 'TEST_CASE("Foo", "[vendor][focuser][stress-guard]") {}'
    check("a [stress-guard]-only TEST_CASE is rejected in a registration file",
          len(guard_tagged_cases_in_text(guard_only)) == 1)
    check("a [stress-guard]-only TEST_CASE is not counted as [stress] coverage",
          stress_tag_sets_in_text(guard_only) == [])

    # Carrying both is fine -- the case still counts as vendor coverage, and
    # the extra tag only adds it to the second TSan invocation.
    both_tags = 'TEST_CASE("Foo", "[vendor][focuser][stress][stress-guard]") {}'
    check("a TEST_CASE tagged both [stress] and [stress-guard] is allowed",
          guard_tagged_cases_in_text(both_tags) == [])
    check("a TEST_CASE tagged both still counts as [stress] coverage",
          stress_tag_sets_in_text(both_tags) == [{"vendor", "focuser", "stress", "stress-guard"}])

    # Catch2 treats "[a][b]" and "[a] [b]" as the same two tags, so a pattern
    # demanding one unbroken bracket run would silently un-register a case
    # over a cosmetic difference -- and would let a spaced [stress-guard] slip
    # past the rejection above.
    spaced = 'TEST_CASE("Foo", "[vendor] [focuser] [stress]") {}'
    check("whitespace between tags still registers as [stress] coverage",
          stress_tag_sets_in_text(spaced) == [{"vendor", "focuser", "stress"}])
    spaced_guard = 'TEST_CASE("Foo", "[vendor] [focuser]  [stress-guard]") {}'
    check("whitespace between tags does not let [stress-guard] evade the check",
          len(guard_tagged_cases_in_text(spaced_guard)) == 1)

    # The other direction, which is the one that actually went wrong: a
    # [stress] tag in a file outside the registration glob re-inflates the
    # vendor threshold and makes CI's zero-coverage grep vacuous again.
    stray = 'TEST_CASE("Foo", "[async_connectable][stress]") {}'
    check("a [stress] TEST_CASE outside the glob is rejected",
          len(stray_stress_cases_in_text(stray)) == 1)
    guarded = 'TEST_CASE("Foo", "[async_connectable][stress-guard]") {}'
    check("a [stress-guard] TEST_CASE outside the glob is fine",
          stray_stress_cases_in_text(guarded) == [])
    unrelated = 'TEST_CASE("Foo", "[async_connectable][unit]") {}'
    check("an untagged-for-stress TEST_CASE outside the glob is fine",
          stray_stress_cases_in_text(unrelated) == [])

    # The predicate above is well covered, but main()'s USE of it was not:
    # deleting `failures.extend(guard_failures)` left every check green. Drive
    # main() end to end over synthetic files so the wiring is pinned too.
    import os
    import tempfile

    real_tracked_files = globals()["tracked_files"]
    with tempfile.TemporaryDirectory() as tmp:
        stress = os.path.join(tmp, "test_fakevendor_concurrency_stress.cpp")
        core = os.path.join(tmp, "test_fakecore.cpp")

        def fake_tracked_files(pattern):
            # No drivers at all: with nothing to be uncovered, a non-zero exit
            # can ONLY come from the tag wiring under test. Returning []
            # says that outright rather than relying on a temp path happening
            # to be shallow enough that find_drivers() skips it.
            if pattern.endswith("_driver.cpp"):
                return []
            # The broad .cpp glob must return the non-registration file too --
            # otherwise find_stray_stress_cases() skips everything it is handed
            # (all of it is in `registrations`) and returns [] no matter what,
            # leaving its main() wiring untestable. That was a real hole:
            # deleting `failures.extend(find_stray_stress_cases())` left every
            # check here green until this fixture grew the second file.
            #
            # Keyed on the pattern itself rather than TEST_GLOBS[0]: reordering
            # that tuple would otherwise silently hand the .cpp glob [] and
            # re-open exactly the hole described above.
            if pattern == "AlpacaCore/tests/*.cpp":
                return [stress, core]
            if pattern in TEST_GLOBS:
                return []
            return [stress]

        def run_main_with(stress_source, core_source=""):
            with open(stress, "w", encoding="utf-8") as fh:
                fh.write(stress_source + "\n")
            with open(core, "w", encoding="utf-8") as fh:
                fh.write(core_source + "\n")
            globals()["tracked_files"] = fake_tracked_files
            saved_allowlist = set(ALLOWLIST)
            ALLOWLIST.clear()
            try:
                return main()
            finally:
                globals()["tracked_files"] = real_tracked_files
                ALLOWLIST.clear()
                ALLOWLIST.update(saved_allowlist)

        clean = 'TEST_CASE("Ok", "[fakevendor][camera][stress]") {}'
        check("main() passes when a registration file uses [stress]",
              run_main_with(clean) == 0)
        offending = 'TEST_CASE("Bad", "[fakevendor][camera][stress-guard]") {}'
        check("main() FAILS when a registration file uses [stress-guard] alone",
              run_main_with(offending) == 1)
        # Pins that the rejection is PER CASE, not per file. Every other
        # fixture here holds a single TEST_CASE, so a per-file implementation
        # (union the file's tags, then test) would pass all of them
        # identically while letting this one through -- the file as a whole
        # carries [stress], but the second case alone does not, and that case
        # is the one that silently drops out of the vendor coverage count.
        mixed = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {}\n'
                 'TEST_CASE("Bad", "[fakevendor][camera][stress-guard]") {}')
        check("main() FAILS on a [stress-guard]-only case beside a [stress] one",
              run_main_with(mixed) == 1)

        # The sibling wiring: a [stress] case in a NON-registration file is the
        # regression that made CI's vendor zero-coverage grep vacuous.
        stray = 'TEST_CASE("Stray", "[fakecore][stress]") {}'
        check("main() FAILS when a non-registration file uses [stress]",
              run_main_with(clean, stray) == 1)
        guarded = 'TEST_CASE("Fine", "[fakecore][stress-guard]") {}'
        check("main() passes when a non-registration file uses [stress-guard]",
              run_main_with(clean, guarded) == 0)

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
