# AI Bridge V18 - General roster-aware switch scorer

V18 removes the Ian-only action mapping used by V17.

V17 assumed:
- action 2 -> Conkeldurr
- action 3 -> Lucario
- action 4 -> Pangoro

That worked for Ian, but it was not general. V18 adds a static trainer-roster table generated from the uploaded/current `trparse.txt` trainer dump. At runtime, the bridge tries to match the active Pokemon and candidate fallback species to a trainer roster, then interprets switch action IDs as party slots.

Result:
- Player-side targets are still read live from battle memory, so changing your own team is supported.
- Opponent switch-in species are inferred from current trainer rosters, so this should work across many normal trainers, not only Ian.
- If no safe roster match is found, V18 falls back to the live candidate pointer species rather than inventing a switch-in.

This is still not the final perfect VGC AI. It is the first broad/general version that does not rely on one hard-coded Ian team.

Log lines to look for:
- v18_roster action=... active=... fallback=... trainer=... species=... score=...
- v18_score ...
- v18_gate blocked/allowed ...
- switch_policy_v18 ...

Use the same config shape as V17, but keep force_existing_action_max = 5 so party-slot switch IDs 2-5 can be evaluated when available.
