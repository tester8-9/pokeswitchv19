# V19 - move-threat-aware dynamic switch scorer

V19 builds on V18's dynamic trainer-team mapping and adds move-threat profiles.

Main change:
- Defensive threat evaluation no longer uses only target species STAB.
- Cinderace / Libero and Greninja / Protean-style targets are evaluated by plausible move threat types instead of only current/base typing.
- This prevents bad assumptions such as treating a Normal-type Cinderace-after-Protect as safe for Lucario when Pyro Ball can be clicked next turn.

Key log labels:
- v19_team
- v19_score
- v19_gate
- switch_policy_v19
- candidate_score_after_switch_policy_v19

Use the same config as V18 unless otherwise instructed.
