### Changed

- **The gphoto camera driver links a bundled libgphoto2 2.5.34** (gphoto driver, packaging, issue #933): the upstream tarball is vendored in `AlpacaCore/external/libgphoto2/` and built by `scripts/build_bundled_gphoto.sh` into `/usr/lib/alpacabridge/libgphoto2`, with the default camlib set. The server finds it through a `DT_RPATH`, the directory is not registered with ldconfig, and the host's own libgphoto2 packages are untouched; the `.deb` no longer depends on `libgphoto2-6t64` or `libgphoto2-port12t64`. `scripts/check_bundled_gphoto.sh` checks this on an installed package, and the new `package-deb` CI job builds and installs the `.deb` and runs it. The camera udev rules (`61-alpacabridge-libgphoto2.rules`, group `plugdev`, mode 0660) are now generated from the bundled build. The release SBOM skips the built bundle. See [decision 0012](docs/decisions/0012-bundled-libgphoto2.md).
- **libgphoto2 updates are tracked as a vendored SDK** (dependency checks, issue #1137): the `libgphoto2` row is gone from `system_libraries` in `scripts/dependency_sdk_versions.json`; its `AlpacaCore/external/libgphoto2` entry (2.5.34) is checked against the upstream tags page, so the open update tracking for it (issue #933) is handled by this change.

### Security

- **libgphoto2 2.5.34 fixes CVE-2026-40333, CVE-2026-40334, CVE-2026-40335, CVE-2026-40336, CVE-2026-40338, CVE-2026-40339, CVE-2026-40340 and CVE-2026-40341** (gphoto camera driver, issue #933): Debian trixie ships 2.5.31, which does not have these fixes; the driver now uses the bundled 2.5.34 library.
