---
description: Stage changes, create a Conventional Commits message, and push
---

Create a git commit following the project's commit conventions (see CLAUDE.md),
then push it.

1. Run `git status` and `git diff` to review what changed.
2. Group into one logical commit; suggest splitting if unrelated changes are mixed.
3. Write a Conventional Commits message: `<type>: <imperative summary>`,
   lowercase, no trailing period, max 50 chars. Add a body only if needed.
4. Stage the relevant files and commit.
5. Push the commit (`git push`, using `-u origin <branch>` if no upstream is set).
6. Show `git log -1`.

All commit text must be in English.
