### Breaking changes
- **A connect no longer powers on a power port the boot left off** (ZWO ASIAIR Pro / Plus CM4, ToupTek StellaVita, iOptron iMate, issue #300): the switch used to claim its GPIO lines driven high, so on a board without the `gpio=...=op,dh` line in `config.txt` the first Connect turned every port on. Now the ports stay as they are and read as they are. **After upgrading:** to have the ports powered at boot, add the `gpio=` line from `AlpacaCore/PowerPorts.md`, or set the device's **At server start** to **Restore the last setting** and turn the ports on once.

### Added
- **Power-on state for the 12V power ports** (ZWO ASIAIR Pro / Plus CM4 / Plus RK3568, ToupTek StellaVita, iOptron iMate, issue #300): a new **At server start** setting (`powerOnState`) chooses what the ports do when AlpacaBridge starts: `leave` (default, as before), `restore` (put back the value a client last set) or `off` (every switchable port off after a reboot; a service restart or update restores the last setting instead, so gear in use keeps power). AlpacaBridge remembers the last value set on each port in `config/power_port_states.json`, applies the setting once per start and never on Connect, and keeps the lines claimed afterwards so a dimmed dew heater stays dimmed. Decision record [0021](docs/decisions/0021-power-port-power-on-state.md).

### Fixed
- **Power ports report what they are doing at connect** (ZWO ASIAIR Pro / Plus CM4, ToupTek StellaVita, iOptron iMate, issue #300): a connect read every port as on, whatever the board held; it now reads each line, and a reconnect still resumes a dimmed port's duty. For a board without the `gpio=` boot line, see Breaking changes.

### Added (tests)
- **Power-on state and line-level cases** (issue #300): the power port state store, the ToupTek and iOptron switches over their fakes, the ASIAIR Pro wrapper over the libgpiod stub (which now keeps line levels across claims), the RK3568 over the ioctl stub, the router's start hook and the iMate switch as a tier-2 contract sweep host. Checked on hardware on the ASIAIR Plus CM4, ASIAIR Pro (Pi 4), StellaVita and iMate; the RK3568 only over its ioctl stub.
