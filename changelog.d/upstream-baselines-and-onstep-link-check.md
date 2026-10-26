### Fixed (tooling)

- **The weekly link check skips the vendored OnStepX command reference** (`.github/workflows/link-check.yml`, issue #1684): `AlpacaCore/external/OnStep/COMMAND_REFERENCE.md` is an unmodified copy of OnStepX's `docs/COMMAND_REFERENCE.md`, and its relative links to `CAN_NOTES.md`, `GOTO_NOTES.md` and `STARTUP_AUTHORITY_NOTES.md` name sibling files in OnStepX that are not vendored here. Both lychee passes now exclude that one file; the copy itself stays unedited.

### Changed (tooling)

- **Two upstream baselines are set to the reviewed versions** (`scripts/dependency_sdk_versions.json`): `indi-zwo-am5` moves to INDI `790c53f5` (issue #1682). That commit makes INDI's AM5 driver park straight to a custom park position with `:hP#` instead of going home with `:hC#` first; the AlpacaCore ZWO driver already sends only `:hP`, so no driver change follows. `libraw` moves to 0.22.3 (issue #1683), a bugfix-only release (CR3/CRX, X3F, DNG and TIFF decoder hardening, no new camera formats) reviewed against the GPhoto driver; the build still takes LibRaw from Debian trixie, which ships 0.21.4, so nothing in the package changes.
