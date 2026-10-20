# Design decisions

One `NNNN-<slug>.md` record per non-obvious choice or reversal, with a Status
line under the title, then Context, Decision, Rules, Alternatives rejected,
Consequences and Links. Two optional sections may sit between them: Adoption
and How to reverse. `scripts/check_docs_drift.py` check 22 holds every record
to this shape. The how-to of a rule stays at its implementation or instruction
owner; the record keeps the reasoning and the short, checkable form below.

**Rules.** The rules a change is checked against, one numbered line each:

```
1. <the rule, in one or two sentences>. Scope: `<glob>`[, `<glob>`]. Checked by: <test, script check, or review>.
```

A glob matches repository paths (`*` stays in one directory, `**` crosses
them) and must match at least one tracked file. "Checked by" names the test or
check that fails when the rule is broken, or says `review` when only a reader
can tell. A superseded record needs no Rules.

**Adoption.** A record that drivers adopt one at a time keeps its roster here,
not in prose. `scripts/check_docs_drift.py` check 23 compares it with the code:

```
Scan: `<glob of files to scan>`
Symbol: `<identifier an adopter names>`

| Driver | Source | State | Issue | Notes |
```

State is `adopted`, `partial` or `planned`. A `partial` row names the issue
that finishes it; code that deviates from a record is a defect with an issue,
never an exception written into the record. A `planned` row may give `—` as its
source. A table with a `Symbol` column checks each row's own symbol, and then
every scanned file needs a row.

Status is one of:

- `proposed`: written so a pilot change can be reviewed against it; not yet a rule.
- `accepted`: the rule. A record can be accepted before its code lands, when the
  migration that implements it is planned in slices.
- `superseded by NNNN`: kept for history; the named record is the rule.

A record is referred to by its slug until it is written, and numbered then.

- [SkyWatcher pointing clock and UTCDate readback](0001-skywatcher-pointing-clock.md)
- [HTTP server thread ownership](0002-server-thread-ownership.md)
- [Documentation drift gates](0003-docs-drift-gates.md)
- [Device catalog](0004-device-catalog.md)
- [Task clock](0005-task-clock.md)
- [Async operation ownership](0006-async-operation-ownership.md)
- [LAN surface threat model](0007-lan-surface-threat-model.md)
- [Rotator Sync offset persistence](0008-rotator-sync-offset-persistence.md)
- [Link-loss and relink policy](0009-link-loss-and-relink-policy.md)
- [Snapshot-served state](0010-snapshot-served-state.md)
- [CGX logical-disconnect reference continuity](0011-cgx-logical-reference-continuity.md)
- [Bundled libgphoto2](0012-bundled-libgphoto2.md)
- [Designs to avoid](0013-designs-to-avoid.md)
- [Mount UTC offset](0014-utc-offset.md)
- [SDK seams](0015-sdk-seams.md)
