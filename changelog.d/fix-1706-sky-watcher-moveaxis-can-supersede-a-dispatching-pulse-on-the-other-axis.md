### Fixed

- **Sky-Watcher MoveAxis no longer drops a PulseGuide on the other axis**, Sky-Watcher driver, issue #1706. A MoveAxis on one axis that landed while a PulseGuide on the other axis was stop-waiting its own axis made the pulse throw "Motion superseded before dispatch" and never start. A pulse dispatch now follows a speed start on the other axis; goto, abort and stop commands still supersede it. Covered by `SkyWatcher async - MoveAxis(RA, rate) during a Dec pulse's dispatch stop-wait does not drop the pulse (#1706)` in `AlpacaCore/tests/test_skywatcher_async.cpp`.
