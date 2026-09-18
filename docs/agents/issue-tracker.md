# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

Issues live on **`open-astro/AlpacaBridge`**, not on whichever remote a given
clone happens to call `origin`. Every example below carries
`--repo open-astro/AlpacaBridge` explicitly for that reason — copy it as
written, not the remote name from your own checkout.

## Conventions

- **Create an issue**: `gh issue create --repo open-astro/AlpacaBridge --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --repo open-astro/AlpacaBridge --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --repo open-astro/AlpacaBridge --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --repo open-astro/AlpacaBridge --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --repo open-astro/AlpacaBridge --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --repo open-astro/AlpacaBridge --comment "..."`

## Pull requests

External PRs are not treated as a triage/request surface in this repo.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --repo open-astro/AlpacaBridge --comments`.
