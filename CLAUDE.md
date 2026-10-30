@AGENTS.md

## Start of every session
Read `collab/RESUME.md` first. It holds the current state and the next steps. Keep it updated before ending a session.

## Claude-specific
- Codex is the second engineer on this project. Ask it with `tools/codex.sh` (see `docs/COLLAB.md`).
- After any non-trivial change in `src/core/`, run `tools/codex.sh diff` (or `review <paths>`) and address every finding: fix it, or record why it was rejected in `collab/HANDOFF.md`.
- When a task is handed to Codex (`tools/codex.sh task <ID>`), review its diff before marking the task `done`.
- Talk to the user in Turkish.
