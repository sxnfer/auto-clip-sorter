# M2 orchestrator prompt

Start a new Claude Code session on this repository with **Opus 5.5** as the model, and
paste the text below.

```text
You are the orchestrator for building Clip Atlas, a native OBS Studio plugin, using
subagent-driven development. You are explicitly authorized and instructed to spawn
subagents for this work.

MILESTONES: M2 only (Recordings and screenshots: WP2.0–WP2.5). M0 and M1 are complete
and tested on Windows by me. Stop at the M2 milestone gate.

Starting point:
- Work on the existing branch claude/m2-recordings-screenshots. It is stacked on
  claude/m1-feature-parity (PR #2), which is stacked on claude/m0-scaffold (PR #1).
  Neither PR is merged yet. Do not rebase or rewrite them.
- Before anything else, read CLAUDE.md, plan.md, docs/PLAN.md, docs/handover/M2-kickoff.md
  (read this one first and closely: current state, product decisions made with me since
  docs/PLAN.md, OBS 31.0.0 facts already verified for M2, and process lessons), every
  other file in docs/handover/, and the current source tree. plan.md is your operating
  manual. Follow its workflow (§3) exactly. Where this prompt and plan.md disagree, this
  prompt wins.
- Clone the OBS source for API checks into your scratchpad:
  git clone --depth 1 --branch 31.0.0 https://github.com/obsproject/obs-studio.git
- If the local tools are missing, reinstall them as described in M2-kickoff.md
  (clang-format-19, gersemi 0.21.0, libobs-dev, Qt 6 dev packages).

Workers:
- Implement every work package (WP) with the Agent tool, using model "haiku" and effort
  "max". Use isolation "worktree" whenever WPs run in parallel. Worktrees are created from
  main, so every brief must start with STEP 0: git merge --ff-only
  claude/m2-recordings-screenshots, then confirm the expected `git log -1` commit.
- Use a Reviewer agent (also model "haiku", effort "max") for every WP marked ★ (WP2.1,
  WP2.2). Have reviewers check OBS claims against the cloned 31.0.0 source.
- Write each worker brief from the template in plan.md §3.2. Briefs must be
  self-contained, because workers know nothing about this conversation.
- Quote the exact OBS 31 API signatures, signal names and calldata fields each worker
  needs. Check them yourself against the 31.0.0 source first, starting from the facts in
  M2-kickoff.md. Never let a worker guess an API. Verify any surprising claim a worker
  makes about OBS before acting on it.
- Send fixes back to the same worker with SendMessage, at most 2 rounds. After that, fix
  it yourself and record why in that WP's hand-over note. If a usage limit stops an
  agent, resume it with SendMessage rather than starting a new one.

Your own work:
- Write all M2 interface headers yourself (WP2.0) and commit them before dispatching the
  WPs that depend on them: core/recording-tracker, the recording glue in src/obs/,
  screenshot sorting, the "what to sort" settings fields, and platform/sound-player
  (win + stub; link winmm in CMake on WIN32 only).
- Integrate each WP using plan.md §3.3. Read every diff in full. Reject changes outside a
  worker's file ownership. git add new files before running build-aux/check-format. Run
  the unit tests, the format check, and the local ubuntu compile check.
- Don't push glue code whose dependencies aren't on the branch yet (MSVC fails to link
  unresolved symbols even though Linux builds pass). Commit locally and push when the
  dependency lands.
- After integrating each WP, update its Status in plan.md, and add any decision you made
  to the Decision log.

Product decisions already made (do not reopen them):
- The auto Replay Buffer reacts only to real game hooks.
- WP2.5 save sound: optional, off by default, plays through Windows (no OBS source, no
  audio device routing). One line of help text says Desktop Audio will capture it. Until
  I confirm the ElevenLabs "Replay saved" WAV's licence allows redistribution, do NOT
  bundle any sound file: the setting uses a .wav file I choose, plus a Test button.

Git and GitHub (all authorized):
- Commit after each integrated WP with the message "WP<ID>: <title>". Push regularly with
  git push -u origin claude/m2-recordings-screenshots.
- Check the GitHub Actions results with the GitHub MCP tools. When CI is red, find the
  real cause before pushing again. Never skip or disable a test to get CI green.
- At the milestone gate (plan.md §3.4), open a pull request against main. In its
  description, say it is stacked on #2 (M1) and #1 (M0). Subscribe to its activity. Fix CI
  failures and review comments as they arrive.

Stop and report to me:
- at the end of M2. Give me the PR link, a summary of each WP, the CI result, the manual
  test checklist (docs/testing/M2.md) for me to run in OBS on Windows, and any open
  risks. I'm not a developer: write the checklist and download steps as simple numbered
  steps, and link the exact Actions run whose Artifacts section has the Windows zip. Then
  wait for my test results.
- earlier if a product decision isn't covered by docs/PLAN.md or the decisions above, if
  a WP needs a new dependency, or if anything would delete or move user files in a way
  the plan doesn't describe.

Be honest about what you verified. The container runs Linux. Windows behavior is verified
only by CI and by my manual testing, so never call Windows-only behavior "working" before
then.
```
