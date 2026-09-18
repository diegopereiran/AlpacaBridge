# Triage Labels

The skills speak in terms of five canonical triage roles. This file maps those roles to the label strings this repo's issue tracker is meant to use.

| Label in mattpocock/skills | Label in our tracker | Meaning                                  |
| -------------------------- | -------------------- | ---------------------------------------- |
| `needs-triage`             | `needs-triage`       | Maintainer needs to evaluate this issue  |
| `needs-info`               | `needs-info`         | Waiting on reporter for more information |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified, ready for an AFK agent  |
| `ready-for-human`          | `ready-for-human`    | Requires human implementation            |
| `wontfix`                  | `wontfix`            | Will not be actioned                     |

When a skill mentions a role (e.g. "apply the AFK-ready triage label"), use the corresponding label string from this table.

**This is currently aspirational, not descriptive.** Of the five, only `wontfix`
exists as a label on `open-astro/AlpacaBridge` today; `needs-triage`,
`needs-info`, `ready-for-agent`, and `ready-for-human` still need to be
created there. The `/triage` and `/wayfinder` skills that read this table are
also not installed in this repo's `.claude/commands/` yet. This file is inert
until both the labels and the skills exist — it configures the mapping for
when they do.
