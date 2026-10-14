# Celestron CGX/CGX-L direct-USB review

Static review of the CGX/CGX-L direct-AUX telescope backend at commit `1c23cefa`,
against ASCOM Master Interfaces 1.0.27 and the repository's Celestron AUX notes.
No source changes or hardware operations were performed during this review.
The working plan, including the complete repair checklist, is maintained at
`/home/pauleyj/src/open-astro/celestron-cgx-cgxl-usb-dirver-plan.md`.

## Follow-up implementation status — 2026-10-12

The follow-up fixes are committed on `driver/celestron-cgx-usb` at `82bafe45bea009cf68aa7afbb13a8a67036cf8db` and pushed to the fork.

The `driver/celestron-cgx-usb` branch now addresses the software-side findings:

- Motion starts treat acknowledgements as ambiguous, attempt both axis stops, reconnect/flush before retrying idempotent stop commands, and keep `Slewing`/coordinate reads in an error state when stopping cannot be verified. Home/GOTO dispatches and flip rebase/second-leg commands are covered by ownership fences.
- Motion initiators drain the active guide packet before taking motion ownership; pulse teardown is bounded to that packet. `Unpark` cancels and joins a pending Park, and `SlewSettleTime` delays completion through the injected task clock.
- The RA encoder transform and RA guide polarity now mirror the southern tracking direction; apparent sidereal time includes the equation of equinoxes. Sync stops tracking during encoder writes/readback, verifies within encoder tolerance, and restores the previous tracking state. The current follow-up preserves Sync's encoder-label correction across the flip re-home and adds post-flip sky-coordinate/tracking assertions.
- Async GOTO completion verifies measured RA/Dec against the target at current pointing time. Partial tracking writes attempt rollback; unverified rollback makes Tracking readback fail. Failed tracking restoration after a verified stop now clears the logical slew state before reporting the restore error. Firmware cache ordering and motion-slot failure reset were corrected.
- AUX exchanges enforce the absolute deadline and poison the stream after timeout until reconnect; the reconnect path flushes queued input. Modern identity requires four-byte RA/Dec motor versions and model `0x1788` on both motors, without separate switch-address probes. The two-byte-version/switch-probe route remains a legacy fake-backed profile. See [USB identity evidence](celestron-cgx-usb-identity.md). Target read-before-set uses `InvalidOperation` as specified in Master Interfaces documentation revision 1.0.27.
- Adversarial tests cover lost home/GOTO acknowledgements, CGX identity rejection, live tracking drift during Sync, off-target completion, Sync-preserving flip re-home, failed pre-GOTO tracking shutdown, unresolved guide-status recovery, Park cancellation, bounded guide teardown, timeout recovery, and settle-time completion. The direct factory rejects invalid site/optical ranges, and the Celestron stress guard no longer accepts broad `DriverException` failures.

Local C++20 syntax checks, `clang-format --dry-run`, `git diff --check`, all 64 web JavaScript tests, docs/instruction/changelog validation, layering, contract-sweep registration, stress registration, and connect-error-hook checks pass.

Native arm64 workflow run [38197443789](https://github.com/pauleyj/AlpacaBridge/actions/runs/38197443789) passed on the commit above: vendor-neutral and all-vendor CTest, ASan/UBSan, TSan, and the all-vendor coverage run/report. The docs-drift, layering, contract-sweep, stress-registration, connect-error-hook, JavaScript, Unicode, ShellCheck, and zizmor jobs also passed. This was a `workflow_dispatch`; PR-only clang-format/clang-tidy/cppcheck/ConformU-report jobs were skipped. Local formatting and syntax checks passed; no new ConformU hardware report was claimed.

The qualification gaps below describe the historical review baseline, not the latest hardware status. Later [identity evidence](celestron-cgx-usb-identity.md) and [run 5 qualification](celestron-cgx-usb-motion.md#run-5-passing-qualification-and-final-software-verification) cover the bare northern CGX with motors 7.17.25 / model `0x1788`; they do not qualify modern CGX-L identity, southern operation, or optical accuracy. The repository's older `ValueNotSet` target tests and implementations outside the CGX direct backend remain a separate migration to the documented `InvalidOperation` rule; revision 1.0.27 is not evidence of when that requirement was introduced.

### PR #1148 review follow-up — 2026-10-14

The follow-up committed as `bddbdc38` moves reconnect and disconnect continuity checkpoint I/O outside the coarse state mutex while retaining lifecycle ownership and rechecking the original reference generation before publication. Disconnect clears its transition flag on both successful and failed-stop completion; motion handoff checks connection again under its lifecycle locks before transition errors. A held-checkpoint fake case covers cached guide-rate responsiveness, queued tracking, failed checkpoint cleanup and calibration retention/refusal on reconnect. The initial native arm64 baseline passed that case (20 assertions), the focused CGX suite (90 cases / 2044 assertions), and all 442 Core CTest cases with only the Celestron vendor enabled. Final-source UBSan and TSan each passed 90 CGX cases / 2046 assertions and two lifecycle stress cases / five assertions. The final ordinary native rerun passed 90 CGX cases / 2045 assertions and both lifecycle stress cases / five assertions. ASan remains unvalidated for this follow-up: the supplied Pi 4 runs a 39-bit-VA kernel, the known [ASan host limitation](failures/0010-rpi4-asan-39-bit-va.md). No new hardware/ConformU result is claimed.

Mutation verification falsified all 95 native candidates and all three web candidates, with passing baseline/restored runs. Of these, 94 native candidates and the three web candidates produced ordinary named-case failures. The destruction-during-connect mutation instead causes SIGABRT when `shutdown_connection()` is omitted: the process abort is a failing outcome that falsifies the lifecycle contract, not an ordinary assertion failure. Candidate compilation failures, survivors and timeouts were not counted. The concrete edits are published in [PR #1148's Falsified by section](https://github.com/open-astro/AlpacaBridge/pull/1148); the counts here summarize operator-observed runs, not publicly attached raw logs.

Idle `IsPulseGuiding` hardware polling is intentionally unchanged. It queries RA and, if RA is inactive, Dec when the pulse slot is idle, preserving current status and fault reporting without another cache. Optimize only after a measured bus-load problem.

Sibling sweep: the shared `MotionTransition` check covers tracking/tracking-rate, Sync, Home/Park and coordinate/target GOTO starts. PulseGuide now also rechecks connection under `motion_start_mutex_`; AbortSlew/Unpark already do so in their shared stop initiator, and both site setters check under the motion/state locks. Connection teardown retains lifecycle ownership across joins; worker stage guards still refuse disconnected/transitioning state. Both reconnect and disconnect checkpoint callers release only the state mutex, retaining their existing lifecycle protection. Neither async connection forwarding nor the worker lock order was changed.

Local follow-up checks: C++20 syntax-only checks for the changed driver/test, clang-format 18 dry-run, changed-line clang-tidy 18, production-scoped cppcheck 2.17.1, all 81 web tests, docs-drift, instruction-structure, changelog, layering, contract/stress registration, connect-error-hook and whitespace checks passed. These local checks are not native C++ runtime or full-preflight evidence; native results are listed above. The corrected PR-body mutation list has 98 concrete entries, including the new regression, with the destructor exception explicitly identified. The latest review notes retain the intentional coordinate-read wait behind the disconnect's motion-start lock; cached-only reads need not take that lock. Idle guide-status polling remains unchanged without measured load evidence.

The committed fake-based suites model applied/reply-lost commands, partial-axis
failures and an explicit per-read drift hook. They do not establish real motor
dynamics, rate units or physical calibration. Hardware qualification remains separate.

## Hardware-free follow-up — 2026-10-13

This section records a historical checkpoint, not the current validation status.
Subsequent native runtime and mutation results are in the 2026-10-14 follow-up
above. A full local pre-flight is still not claimed.

The follow-up based on `ff836c44` adds explicit elapsed-time encoder motion,
fractional signed motion and wrap handling, physical/encoder-label separation,
a fixed simulated home origin, and a held-fragment rendezvous that teardown wakes.
The physical-relabel test now observes physical position, rather than just reading
back a label. Both instant and explicit Home reset the same simulated origin;
manual positive/negative commands and tracking use the configured test rate.
These are fake assumptions, not verified controller velocity-register semantics.

New/extended cases cover both hemispheres and pointing branches, guide-duration
rounding around 2550 ms, segmented packet totals, independent driver instances,
reconnect calibration refusal, and the existing 180-second GOTO deadline. The
deadline test auto-completes Home before holding GOTO, so its setup cannot park
on an unadvanced fake clock.

Checks completed before committing this follow-up: C++20 syntax-only checks for both CGX
production translation units, the protocol/geometry/telescope tests, CGX stress
and the contract sweep; clang-format **18.1.8** changed-line check against the PR
base; clang-tidy **18.1.3** on the CGX driver with a source-only line filter;
docs-drift, layering, contract/stress registration, connect-error hook,
changelog validation and whitespace checks. Source-scoped tidy does not certify
unchanged shared-header warnings. At that checkpoint no runtime tests had run:
the host was x86_64, and the existing ARM container could not execute. Native
arm64 CTest, sanitizers, runtime mutation checks and full pre-flight were pending.
ConformU was not re-run in that continuation. Those source-only checks did not
qualify the hardware or replace native runtime validation.

### Mutation evidence

The initial proposed mutations from this checkpoint are superseded by the tested
98-entry list in [PR #1148](https://github.com/open-astro/AlpacaBridge/pull/1148).
The 2026-10-14 follow-up above records the observed outcomes and their limits.

## P1 — fix before hardware qualification

These are findings at the original `1c23cefa` review baseline, not open defects
at the current PR head. Resolution and remaining qualification are recorded above.

1. **Ambiguous first-axis command failure leaves unowned motion.** In
   `start_home_operation()` and `start_slew_operation()`, the first RA command
   is outside the cleanup `try`. A command applied by the mount whose reply is
   lost can leave an axis moving without a monitor or stop. Cover every dispatch
   stage and treat lost acknowledgements as possibly applied.
2. **Abort can be followed by a stale flip GOTO.** In
   `run_flip_slew_operation()`, cancellation is checked before home polling,
   but not revalidated between encoder rebasing and dispatching the target
   GOTO. Fence each motion-stage transition against AbortSlew/disconnect.
3. **Stop failures are swallowed and can become false completion.**
   `stop_axes_no_throw()` discards both errors; a cancelled body can then return
   normally and make `Slewing` false without evidence that the mount stopped.
   Attempt both stops but retain/report failures and verify stopping before
   publishing completion.
4. **Motion initiators do not take ownership from pulse guiding.** Slew, Park,
   FindHome, Sync, tracking changes, and AbortSlew do not cancel/drain
   `pulse_slot_`. A long guide can send a later pulse segment after another
   operation starts. Serialize guide ownership across the initiator family.
5. **Disconnect can wait for the full requested guide duration.** A cancelled
   pulse worker waits for the active bit to clear, with a deadline based on the
   original duration (up to 600 seconds), not the current ≤2.55-second packet.
   Bound cancellation to the active segment and a short timeout.
6. **Southern tracking sign contradicts the RA encoder transform.** The
   transform retains the northern RA orientation while tracking switches to
   negative command `0x07` in the south. Under these equations, reported RA
   drifts at about two sidereal rates (≈1,805 arcseconds/minute). Derive the
   transform, tracking, branch selection and guide polarity together and add
   dynamic fixtures.
7. **Sync readback requires an exact encoder count while tracking can move it.**
   Sync writes both axes then requires exact equality with the precomputed RA
   count. A 24-bit RA count advances about every 5.14 ms at sidereal rate, so
   normal transaction latency can fail Sync and invalidate calibration. Stop/
   restore tracking or use a time-adjusted comparison with encoder tolerance.
8. **Flip re-home discards Sync's encoder correction.** Sync relabels the
   current counters, but a later flip re-home writes fixed quarter/half-turn
   values as the new reference. Preserve the calibrated home-to-sky transform
   across re-home, or invalidate it and require Sync again.
9. **GOTO completion is not verified against the target sky position.** The
   worker trusts `MC_SLEW_DONE`, without reading arrival position or accounting
   for elapsed pointing time. The repository's CGX-L 7.18 notes say tracking
   stops during GOTO, giving roughly 15 arcseconds/second of RA undershoot if a
   fixed target is used. Verify arrival before reporting `Slewing=false`.
10. **Partial tracking writes can leave cached state false to hardware.** The
    helper attempts both axis commands and throws if either fails; the setter
    restores only the previous boolean even if one hardware command succeeded.
    Restore/verify hardware state or latch an error that makes `Tracking`
    unreliable.

## P2 — resolve before calling the interface complete

- `Unpark` only clears `parked_`; an active Park body can set it true again.
- `SlewSettleTime` is stored but ignored when deciding when `Slewing` becomes
  false.
- `AtHome` remains true after tracking starts, although ASCOM defines it as
  stopped in the Home position.
- Motion-slot failure state survives disconnect/reconnect, so a fresh session
  can report the preceding session's motion failure.
- LST uses mean sidereal time; ASCOM `SiderealTime` requires local apparent
  sidereal time.
- The AUX exchange loop does not enforce its absolute deadline if the fd stays
  continuously readable with unrelated frames.
- A delayed response with the same source/destination/command can satisfy a
  later identical request; timeout recovery needs resynchronization.
- Target/site/optical/rate/refraction properties do not consistently enforce
  disconnected state. The official Master page also says target read-before-set
  is `InvalidOperationException`, conflicting with repository
  `ValueNotSetException` guidance/tests; resolve the conflict explicitly.
- Firmware is published before `connected_` becomes true and cleared after it
  becomes false, allowing a management poll to expose firmware while the
  driver reports disconnected.
- TSan stress accepts broad `DriverException` failures with a healthy fake,
  potentially masking protocol regressions.
- Connect checks generic motor-controller version replies but has no CGX/CGX-L
  identity gate; the direct AUX application source address is also unverified.

## Repair order

1. Fix ambiguous dispatch failures, cancellation/ownership fencing, stop
   verification and bounded pulse teardown.
2. Fix hemisphere-aware transforms, Sync tolerance/reference persistence and
   GOTO arrival verification.
3. Correct ASCOM state/completion semantics and narrow stress expectations.
4. Add adversarial fake cases, rerun all-vendor arm64 CTest/TSan, then perform
   bare-mount and southern-hemisphere hardware qualification.
