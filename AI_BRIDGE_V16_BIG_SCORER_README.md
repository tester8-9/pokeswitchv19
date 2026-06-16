# AI Bridge V16 — Broad Matchup-Aware Switch Scorer

This version is the first large scoring step instead of another tiny probe.

## What V16 adds

- Full 18-type effectiveness table in C++.
- Full move-type lookup generated from `MoveData.txt` for move IDs 1-826.
- Runtime species/type mapping for the known/likely Sword/Shield trainer Pokémon plus all Gen 8 species.
- Switch-in defensive scoring using visible target/opponent species from the live battle objects.
- Switch-in offensive pressure scoring using switch-in STAB into visible targets.
- Active Pokémon danger comparison: penalizes switching into a worse defensive matchup than the current active Pokémon.
- Fake Out tempo preservation: blocks switching if Fake Out has a positive score in the current move-evaluation window.
- Hard bad-switch penalties such as Lucario into visible Cinderace or Pangoro into visible Fairy boards, generalized through type chart scoring.
- Conservative unknown-species fallback: if a species type is unknown, do not invent a positive switch score.

## Important limitation

V16 still does not fully read HP, Dynamax state, or all party-role fields. It uses runtime species, the live move-score summary, the switch-in candidate object, and the current target/opponent species. HP preservation logic is planned after the HP fields around active/switch candidate objects are mapped.

## Recommended config

Paste this as your full `[ai_bridge]` section:

```toml
[ai_bridge]
active = true
install_candidate_hooks = true

output_readback_post_offset = 8211420
candidate_list_post_offset = 9342476
candidate_score_post_offset = 9342576
final_selection_post_offset = 9342816

hook_output_readback_post = true
hook_candidate_list_post = false
hook_candidate_score_post = true
hook_final_selection_post = false

log_hits = true
max_log_hits = 160
candidate_dump_count = 8

force_output_score = 0
force_pokechange_enable = -1

force_final_selection = false
force_final_action_id = 0
force_final_score = 1000000

force_candidate_entry = false
force_candidate_slot = 0
force_candidate_action_id = 0
force_candidate_score = 1000000
force_candidate_enable = true

force_existing_candidates = false
force_existing_action_min = 2
force_existing_action_max = 3
force_existing_score = 1000000
force_existing_enable = true

switch_policy_mode = 7
switch_min_hit = 1
switch_score = 115
switch_max_forces_per_battle = 99
switch_max_forces_per_action = 99
switch_cooldown_hits = 0
switch_action2_score = 0
switch_action3_score = 0

switch_max_candidates_per_hit = 1
switch_require_candidate_count = 2
switch_min_native_score = 100
switch_max_native_score = 100
switch_preferred_action = 0
switch_alternate_actions = true
switch_native_score_only = false
switch_disable_after_force = true

switch_gate_use_move_summary = true
switch_gate_best_move_max = 99
switch_gate_require_negative_record = false
switch_gate_negative_record_threshold = -20
switch_gate_min_move_records = 12

score_survey_mode = true
score_survey_dump_state = false
score_survey_dump_readback_x0 = false
score_survey_dump_readback_out = false
score_survey_words = 32
```

## What to look for in logs

Search for:

- `v16_score`
- `v16_gate blocked`
- `v16_gate allowed`
- `switch_policy_v16`

If you see `v16_gate allowed` followed by `switch_policy_v16`, the bridge allowed a matchup-aware switch candidate. If you see only blocked lines, it is refusing to switch because the matchup or move-score opportunity was not good enough.
