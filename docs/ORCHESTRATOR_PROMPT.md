# Orchestrator prompt

Start a new Claude Code session on this repository with **Opus 5.5** as the model. Paste
the prompt below. To continue later, start a new session and paste it again: the
orchestrator picks up from the Status column in `plan.md` and the files in
`docs/handover/`.

Change the `MILESTONES` line to choose how far it should go in one session.

---

```text
You are the orchestrator for building Clip Atlas, a native OBS Studio plugin, using
subagent-driven development. You are explicitly authorized and instructed to spawn
subagents for this work.

MILESTONES: start at the first milestone in plan.md that isn't complete, and work through
M0 only. Then stop at its milestone gate.

Before doing anything else, read CLAUDE.md, plan.md, docs/PLAN.md, every file in
docs/handover/ (if any), and the current source tree. plan.md is your operating manual.
Follow its workflow (§3) exactly. Where this prompt and plan.md disagree, this prompt wins.

Workers:
- Implement every work package (WP) with the Agent tool, using model "haiku" and effort
  "max". Use isolation "worktree" whenever WPs run in parallel.
- Use a Reviewer agent (also model "haiku", effort "max") for every WP marked ★.
- Write each worker brief from the template in plan.md §3.2. Briefs must be
  self-contained, because workers know nothing about this conversation.
- Quote the exact OBS 31 API signatures, signal names and calldata fields each worker
  needs. Check them yourself against the obsproject/obs-studio source at the 31.0 tag
  first. Never let a worker guess an API.
- Send fixes back to the same worker with SendMessage, at most 2 rounds. After that, fix
  it yourself and record why in that WP's hand-over note.

Your own work:
- Write all interface headers yourself (the "Orch" WPs) and commit them before you
  dispatch the WPs that depend on them.
- Integrate each WP using plan.md §3.3. Read every diff in full. Reject changes outside a
  worker's file ownership. Run the unit tests, the format check, and a local compile check
  where one is available.
- After integrating each WP, update its Status in plan.md, and add any decision you made
  to the Decision log.

Git and GitHub (all authorized):
- For each milestone, work on the branch claude/m<N>-<slug>. Branch M0 from
  claude/clip-atlas-foundation unless that branch has already been merged into main; if
  it has, branch from main.
- Commit after each integrated WP with the message "WP<ID>: <title>". Push regularly with
  git push -u origin <branch>.
- Check the GitHub Actions results with the GitHub MCP tools. When CI is red, find the
  real cause before pushing again. Never skip or disable a test to get CI green.
- At the milestone gate (plan.md §3.4), open a pull request against main and subscribe to
  its activity. Fix CI failures and review comments as they arrive.

Stop and report to me:
- at the end of each milestone. Give me the PR link, a summary of each WP, the CI result,
  the manual test checklist (docs/testing/M<N>.md) for me to run in OBS on Windows, and
  any open risks. Then wait for my test results before starting the next milestone.
- earlier if a product decision isn't covered by docs/PLAN.md, if a WP needs a new
  dependency, or if anything would delete or move user files in a way the plan doesn't
  describe.

Be honest about what you verified. The container runs Linux. Windows behavior is verified
only by CI and by my manual testing, so never call Windows-only behavior "working" before
then.
```
