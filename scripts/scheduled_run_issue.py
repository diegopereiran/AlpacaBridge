#!/usr/bin/env python3
"""Keep one open issue per scheduled workflow while its runs fail.

A failed scheduled run is otherwise visible only in the Actions tab: nothing
opens an issue when the nightly build, the weekly CodeQL scan or another
scheduled workflow goes red. The workflow .github/workflows/scheduled-failure.yml
(issue #1352) runs this script after each scheduled or manually dispatched run
of those workflows on main.

- A failed, timed-out or never-started run opens an issue for its workflow, or
  comments on the one already open, with a link to the run.
- A successful run closes that workflow's open issue.
- A cancelled or skipped run changes nothing.

The issue is found by a marker in its body, not by its title, so a retitled
issue is still found. The run details come from the environment.

Usage:
  scheduled_run_issue.py [--dry-run]   (WORKFLOW_NAME, CONCLUSION, RUN_URL,
                                        HEAD_SHA, EVENT; GH_TOKEN, GH_REPO)
  scheduled_run_issue.py --self-test
"""
import argparse
import json
import os
import re
import subprocess
import sys

MARKER_RE = re.compile(r"<!-- scheduled-run: (\{.*?\}) -->")
FAILED = ("failure", "timed_out", "startup_failure")
LABELS = ("bug", "P3")
LIST_LIMIT = 500


def marker(workflow):
    return "<!-- scheduled-run: %s -->" % json.dumps({"workflow": workflow}, sort_keys=True)


def marked_workflow(body):
    """The workflow name in a body's marker, or None."""
    m = MARKER_RE.search(body or "")
    if not m:
        return None
    try:
        d = json.loads(m.group(1))
    except json.JSONDecodeError:
        return None
    return d.get("workflow") if isinstance(d, dict) else None


def find_issue(issues, workflow):
    """The lowest-numbered open issue whose marker names `workflow`, or None."""
    found = [i for i in issues if marked_workflow(i.get("body")) == workflow]
    return min(found, key=lambda i: i["number"]) if found else None


def decide(conclusion, issue):
    """'open', 'comment', 'close' or 'none' for a run with `conclusion` and the workflow's open `issue`."""
    if conclusion in FAILED:
        return "comment" if issue else "open"
    if conclusion == "success" and issue:
        return "close"
    return "none"


def run_line(run):
    return "[%s run](%s) on `%s` (%s): **%s**." % (run["event"], run["url"], run["sha"][:7], run["workflow"],
                                                   run["conclusion"])


def issue_body(run):
    return ("%s\nA run of `%s` on main did not succeed.\n\n- %s\n\n"
            "The next run of `%s` that succeeds closes this issue; each further failure adds a comment.\n\n"
            "Opened by `.github/workflows/scheduled-failure.yml`.\n"
            % (marker(run["workflow"]), run["workflow"], run_line(run), run["workflow"]))


def reconcile(run, client):
    """Apply the decision for `run` through `client`; return a one-line summary."""
    issue = find_issue(client.list_open(), run["workflow"])
    action = decide(run["conclusion"], issue)
    if action == "open":
        client.create("Scheduled run failing: %s" % run["workflow"], issue_body(run), LABELS)
        return "opened an issue for %s" % run["workflow"]
    if action == "comment":
        client.comment(issue["number"], "Failed again: " + run_line(run))
        return "commented on #%d" % issue["number"]
    if action == "close":
        client.close(issue["number"], "Recovered: " + run_line(run))
        return "closed #%d" % issue["number"]
    return "nothing to do (%s, %s)" % (run["conclusion"], "issue #%d open" % issue["number"] if issue else "no issue")


class GhIssues:
    """Issue operations through the gh CLI (GH_TOKEN and GH_REPO from the environment)."""

    def __init__(self, dry_run):
        self.dry_run = dry_run

    def _gh(self, *args, stdin=None):
        if self.dry_run and args[1] != "list":
            print("DRY RUN: gh " + " ".join(args) + ("\n" + stdin if stdin else ""))
            return ""
        return subprocess.run(["gh", *args], input=stdin, check=True, capture_output=True, text=True).stdout

    def list_open(self):
        out = self._gh("issue", "list", "--label", LABELS[0], "--state", "open", "--limit", str(LIST_LIMIT),
                       "--json", "number,body")
        return json.loads(out or "[]")

    def create(self, title, body, labels):
        args = ["issue", "create", "--title", title, "--body-file", "-"]
        for label in labels:
            args += ["--label", label]
        self._gh(*args, stdin=body)

    def comment(self, number, text):
        self._gh("issue", "comment", str(number), "--body-file", "-", stdin=text)

    def close(self, number, text):
        self._gh("issue", "close", str(number), "--comment", text)


def self_test():
    bad = []

    def check(name, got, want):
        if got != want:
            bad.append("FAIL %s: got %r, want %r" % (name, got, want))

    issue = {"number": 7, "body": marker("Nightly Build")}
    for conclusion, open_issue, want in [
        ("failure", None, "open"), ("timed_out", None, "open"), ("startup_failure", None, "open"),
        ("failure", issue, "comment"), ("timed_out", issue, "comment"),
        ("success", issue, "close"), ("success", None, "none"),
        ("cancelled", None, "none"), ("cancelled", issue, "none"),
        ("skipped", issue, "none"), ("neutral", None, "none"), ("", None, "none"),
    ]:
        check("decide(%r, %s)" % (conclusion, "issue" if open_issue else "None"), decide(conclusion, open_issue), want)

    issues = [{"number": 9, "body": "text\n" + marker("CodeQL")},
              {"number": 4, "body": marker("Nightly Build") + "\nretitled"},
              {"number": 3, "body": "<!-- scheduled-run: {not json} -->"},
              {"number": 2, "body": None},
              {"number": 8, "body": marker("Nightly Build")}]
    check("find by marker", (find_issue(issues, "CodeQL") or {}).get("number"), 9)
    check("lowest of two", (find_issue(issues, "Nightly Build") or {}).get("number"), 4)
    check("no marker for workflow", find_issue(issues, "OSV scan"), None)
    check("marker round trip", marked_workflow(issue_body({"workflow": "OSV scan", "event": "schedule",
                                                           "url": "u", "sha": "abcdef0123", "conclusion": "failure"})),
          "OSV scan")

    class Fake:
        def __init__(self):
            self.issues = []
            self.calls = []

        def list_open(self):
            return [i for i in self.issues if i["state"] == "open"]

        def create(self, title, body, labels):
            self.issues.append({"number": len(self.issues) + 1, "title": title, "body": body, "state": "open"})
            self.calls.append(("create", title, labels))

        def comment(self, number, text):
            self.calls.append(("comment", number))

        def close(self, number, text):
            self.issues[number - 1]["state"] = "closed"
            self.calls.append(("close", number))

    def run(workflow, conclusion):
        return {"workflow": workflow, "conclusion": conclusion, "event": "schedule",
                "url": "https://example.invalid/run", "sha": "0123456789abcdef"}

    f = Fake()
    for workflow, conclusion in [("Nightly Build", "failure"), ("CodeQL", "failure"), ("Nightly Build", "failure"),
                                 ("Nightly Build", "cancelled"), ("Nightly Build", "success"),
                                 ("Nightly Build", "success"), ("Nightly Build", "failure")]:
        reconcile(run(workflow, conclusion), f)
    check("reconcile sequence", f.calls, [
        ("create", "Scheduled run failing: Nightly Build", LABELS),
        ("create", "Scheduled run failing: CodeQL", LABELS),
        ("comment", 1),
        ("close", 1),
        ("create", "Scheduled run failing: Nightly Build", LABELS),
    ])
    check("CodeQL issue still open", [i["number"] for i in f.list_open()], [2, 3])

    for line in bad:
        print(line)
    if bad:
        return 1
    print("scheduled_run_issue self-test: all cases passed")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    env = {k: os.environ.get(k, "") for k in ("WORKFLOW_NAME", "CONCLUSION", "RUN_URL", "HEAD_SHA", "EVENT")}
    missing = [k for k in ("WORKFLOW_NAME", "RUN_URL") if not env[k]]
    if missing:
        p.error("missing environment: " + ", ".join(missing))
    run = {"workflow": env["WORKFLOW_NAME"], "conclusion": env["CONCLUSION"], "url": env["RUN_URL"],
           "sha": env["HEAD_SHA"], "event": env["EVENT"]}
    print(reconcile(run, GhIssues(args.dry_run)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
