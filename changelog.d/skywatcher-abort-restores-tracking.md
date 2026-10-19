### Fixed

- **AbortSlew during a MoveAxis jog no longer turns tracking off**, Sky-Watcher driver. AbortSlew cleared tracking whatever it aborted, so stopping a jog with AbortSlew left the mount untracked, and a later goto landed untracked as well. ASCOM's ITelescopeV4 requires AbortSlew to return Tracking to its state before a MoveAxis; the driver now restarts tracking after the stop when it was on before the jog. Aborting a goto, Park or FindHome is unchanged. Covered by two cases in `AlpacaCore/tests/test_skywatcher_async.cpp`.
