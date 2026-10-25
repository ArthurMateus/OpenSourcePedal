# GEMINI.md

Read [`AGENTS.md`](AGENTS.md). It is the source of truth for every rule, command and workflow in this repository.
For a role prompt, read the matching file in `.claude/agents/` (plain Markdown; ignore the YAML frontmatter).

Gemini/Antigravity agents usually act as the **second-model-family code reviewer**: follow `.claude/agents/code-reviewer.md`
and `.claude/commands/review-pr.md`. Sign your verdict as `reviewed-by: gemini/<role>`.
