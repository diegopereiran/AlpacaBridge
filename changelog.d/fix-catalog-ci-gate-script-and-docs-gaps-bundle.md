### Fixed
- **Skipped persisted device warning no longer logs a raw `deviceType`** (AlpacaHTTP, issue #1292): a saved entry with a type the server does not know is skipped with a warning that quotes the saved `vendor/deviceType`; control bytes in it are now escaped as `\xNN`, as the debug line from `sanitize_device_config()` already did (test "persisted deviceType with a control byte is escaped in the log" in `test_routing.cpp`).
- **`PowerPorts.md` no longer tells ASIAIR and StellaVita users to override the GPIO chip** (docs, issue #1292): the setup steps now say the chip is fixed at `/dev/gpiochip0` and only the PWM frequency can change.

### Fixed (tooling)
- **The `Falsified by` gate keys base test cases by file** (`scripts/check_falsified_by.py`, issue #1292): a new case named like a case that stays in another file is now reported as new; a case cut from one file and added to another is still a moved case. Covered by the scratch-repo self-test.
- **The contract-sweep gate checks each catalog file's vendor** (`scripts/check_contract_sweep.py`, issue #1292): a `DeviceKey` vendor id that differs from the file's directory fails with `CATALOG VENDOR MISMATCH`, and a file with no `DeviceKey` literal fails with `CATALOG VENDOR UNVERIFIED`, so the `ORPHAN ENTRY` exemption cannot go to the wrong vendor.

### Changed (tests)
- **Bisque routing pin for the `siteLatitude` range message** (AlpacaHTTP tests, issue #1292): the API refusal and the saved-config warning for `siteLatitude` 200 are pinned in `test_routing.cpp`, as for Sky-Watcher.
