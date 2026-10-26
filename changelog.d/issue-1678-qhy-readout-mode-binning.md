### Fixed
- **QHY camera: a ReadoutMode change keeps binning and sizes the ROI to it** (AlpacaCore, issue #1678): changing `ReadoutMode` while `BinX` was above 1 reset `NumX`/`NumY` to the unbinned sensor but kept the binning, so the next `StartExposure` failed with `InvalidValue` ("ROI exceeds sensor area for current binning"). The mode change now re-applies the SDK bin mode after the re-initialization and resets the ROI to the binned full sensor, the way a `BinX` write does.

### Added (tests)
- **QHY readout mode and binning case** (issue #1678): one unit case that sets binning to 2, changes readout mode, checks the binned ROI and the re-applied bin mode, and takes an exposure.
