### Breaking changes
- **QHY camera: failed configuration no longer leaves trusted control readings** (AlpacaCore, issue #1673): Gain/Offset reads fail after a failed write, and their limits and MaxBinX/Y also fail after an incomplete mode change. **After upgrading:** retry the failed configuration or reconnect before reading affected controls.
- **QHY camera and integrated wheel opens wait for cooler OFF cleanup** (AlpacaCore, issue #1726): a new connection is refused while an old OFF operation is still in flight. **After upgrading:** retry after cleanup finishes; a permanently wedged vendor call requires restarting the server.

### Fixed
- **QHY camera control getters do not wait behind downloads** (AlpacaCore, issue #1673): cache Gain, Offset, their limits and MaxBinX/Y at connect and refresh after writes and mode changes, keeping actual HDR readback separate from requested restoration values; for failed configuration, see Breaking changes.
- **Explicit QHY cooler OFF survives disconnect** (AlpacaCore, issues #1673 and #1726): retain the original physical handle until one PWM-zero attempt after regulation stops, without blocking logical disconnect indefinitely or changing cooling on plain disconnect.
