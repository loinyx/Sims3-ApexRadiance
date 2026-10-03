# Agent guidance

Read the project documentation relevant to the requested change; do not load the full documentation set for an unrelated edit. Reusable workflows are versioned with the project:

- `.agents/skills/apex-menu/SKILL.md` covers menu and UI changes. Its Claude Code discovery copy is `.claude/skills/apex-menu/SKILL.md`; keep both copies and their audit script in sync.
- `.codex/skills/apex-compile-project/SKILL.md` covers builds and reuses a machine-local recipe.
- `.codex/skills/apex-version-pr-workflow/SKILL.md` covers version branches, commits, draft PRs and multi-agent continuity.
- `.codex/skills/apex-review-pr-release/SKILL.md` is required before releases.

Follow `CLAUDE.md` for Apex-specific behavior, source conventions and user decisions when the agent reads that file. Treat skill instructions as workflow guidance, not permission to merge, publish, install, or perform unrelated external actions. Read only the skill relevant to the task.
