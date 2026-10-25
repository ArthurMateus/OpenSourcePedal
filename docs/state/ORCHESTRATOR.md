# Orchestrator state (pointer)

The **live** orchestrator state lives on the unprotected `state` branch, so that heartbeat updates need no PR:

```bash
git fetch origin state && git show origin/state:docs/state/ORCHESTRATOR.md
git show origin/state:docs/state/BLOCKERS.md
```

To take over as orchestrator, paste `docs/MASTER_PROMPT.md` into a fresh agent with **RESUME** appended.
