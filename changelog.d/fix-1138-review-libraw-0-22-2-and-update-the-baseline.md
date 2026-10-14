### Changed
- **Dependency baseline: libraw 0.22.2** (tooling, issue #1138): the libraw baseline in `scripts/dependency_sdk_versions.json` moves from 0.21.4 to 0.22.2, a bug-fix-only LibRaw release; the gphoto driver uses the same LibRaw calls and the package still takes libraw from Debian, so nothing is pinned or rebuilt.
