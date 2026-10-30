#!/usr/bin/env bash
# Claude <-> Codex collaboration bridge. See docs/COLLAB.md.
#
#   tools/codex.sh ask    "<question>"            read-only question about the repo
#   tools/codex.sh review <path>... [-- "<focus>"] read-only code review of files/dirs
#   tools/codex.sh diff   [-- "<focus>"]          read-only review of uncommitted git changes
#   tools/codex.sh task   <TASK-ID>               Codex implements a task from collab/TASKS.md (workspace-write)
#   tools/codex.sh resume "<follow-up>"           continue the most recent Codex session
#
# Every answer is saved under collab/out/<timestamp>-<mode>-<slug>.md and printed to stdout.
# Env: CODEX_MODEL (optional model override), CODEX_BIN (default: codex).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT/collab/out"
CODEX_BIN="${CODEX_BIN:-codex}"
mkdir -p "$OUT_DIR"

die() { echo "codex.sh: $*" >&2; exit 2; }

slugify() { printf '%s' "$1" | tr -cs 'A-Za-z0-9' '-' | cut -c1-40 | sed 's/^-//; s/-$//'; }

PREAMBLE="You are Codex, collaborating with Claude (Claude Code) on SandVM, a C++20 WHPX-based VMM with Sandboxie-Plus integration.
Read AGENTS.md and docs/COLLAB.md first and follow them. Answer in English, concise, technical.
Cite code as path:line. If you are unsure of a WHPX/x86 detail, say so instead of guessing."

# run_codex <mode> <slug> <sandbox> <prompt>
run_codex() {
  local mode="$1" slug="$2" sandbox="$3" prompt="$4"
  local out="$OUT_DIR/$(date +%Y%m%d-%H%M%S)-$mode-$(slugify "$slug").md"
  local args=(exec -C "$ROOT" -s "$sandbox" --skip-git-repo-check --color never -o "$out")
  [[ -n "${CODEX_MODEL:-}" ]] && args+=(-m "$CODEX_MODEL")
  printf '%s\n\n%s\n' "$PREAMBLE" "$prompt" | "$CODEX_BIN" "${args[@]}" - >"$out.log" 2>&1 || {
    echo "codex exited with an error, log: $out.log" >&2
    tail -n 30 "$out.log" >&2
    exit 1
  }
  echo "=== Codex ($mode) -> ${out#$ROOT/} ==="
  cat "$out"
}

# Splits "a b -- focus text" into PATHS array and FOCUS string.
split_focus() {
  PATHS=(); FOCUS=""
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--" ]]; then shift; FOCUS="$*"; break; fi
    PATHS+=("$1"); shift
  done
}

cmd="${1:-}"; shift || true
case "$cmd" in
  ask)
    [[ $# -ge 1 ]] || die "usage: ask \"<question>\""
    run_codex ask "$1" read-only "Question from Claude:
$*"
    ;;
  review)
    split_focus "$@"
    [[ ${#PATHS[@]} -ge 1 ]] || die "usage: review <path>... [-- \"<focus>\"]"
    run_codex review "${PATHS[0]}" read-only "Review these paths: ${PATHS[*]}
Focus: ${FOCUS:-correctness bugs first (WHPX API misuse, x86 semantics, lifetime/ownership, thread safety), then design issues.}
Output format: a numbered list, most severe first. Each item: severity (high/medium/low), path:line, the problem, a concrete failure scenario, and the suggested fix. Do not modify files. End with 'VERDICT: <one line>'."
    ;;
  diff)
    split_focus "$@"
    git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 || die "not a git repo"
    run_codex diff "uncommitted" read-only "Review the uncommitted changes (run 'git diff' and 'git status' yourself; include untracked files).
Focus: ${FOCUS:-correctness first}.
Same output format as a review: numbered, most severe first, with path:line and fixes. Do not modify files. End with 'VERDICT: <one line>'."
    ;;
  task)
    [[ $# -eq 1 ]] || die "usage: task <TASK-ID>"
    grep -q "| $1 |" "$ROOT/collab/TASKS.md" || die "task $1 not found in collab/TASKS.md"
    run_codex task "$1" workspace-write "Implement task $1 from collab/TASKS.md.
Rules: only touch files needed for the task; follow the existing code style; do not commit.
When done, set the task status to 'review' in collab/TASKS.md and append a handoff entry to collab/HANDOFF.md (what you changed, what is untested, open questions for Claude)."
    ;;
  resume)
    [[ $# -ge 1 ]] || die "usage: resume \"<follow-up>\""
    out="$OUT_DIR/$(date +%Y%m%d-%H%M%S)-resume.md"
    printf '%s\n' "$*" | "$CODEX_BIN" exec resume --last -C "$ROOT" --skip-git-repo-check --color never -o "$out" - >"$out.log" 2>&1 \
      || { tail -n 30 "$out.log" >&2; exit 1; }
    echo "=== Codex (resume) -> ${out#$ROOT/} ==="
    cat "$out"
    ;;
  *)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
