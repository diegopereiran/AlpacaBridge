# QHY filter wheel cached a transit slot as settled

## Summary

ConformU move checks timed out because `Position` stayed at a stale slot after a move.

## Root Cause

`GetQHYCCDCFWStatus` returns physical passing slots during rotation, mostly as valid non-negative digits. The earlier cache treated the first such digit as settled. It could therefore freeze at the pre-move slot. Each raw read also takes about 100–130 ms, too slow for the first FAST-classified read after connect.

## Prevention

Prime a settled cache once at connect. On `set_position()`, record the target and invalidate that cache. During a pending move, read live and report `-1` until hardware reports the commanded target; only then cache it. Keep the ASCOM moving sentinel independent of the SDK's brief `-1` near one physical position.

## Evidence

- Hardware finding and rule: `.github/instructions/qhy.instructions.md` (`GetQHYCCDCFWStatus`).
- Regression tests: `AlpacaCore/tests/test_qhy_filterwheel.cpp` cases “Connect seeds the position cache” and “A move reports -1 in transit then settles”.
- Implementation: `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp`.
