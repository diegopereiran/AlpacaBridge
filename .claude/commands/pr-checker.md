---
description: Drive one or more open PRs through the review-bot loop until each is clean, then merge them in order; keeps looping until every PR is done
allowed-tools: Read, Edit, Write, Bash, Grep, Glob
---

You are the PR checker for the AlpacaBridge project. The user gives you one or more PR numbers
(`/pr-checker 257 258 259`, `/pr-checker 257-259`, or `/pr-checker` alone to mean every open PR).
For each PR, in ascending number order, loop **fix -> push -> poll the review bot** until the bot
posts a clean verdict, then merge it, then move to the next PR. Do not stop early, do not hand a
half-finished PR back, and do not ask "shall I continue?" between rounds. The only stopping points
are: every listed PR is merged (or closed), or a PR is blocked on something only the user can
decide (see **Hard stops**).

## Step 0 — Resolve the PR list

```bash
gh pr list --state open --json number,title,author,headRefName,headRepositoryOwner,isDraft,labels \
  --jq '.[] | "\(.number) \(.title) | \(.author.login) \(.headRepositoryOwner.login):\(.headRefName) draft=\(.isDraft) labels=\([.labels[].name]|join(","))"'
```

Expand ranges (`257-259` -> 257 258 259). Skip numbers that are not open PRs and say so. Record
for each PR: author, head owner/branch, whether it is a **fork PR** (head owner != `open-astro`),
whether it is a draft, and whether it carries the `safe-to-review` label.

**Validate every contributor-controlled string before it touches a shell command.** Branch
names and fork owners come from the PR author and can contain anything git allows. Refuse (hard
stop for that PR) any `headRefName` or `headRepositoryOwner.login` that does not match
`^[A-Za-z0-9][A-Za-z0-9._/-]*$` **and** containing no `..` (check both: the regex alone lets
`foo..bar` through), i.e. no leading `-`, no whitespace, no quotes, no path traversal, and
always double-quote them when interpolated (`"$BRANCH"`, `"$OWNER"`), never bare `<branch>`.
PR numbers must match `^[0-9]+$`. Never `eval` or build a command from a PR title or body.

Print the queue once, then work it top to bottom.

## Step 1 — Per PR: make sure the bot is actually going to run

The review bot (`.github/workflows/claude-review.yml`) posts a comment as the GitHub Actions
bot, ending in `✅ Approved` or `⚠️ Issues found`. `gh pr view --json comments` reports that
author as `github-actions` while the REST API reports `github-actions[bot]`, so match both. It only runs when:

- the PR author has write access, **or** the PR carries the `safe-to-review` label (fork PRs), and
- the author is in `allowed_non_write_users` for pushes the author makes themselves.

Checks to make before waiting on anything:

1. **`review` check skipped / no verdict comment and no `safe-to-review` label** -> add the label
   via REST (`gh pr edit --add-label` can choke on a GraphQL projects warning):
   ```bash
   gh api -X POST repos/open-astro/AlpacaBridge/issues/<N>/labels -f 'labels[]=safe-to-review'
   ```
   The `labeled` event starts a fresh review immediately.
2. **`review` check failed in ~15 s with "Actor does not have write permissions"** -> the author's
   own push could not run the bot. Remove and re-add the label via REST to re-run it as the
   maintainer:
   ```bash
   gh api -X DELETE repos/open-astro/AlpacaBridge/issues/<N>/labels/safe-to-review
   gh api -X POST   repos/open-astro/AlpacaBridge/issues/<N>/labels -f 'labels[]=safe-to-review'
   ```
3. **Branch is behind main** (`gh api "repos/open-astro/AlpacaBridge/compare/main...$OWNER:$BRANCH" --jq .behind_by`
   is non-zero; `$OWNER`/`$BRANCH` validated in Step 0). Branch protection is strict, so it must be updated before it can merge, and
   updating re-runs CI + the bot. Do this **now** rather than after the verdict so you do not pay
   for two bot rounds:
   ```bash
   gh api -X PUT repos/open-astro/AlpacaBridge/pulls/<N>/update-branch
   ```
   A 422 "merge conflict" means resolve locally on the head branch (Step 3 mechanics) and push.
   It is fine to do this while a review is still in flight: the run on the old head is cancelled
   and a fresh one starts on the merged head, so nothing is lost.
4. **Verdict already present for the current head SHA** -> skip the wait and go straight to Step 3.
   Verify the verdict belongs to the current head: the bot comment is newer than the last commit
   (`gh api repos/open-astro/AlpacaBridge/pulls/<N>/commits --jq '.[-1].commit.committer.date'`).
   A verdict older than the head commit is stale and must not be trusted.

## Step 2 — Poll for the verdict (3-minute cadence, background)

Never foreground-sleep. Run this with `run_in_background` and a 30-minute deadline:

```bash
PR=<N>
FILTER='[.comments[] | select((.author.login | test("^github-actions(\\[bot\\])?$")) and (.body | test("✅ Approved|⚠️ Issues found")))]'
DEADLINE=$(( $(date +%s) + 1800 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  sleep 180
  c=$(gh pr view "$PR" --json comments --jq "$FILTER | last | \"\(.createdAt)\n\(.body)\"")
  head_at=$(gh api repos/open-astro/AlpacaBridge/pulls/"$PR"/commits --jq '.[-1].commit.committer.date')
  # Only a verdict NEWER than the head commit counts: counting comments is
  # fooled by a stale verdict on an older head, and by the contributor
  # pushing mid-round (which restarts the review on a new head).
  if [[ "$(echo "$c" | head -1)" > "$head_at" ]]; then echo "$c"; exit 0; fi
done
echo "TIMEOUT: no review-bot comment within 30 minutes" >&2; exit 1
```

When several PRs are queued, poll them all in one background loop and act on whichever verdict
lands first, but **merge strictly in ascending number order** so the update-branch dance is
predictable.

If it times out: `gh run list --workflow=claude-review.yml --limit 5` and read the failing job.
Known stalls: a stuck run is fixed by closing and reopening the PR (`gh pr close N && gh pr reopen N`),
a permission skip by the relabel trick above. Do not restart the poll blindly.

While waiting, watch CI too (`gh pr checks <N>`). A red CI check gets fixed and pushed in the same
batch as the bot findings, not on its own.

## Step 3 — Act on the verdict

Read the newest bot comment in full.

### `⚠️ Issues found`

Fix **every** finding the bot raises on this PR, in this PR, in **one batched commit**. Each push
restarts a full fresh review (PR #99 took 46 rounds when pushes trickled). Do not defer findings to
follow-up issues and do not decline them as low priority unless the user says so; the standing
rule is "work it in the same PR till there are no more issues."

**Before writing a line**, fetch the fork head: contributors watch the same bot and often push
their own fix for the same finding within minutes (PR #258 did this twice in one session).
If their head already moved past the reviewed SHA, read their diff first; if it addresses the
finding, adopt it (reset your local branch to their head) and just poll again.

Mechanics for a **fork PR** (the usual case for contributor branches):

```bash
# $REMOTE is a local remote name you chose (e.g. `diego`), $BRANCH the validated head name.
git fetch "$REMOTE" "$BRANCH"
git checkout -B "$BRANCH" "$REMOTE/$BRANCH"
# ... apply fixes ...
./scripts/ci_preflight.sh                   # HARD BLOCK: do not push red (summary lines are indented "  [PASS] ...")
git fetch "$REMOTE" "$BRANCH"               # contributors push concurrently; re-check the head
git log --oneline "HEAD..$REMOTE/$BRANCH"   # must be empty; if not, rebase onto it first
git push "$REMOTE" "HEAD:$BRANCH"
```

If the fork remote does not exist, add it with the validated owner:
`git remote add "$REMOTE" "https://github.com/$OWNER/AlpacaBridge.git"`.
If the contributor already pushed an equivalent fix while you were working, **adopt theirs** and
drop your duplicate instead of force-pushing. Never force-push a contributor's branch. Adopting
is not a rubber stamp: read their **entire** diff against the previously reviewed head (not
just the hunk that addresses the finding) and confirm it contains nothing beyond that fix before
resetting onto it; anything unrelated goes back to the bot as a normal push and review round.

For an `open-astro` branch, the same flow against `origin`.

Commit message: verb-first title under 70 chars, body explaining what the bot found and how it was
fixed, then the attribution trailer from the session. After pushing, go back to Step 1 (the push
may need the relabel trick again if it lands as the contributor) and Step 2.

**Never post PR comments replying to the bot.** A finding is either a change to the code, skill
or docs (push it), or, if it is clearly wrong and nothing can be changed to satisfy it, a hard
stop for the user to rule on. Explanations belong in the commit message, not in the PR thread.
Keep a running tally of rounds per PR and report it in the wrap-up.

### `✅ Approved`

**Stop pushing to this branch.** Approval is the terminal state; non-blocking notes in an approval
are not commits. Then:

```bash
gh pr view <N> --json isDraft,mergeable,mergeStateStatus --jq '"draft=\(.isDraft) mergeable=\(.mergeable) state=\(.mergeStateStatus)"'
```

- `draft=true` -> `gh pr ready <N>` first (contributors often open drafts; `gh pr merge` refuses them).
- `state=BEHIND` -> `update-branch` (Step 1.3) and go back to Step 2; the merge commit re-runs the bot.
- `state=BLOCKED` with checks still running -> wait for `gh pr checks <N> --watch`, then merge.
- otherwise merge:
  ```bash
  gh pr merge <N> --merge
  ```
  (merge commit, not squash, matching the repo history). Confirm `state=MERGED` afterwards.

Because the user invoked `/pr-checker` with the instruction to merge once the bot is clean, that
invocation **is** the merge authorization for every PR in the list. Do not ask again per PR.
This is the maintainer's deliberate policy for this repository (stated 2026-09-09 when the skill
was commissioned: "merge and close once the bot says there are no outstanding issues"), not a
convenience default: the review bot plus the full CI matrix is the review gate, and the
maintainer runs this skill themself, interactively, so a human is in the loop at invocation time
and can interrupt at any round. The guardrails that keep it safe are the ones above: every fork
input validated, every finding fixed in-PR rather than waived, every adopted contributor diff
read in full, and the **Hard stops** below, which override this authorization.

After a merge, every remaining PR in the queue is now behind main: run Step 1.3 on the **next** PR
right away so its refresh round starts while you tidy up.

## Hard stops (the only reasons to hand back to the user)

- A PR's head branch name or fork owner fails the Step 0 validation, or a contributor's diff
  contains changes outside the reviewed finding that you cannot vouch for.
- The bot finding requires a product decision (change a default, drop a platform, alter a
  user-facing behaviour) that the PR author did not intend.
- A ConformU report on the branch is failing (a driver PR cannot merge with a red report; see
  `/submit-pr` Step 1).
- Merge conflicts that cannot be resolved without choosing between two contributors' intents.
- The review workflow itself is broken (two consecutive timeouts after the relabel/close-reopen
  tricks) — report the run URL.

State the blocker in one or two sentences, finish every other PR in the list, and say exactly
which PR was left and why.

## Wrap-up

One table: PR, title, rounds, final verdict, merge SHA (or "left open: reason"). Then a single
line naming anything the next session should know (e.g. an `update-branch` still running on a
PR outside the list). Update memory only if the loop mechanics themselves changed (new bot login,
new label, new stall trick); the per-PR outcome does not belong in memory.
