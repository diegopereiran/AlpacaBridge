# Rotator Sync offset persistence

Status: accepted

## Context

ASCOM IRotatorV4 `Sync` sets an offset between `Position` and `MechanicalPosition`, and the offset must survive a driver restart and a device reboot. The WandererAstro rotator and the ZWO CAA kept it in memory and reset it to 0 on every connect, so `Position` fell back to the mechanical angle after a reconnect (issue #777, SP-19a). Neither device stores the offset itself.

## Decision

One helper, `alpacacore::util::RotatorSyncOffsetStore` (`AlpacaCore/include/alpacacore/util/rotator_sync_offset_store.h`), keeps the offsets in a JSON object in `config/rotator_sync_offsets.json`, relative to the working directory. Key: the device's persisted GUID, its `get_unique_id()` ([decision 0018](0018-persisted-unique-id.md)). Value: the offset in degrees.

- `Sync` saves the new offset (temp file, then rename; other keys kept; one mutex for all calls).
- Connect loads it through `load_or_adopt()`: with no entry under the GUID, an entry under the driver's old ID (the device-number ID for WandererAstro, the serial-based ID for the ZWO CAA, which the connect reads) is moved to the GUID once. No entry, a missing file or a corrupt file gives 0.
- If `Sync` cannot save the new offset, it throws `DriverException` and keeps the previous offset, so `Position` and the stored offset still agree. ASCOM requires the offset to persist, so a `Sync` that cannot persist it has failed.
- A Connect that cannot read the file logs a WARNING and uses 0; Connect never fails for the offset.
- A removed rotator's entry is deleted at the next start (`prune_unknown_unique_ids()`, called by the router's load), not by `removedevice`: the web UI edits a device by removing and re-adding it with the same GUID, and a delete at removal would wipe the offset on every edit. A new device that takes the same device number gets a new GUID, so it starts with `Position` equal to `MechanicalPosition` at once.
- AlpacaCore has no JSON library, so the helper reads and writes only a flat object of numbers.
- Tests set the path with `RotatorSyncOffsetStore::set_default_path()` and restore it; factory signatures do not change.

The `Sync` failure and clearing a removed rotator's offset were decided 2026-10-21 on issue #1470 (item 3) and landed with #1471 and #1473. Before that, a storage failure logged a WARNING and `Sync` still succeeded, the key was the driver-built ID, and removal left the entry in the file.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A rotator `Sync` saves its offset through `util::RotatorSyncOffsetStore`, keyed by `get_unique_id()`, and Connect loads it with `load_or_adopt()` from its old ID. Scope: `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`, `AlpacaCore/tests/test_wandererastro_rotator.cpp`, `AlpacaCore/tests/test_zwo_rotator.cpp`.
2. A `Sync` whose save fails throws `DriverException` and leaves the previous offset in place. A Connect that cannot read the file uses 0 and logs a WARNING; a missing or corrupt file reads as 0. Scope: `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp`. Checked by: `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`, and the "Sync that cannot save throws and keeps the offset" cases in `AlpacaCore/tests/test_wandererastro_rotator.cpp` and `AlpacaCore/tests/test_zwo_rotator.cpp`.
3. The offset value never passes through the device config or the router. The router's only use of the store is pruning, at load, the GUID keys no stored entry holds. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("a start drops the sync offset of a rotator no longer configured").

## Alternatives rejected

- A field in the device config (`registered_devices.json`): the offset would pass through the router, `sanitize_device_config` and a config write on every `Sync`, and mixes a runtime calibration with user settings.
- A constructor parameter for the path: changes every factory and the router for a test-only need.
- Writing the offset to the device: neither device has a field for it.

## Consequences

- Moving the state file orphans a saved offset; the rotator then reads 0 until the next `Sync`.
- A removed rotator's offset stays in the file until the next start. Nothing can read it in between: a new device gets a new GUID.
- An offset saved under an old ID whose rotator never connects after the upgrade stays in the file, since pruning removes only GUID keys.
- A client whose `Sync` used to succeed on a read-only or full disk now gets `DriverException`.
- To reverse: remove the load and save calls in the two drivers and delete the helper; nothing else reads the file.

## Links

- Issue #777 (SP-19a); [decision 0018](0018-persisted-unique-id.md); issues [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 3), [#1471](https://github.com/open-astro/AlpacaBridge/issues/1471), [#1473](https://github.com/open-astro/AlpacaBridge/issues/1473).
