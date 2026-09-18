# Nikon bulb close failed and hung the camera after a full config-tree walk

## Summary

On a Nikon D3300 (libgphoto2 2.5.31, OpenAstro ASIAIR Pro image) every GPhoto exposure past the
30 s native shutter ceiling opened the shutter, held it for the requested time, then failed the
close with `Failed to set bulb=0: Unspecified error`. No frame was delivered, and the camera's
PTP stack was hung until it was power-cycled; a USB re-enumeration from the host did not
recover it (even `OpenSession` timed out). An earlier session on the same body had failed
differently, `Widget not present: shutterspeed2`, for a widget `gphoto2 --get-config` showed
plainly with `Current: Bulb`.

## Root Cause

`GPhotoSDKWrapper` fetched the camera's whole config tree with `gp_camera_get_config` for every
widget read and write, looked the one widget up in it, and for a setter wrote the whole tree back
with `gp_camera_set_config`. The ptp2 camlib builds that tree by reading dozens of PTP
properties; while the body is busy (mid-bulb, or right after a capture it refused) some of those
reads fail or the tree comes back truncated. The close toggle therefore never reached
`TerminateCapture` cleanly, the bulb capture was left open on the camera side, and the body
wedged. The "missing" `shutterspeed2` was the same truncation in a session whose priming capture
had already failed.

The control that isolated it: with AlpacaBridge disconnected, on the same SBC,
`gphoto2 --set-config bulb=1 --wait-event=60s --set-config bulb=0 --wait-event-and-download=40s`
delivered a NEF every time. The CLI's `--set-config` goes through
`gp_camera_set_single_config`, which touches only the one widget.

Two secondary gaps were in the same path. The hold between `bulb=1` and `bulb=0` was a plain
sleep, where the CLI (and indi-gphoto) keep polling the camera's events for the whole exposure.
And the wait for the frame after the close was a fixed 15 s, while a body with long-exposure
noise reduction on posts the file a full exposure-length later (the dark frame), so every long
frame would have been lost even with a working close.

## Prevention

- Every widget operation goes through libgphoto2's single-config API
  (`gp_camera_get_single_config` / `gp_camera_set_single_config`, available since 2.5.10). A
  name the camlib has no widget for answers `GP_ERROR_BAD_PARAMETERS` and maps to "absent";
  every other failure throws, so a busy camera reads as an error rather than as a missing widget.
  Do not reintroduce a full-tree read or write on any path a busy camera can be on.
- The bulb hold pumps the event queue in 100 ms slices (`GPhotoSDK::drain_events`) rather than
  sleeping. A file-added event during the hold cannot be this exposure's (the shutter is open),
  so it is deleted from the camera and dropped, never handed to the next poll as a fresh frame.
- The frame wait after the close is `duration + 30 s`, polled in 1 s slices
  (`GPhotoSDK::poll_bulb_file_and_download`) so stop/abort stay responsive, and the
  `start_exposure` watchdog deadline includes that window for a bulb capture. An abort still
  consumes the aborted frame (bounded at 15 s) so it is not left queued.
- Bench settings that matter for a Nikon body, recorded in the gphoto instructions and
  `SUPPORTED-DRIVERS.md`: mode dial M, shutter speed Bulb, lens on MF (with AF the body refuses
  to fire, which is also how a priming capture can fail on first connect), Long exposure NR Off.

## Evidence

- Issue [#569](https://github.com/open-astro/AlpacaBridge/issues/569).
- Bench: Nikon D3300 (04b0:0433) on the OpenAstro ASIAIR Pro image (Raspberry Pi 4B, Debian
  trixie, libgphoto2 2.5.31-4), AlpacaBridge 4.0.0. Failure reproduced twice through the Alpaca
  API; the CLI control passed at 10 s and 60 s. After the fix: 60 s and 300 s bulb frames
  (6016x4016) delivered through the Alpaca API, no warnings.
- Regression tests (`AlpacaCore/tests/test_gphoto_fake_sdk.cpp`): "bulb hold pumps camera events
  for the whole exposure instead of sleeping" and "bulb frame poll keeps polling until the camera
  delivers the file". The single-config change itself has no hardware-free test: the fake SDK sits
  above the libgphoto2 call boundary, so the full-tree-versus-single-widget distinction is only
  observable on a real body. Coverage there is the bench run above and the comment on
  `get_single_widget_or_null` in `gphoto_sdk_wrapper.cpp` naming this record.
