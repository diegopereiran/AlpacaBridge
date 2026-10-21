# Persisted UniqueID per configured device

Status: accepted

## Context

The Management API's configured-devices list gives every device a `UniqueID`. ASCOM says it should be "a GUID for native Alpaca devices" ([AlpacaConfiguredDevice.UniqueID](https://ascom-standards.org/library/html/P_ASCOM_Alpaca_Discovery_AlpacaConfiguredDevice_UniqueID.htm)). Clients use it to recognise a device they have seen before, across restarts and across a change of device number.

AlpacaBridge serves it from each driver's `get_unique_id()` (`DeviceRegistry`, `AlpacaCore/src/management/device_registry.cpp`). On `main` 313cf184, 25 vendor drivers build that string from the device number (for example `"ASTROASIS_FOCUSER_" + std::to_string(device_number_)` in `astroasis_focuser_driver.cpp`). Two AlpacaBridge hosts on one LAN both report `SkyWatcher_0`, and renumbering a device changes its ID. Other drivers use a hardware serial (ZWO `ZWO_SN_<serial>`, ToupTek), and the ZWO camera stores a generated `ZWO_UID_<16 hex>` in its config when the camera has no serial (#914, #1166). A device that failed to load is listed by `faileddevices` with an empty `UniqueID`.

The rotator sync offset store is keyed by `get_unique_id()` ([decision 0008](0008-rotator-sync-offset-persistence.md)), so the ID is also an on-disk key.

Decided 2026-10-21 on issue #1470 (item 1): every configured device gets a GUID stored in its config, and existing devices get a new GUID once on upgrade.

## Decision

- **One ID per configured device.** Each entry in the persisted device config carries a `uniqueId` field holding a random (version 4) UUID in its canonical lowercase text form. It is created when the device is first configured and never changes while the entry exists. Editing the entry, renumbering it or replacing the hardware behind it keeps it. Removing the entry ends it; a device added later gets a new one.
- **The server chooses it.** On `configuredevice` the router keeps a `uniqueId` the client sends only when it is a version 4 UUID that no other stored entry holds; otherwise it mints one. The web UI edits a device by removing the entry and adding it again, so the form sends the stored GUID back, and the removal has just freed it. A value in any other form (a ZWO `ZWO_UID_`, a device-number ID) is replaced. `sanitize_device_config` keeps the field for every device type.
- **Every reader sees the same value.** The router calls `AlpacaDriver::assign_unique_id()` with the stored GUID before it registers the driver, and the base class's non-virtual `get_unique_id()` returns it, so `configureddevices`, logs and the rotator offset store all use one ID. A driver keeps a `fallback_unique_id()`, answered only when no GUID was assigned, which happens only for a driver a unit test builds directly. A device that failed to load is listed by `faileddevices` with its stored GUID, not an empty string.
- **Hardware identity stays separate.** A serial number or SDK index still selects which physical device a driver binds to. It no longer forms the `UniqueID`.
- **Upgrade.** On the first start of a version with this rule, every entry without a valid `uniqueId` gets one, a GUID two entries share is replaced in the later one, and the config is written back once. The ZWO camera's stored `ZWO_UID_` value is replaced like any other ID, and the ZWO catalog no longer declares, reads or learns the key.
- **Rotator offsets follow the GUID.** A rotator's first connect after the upgrade moves the offset saved under its old ID to the GUID (`RotatorSyncOffsetStore::load_or_adopt()`). The move happens at connect, not at load, because the ZWO CAA's old ID is built from the serial it reads at connect. This is why issue #1471 and issue #1473 land together ([decision 0008](0008-rotator-sync-offset-persistence.md)).

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. Every persisted device entry has a version 4 UUID in `uniqueId`, kept across edits and renumbering. A client-sent value is kept only when it is a valid GUID no other entry holds. Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("every configured device gets a persisted GUID as its UniqueID", "a stored entry gets a GUID once at load and keeps it across restarts", "a stored entry that fails to load still holds its GUID against a new device").
2. The router assigns the GUID before it registers a driver, and `get_unique_id()` is not virtual; a driver's own ID is only `fallback_unique_id()`. Scope: `AlpacaCore/include/alpacacore/alpacadriver.h`, `AlpacaCore/src/vendors/**`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` (the UniqueID listed equals the stored GUID), the compiler (an override of `get_unique_id()` does not build), review.
3. `faileddevices` lists a device that failed to load with its stored GUID (since #576 `configureddevices` lists only loaded devices). Scope: `AlpacaHTTP/src/http/router.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` (the persisted GUID case and the #576 faileddevices case).
4. A rotator moves the offset saved under its old ID to the GUID at its first connect. Scope: `AlpacaCore/src/vendors/*/*_rotator_driver.cpp`, `AlpacaCore/src/core/util/rotator_sync_offset_store.cpp`. Checked by: `AlpacaCore/tests/test_wandererastro_rotator.cpp`, `AlpacaCore/tests/test_zwo_rotator.cpp` (the "Connect adopts the offset" cases), `AlpacaCore/tests/test_rotator_sync_offset_store.cpp`.

## Alternatives rejected

- **Keep the device-number IDs.** They collide across hosts and change on renumbering, which is what the ID exists to prevent.
- **Derive the ID from a hardware serial where one exists.** It follows the camera rather than the configured device, half the supported hardware has no serial, and a swapped camera would silently inherit another device's identity in a client.
- **Hash the host name and device number.** Stable across restarts, but it still changes on renumbering, and a renamed host changes every ID.
- **Keep old IDs for existing devices and use GUIDs only for new ones.** Two ID schemes would stay in the code indefinitely, and the existing ones keep colliding.
- **Ignore every client-sent value.** The web UI's edit is a remove followed by an add, so every edit would mint a new GUID and orphan a rotator's offset.
- **Rename the offsets at load.** The ZWO CAA's old key needs the serial, which only the connect reads.

## Consequences

- Every existing device reports a new `UniqueID` once, after the upgrade. A client that stored the old ID sees a new device and must be pointed at it once. The changelog entry for issue #1471 goes under Breaking changes.
- Restoring an old config backup brings back the GUIDs it held; copying one config to a second host gives both hosts the same GUIDs, so a cloned SD card should be re-configured.
- A management client that edits a device by removing and re-adding it without sending the GUID back gets a new one; the web UI sends it.
- The ZWO camera's catalog no longer generates `ZWO_UID_` values. The direct factories that unit tests use still do, for the fallback.
- An offset saved under an old ID whose rotator never connects again stays in the file; pruning removes only GUID keys no entry holds.

## Links

- Issues [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 1), [#1471](https://github.com/open-astro/AlpacaBridge/issues/1471); earlier ZWO work [#914](https://github.com/open-astro/AlpacaBridge/issues/914), [#1166](https://github.com/open-astro/AlpacaBridge/issues/1166).
- [Decision 0008](0008-rotator-sync-offset-persistence.md) (offset store keyed by the ID).
- `.claude/skills/ascom-alpaca-protocol/references/management-and-discovery.md` (UniqueID requirements).
