### Added
- **Celestron focus motor driver** (AlpacaCore, AlpacaHTTP, issue #896): new `celestron` Focuser on the motor's own USB port (AUX device 0x12, 9600 baud), configured with an explicit `portPath`. Absolute Move clamped to the calibrated range, Halt, `Position`/`IsMoving` from a 250 ms state poll, firmware in the web UI. An uncalibrated motor refuses the connect. Not yet validated on hardware.

### Added (tests)
- **Celestron focus motor cases** (issue #896): driver contract and PTY-fake protocol cases in `test_celestron_focuser.cpp`, three `[stress]` cases in `test_celestron_focuser_concurrency_stress.cpp`, a no-reopen resync case in `test_celestron_cgx_protocol.cpp`, the config round-trip and missing-port refusal in `test_routing.cpp`, and the web form in `celestron_focuser.test.js`.
