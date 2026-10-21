### Changed (docs)

- **The LAN surface threat model records where AlpacaBridge departs from the Alpaca API listings** (docs, issue #1479): decision 0007 gains a Deliberate departures section for the Device API 403s, the 413 body cap, `PUT /management/v1/description` and its 400s, and the `configuredevice` (issue #765) and update channel (issue #1103) HTTP 400s, each with its reason, plus rule 13 that a new departure is listed there in the same change. The HTTP integration instructions now point at it. Docs only, no behaviour change.
