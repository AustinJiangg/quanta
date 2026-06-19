---
description: Stage changes and create a Conventional Commits message (local only)
---

Create a git commit following the project's commit conventions (see CLAUDE.md).
Commit locally only — do not push.

1. Run `git status` and `git diff` to review what changed.
2. Group into one logical commit; suggest splitting if unrelated changes are mixed.
3. Write a Conventional Commits message: `<type>: <imperative summary>`,
   lowercase, no trailing period, max 50 chars. Add a body only if needed.
4. Stage the relevant files and commit.
5. Show `git log -1`. Do not push — remind the user they can `git push` when
   ready (use `-u origin <branch>` if no upstream is set).

All commit text must be in English.
