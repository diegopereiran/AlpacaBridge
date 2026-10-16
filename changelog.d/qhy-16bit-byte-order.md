### Fixed
- **QHY: 16-bit frames are no longer byte-swapped** (AlpacaCore, QHY camera driver): `GetQHYCCDSingleFrame` fills its buffer with native little-endian `uint16` samples, but the driver read each pixel high byte first, so every ImageArray/ImageBytes frame from a 16-bit QHY camera came out as noise with ringed star cores (a QHY5III585M guide camera on a Raspberry Pi 4: neighbouring pixels differed by ~23,000 ADU as sent and by ~100 ADU once byte-swapped). The guider's star detector found nothing in such frames. The driver now reads the low byte first, as PHD2's and INDI's QHY drivers do. 8-bit and colour frames were never swapped and are unchanged.

### Added (tests)
- **QHY: 16-bit samples are read little-endian** (AlpacaCore): a fake-SDK frame with samples 0x1234, 65535, 32768 and 1 must come back as those values from `get_image_array`; the existing padded-ROI case (a positive short SDK frame is zero-padded to the requested ROI) now feeds the fake little-endian bytes too.
