### Fixed (docs)
- **ToupTek short-frame release note** (issue #978): clarify that frames smaller than the requested ROI fail with DriverException instead of being zero-padded, except a shortfall of exactly one column or row at bin 2 or more, which is edge-filled (issue #977).
