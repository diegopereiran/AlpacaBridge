### Fixed

- **Sky-Watcher serial settle no longer ends early on a spurious-readable port**, Sky-Watcher driver, issue #1291: the drain after a mis-paired or malformed reply now keeps polling the rest of its window (and retries on `EINTR`) instead of stopping at the first read that returns nothing, so a late straggler cannot reach the next exchange (`SkyWatcher serial - the settle drain keeps polling after a spurious-readable EAGAIN`).

### Fixed (tests)

- **Sky-Watcher pulse restore hook test guard**, Sky-Watcher driver tests, issue #1291: the pulse-restore-hook cases use an RAII guard that releases the parked task and clears the process-wide hook when a `REQUIRE` fails (`SkyWatcher async - the pulse restore hook guard clears the hook when a REQUIRE unwinds`), and the malformed-reply give-up path is pinned (`SkyWatcher serial - giving up on a second malformed reply throws and settles the line`).
