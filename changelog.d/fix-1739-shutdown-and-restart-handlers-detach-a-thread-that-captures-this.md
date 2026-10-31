### Fixed

- **POST shutdown and restart no longer leave a detached thread reading the Router** (AlpacaHTTP, issue #1739). The thread behind each handler now owns a copy of the callback instead of reading the Router's member 100 ms later, so a Router torn down or re-armed in that window cannot be dereferenced. Covered by `AlpacaHTTP/tests/test_router_shutdown.cpp`, which swaps the callback after the POST and asserts the original runs once and the replacement never.
