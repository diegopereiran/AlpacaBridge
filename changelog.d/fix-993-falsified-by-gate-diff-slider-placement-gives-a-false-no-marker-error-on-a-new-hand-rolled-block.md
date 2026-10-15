### Fixed (tooling)

- **The `Falsified by:` gate no longer reports a false "no marker" error for a new hand-rolled block inserted directly before another one** (CI, `scripts/check_falsified_by.py`, issue #993). Git can slide such an insertion so the added `    {` is the next block's opener; the gate now reads each block's `// case:` marker from the file at HEAD instead of the next added diff line, and collects the real case name. `--self-test` case "slid block: real case name collected, no false marker error" fails on the old code with the false error and passes now.
