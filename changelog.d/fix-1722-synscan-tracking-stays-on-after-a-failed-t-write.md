### Fixed

- **SynScan no longer reports Tracking true after a failed tracking restore** (SynScan, issue #1722): when the `T` write that restores tracking after AbortSlew, a MoveAxis stop, a timed move or the end of an RA pulse-guide fails, the cached tracking mode is dropped and the next `Tracking` read asks the handset (tests "failed tracking restore after an RA stop drops the cached mode" and "a failed tracking restore after the RA landing move drops the cached mode").
