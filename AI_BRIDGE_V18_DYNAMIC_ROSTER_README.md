# AI Bridge v18 - dynamic roster switch mapping

V18 makes the switch-in identity mapping more general.

What changed from v17:
- v17 had Ian-specific logic: action 2 -> Conkeldurr, action 3 -> Lucario, action 4 -> Pangoro.
- v18 generates a trainer roster table from the current `trparse.txt` trainer dump.
- For each switch action row, v18 uses:
  - the runtime active species (`cs_80`),
  - the runtime fallback switch candidate (`cs_78`), and
  - the generated trainer roster table
  to infer the likely switch-in species for `action=2`, `action=3`, etc.

What this means:
- If your PLAYER team changes, the opposing target species are still read live from battle memory.
- If the OPPONENT is a different trainer, v18 can infer switch destinations from the generated roster table.
- If you later edit trainer teams again, the roster table can become stale. Rebuild v18 from an updated `trparse.txt` after major trainer-team edits.
- If v18 cannot infer the switch-in safely, it falls back to the runtime candidate pointer or blocks the switch rather than guessing.

Look for these log lines:
- `v18_roster`
- `v18_score`
- `v18_gate blocked`
- `v18_gate allowed`
- `candidate_score_after_switch_policy_v18`
