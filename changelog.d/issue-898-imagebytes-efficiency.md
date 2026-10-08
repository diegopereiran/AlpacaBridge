### Changed
- **Camera ImageBytes use less memory and transfer faster** (AlpacaHTTP, issue #898): pack pixels directly into the final payload, move temporary response bodies into ownership, and send headers and bodies with one vectored write.

### Added (tests)
- **ImageBytes and large binary response coverage** (AlpacaHTTP, issue #898): verify pixel ordering and element encodings, arithmetic validation, and large-body framing, keep-alive, embedded NULs, and peer disconnects.
