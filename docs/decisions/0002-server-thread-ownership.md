# HTTP server thread ownership

## Context

`run_server()` may return before the server is running, including when `bind()` fails. `running_ == false` does not imply `server_thread_` is non-joinable. A simple join from two concurrent `stop()` callers is also unsafe.

## Decision

`join_server_thread()` moves the thread under `server_thread_mutex_`, so exactly one caller owns the join. The join happens outside the mutex; other callers wait for its completion before returning to code that may destroy the server. A generation check keeps a waiter from adopting a newly started thread. The completion notification occurs while the mutex is held, before the last access to the condition variable. `stop()`, `wait()`, `start_async()`, and teardown use this ownership protocol. Ownership is released on every exit path: a scope guard clears the in-flight flag and notifies whether the join returns or throws, and a `join()` that fails parks the thread with the orphans the destructor reaps rather than destroying it joinable.

## Alternatives rejected

Return early when `running_` is false: leaves a failed-start thread joinable. Join directly from each caller: concurrent joins are undefined behavior. Detach the worker or let a losing caller return while another joins: teardown can free state the worker still uses. Hold the ownership mutex through `join()`: can deadlock while the accept loop unwinds.

## Consequences

A failed bind remains a recoverable start failure. A stop caller returns only after the thread for its generation is gone. Any new path that reads, moves, assigns, or joins `server_thread_` must participate in the same protocol.

## Links

- Issue [#402](https://github.com/open-astro/AlpacaBridge/issues/402); PR [#428](https://github.com/open-astro/AlpacaBridge/pull/428).
- Implementation: `AlpacaHTTP/src/http/server.cpp`, `AlpacaHTTP/include/alpacahttp/server.h`.
- Regression tests: `AlpacaHTTP/tests/test_server_socket.cpp` port-conflict, same-object retry, and concurrent-stop cases.
