### Fixed (tests)

- **Sky-Watcher stale-link reconnect test no longer races the status poll**, Sky-Watcher tests. The redundant-connect check in `AlpacaCore/tests/test_skywatcher_serial.cpp` counted every frame on the fake board, so a 100 ms status-poll `j`/`f` query landing between the two reads failed it (`16 == 15` in the coverage job on `main` at `abd42bbf`); it now counts only the `:e` identity query that a real connect sends.
