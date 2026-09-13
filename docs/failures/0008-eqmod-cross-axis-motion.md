# EQMOD-style direct support: one axis stranded the other axis's stop and tracking

## Summary

Alternating MoveAxis commands on the EQM-35 Pro left `Slewing` true after both axes
stopped, with `Tracking` true even though tracking had not restarted. Fixing the
shared stop task revealed further cross-axis ownership and rate-reapplication bugs.
The failures also affected Wave mounts using the same driver.

## Root Cause

A single stop task let a Dec stop cancel the RA stop before it cleared its state.
After splitting tasks per axis, a whole-mount generation counter still suppressed
RA tracking restoration when only Dec had changed. Whole-mount busy checks likewise
could defer a rate write because the other axis was busy, with no owner scheduled
to apply the deferred rate afterward.

## Prevention

Scope stop tasks, cancellation and reapplication ownership to the affected axis.
Sweep both rate setters and tracking-restoration paths together. Tests must assert
that tracking physically resumes in the fake, not merely that `Slewing` clears or
`Tracking` reads true. Reserve whole-mount busy checks for genuinely whole-mount work.

## Evidence

- [Canonical SkyWatcher history](../../.github/instructions/skywatcher.instructions.md),
  the three “KNOWN BUG (FIXED)” sections covering superseded MoveAxis stops,
  cross-axis generation, and per-axis reapplication.
- Regression coverage: `AlpacaCore/tests/test_skywatcher_async.cpp`, using
  `AlpacaCore/tests/fake_skywatcher_mount.h`; the recorded regression dispatches a
  Dec stop while the RA stop is still settling and checks resumed RA motion.
