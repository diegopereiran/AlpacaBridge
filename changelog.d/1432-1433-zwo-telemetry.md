### Breaking changes
- **ZWO AM: unreliable telemetry now raises an error instead of returning indefinitely stale state** (AlpacaCore, issues #1432, #1433): missing initial measurements and three failed mandatory polls return DriverException; persistent faults or stalled polling become NotConnected and require an explicit reconnect. **After upgrading:** clients must handle unavailable/compromised telemetry and reconnect after link loss; saved device settings are unchanged.

### Fixed
- **ZWO AM: property reads no longer wait for mount telemetry** (AlpacaCore, issues #1432, #1433): serve measured snapshots, retain coordinate-before-arrival ordering, evaluate Park in the poll, and bound telemetry failures without suppressing safety-stop traffic; for changed error behavior, see Breaking changes.
