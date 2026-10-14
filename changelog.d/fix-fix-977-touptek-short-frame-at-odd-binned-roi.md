### Fixed
- **ToupTek exposure at an odd binned subframe no longer fails** (AlpacaCore, issue #977): at bin 2 or more the SDK rounds the binned output down to even, so an odd NumX or NumY arrived one column or row short and the 5.0.0-beta.3 frame check refused it with "ToupTek frame is smaller than the requested ROI". A shortfall of exactly one column and/or one row at bin 2 or more is now filled by repeating the last delivered column/row, and the image keeps the requested NumX x NumY. A shortfall of two or more, or any shortfall at bin 1, still fails with DriverException.

### Added (tests)
- **ToupTek odd binned ROI cases** (issue #977): cover odd NumX at bin 2, odd NumY at bin 3, both odd at bin 4 with the edge fill checked against its neighbour, and the refused two-pixel and bin 1 shortfalls.
