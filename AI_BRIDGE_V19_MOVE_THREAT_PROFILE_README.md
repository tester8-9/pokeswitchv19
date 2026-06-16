# AI Bridge v19 — move-threat-aware switch scorer

This version builds on v18 dynamic team mapping.

Key change:
- v18 scored visible threats mostly from their species typings.
- v19 uses a move-threat profile for Pokémon whose current type can mislead the switch scorer, especially Cinderace/Libero and Greninja/Protean-style attackers.

Why:
A Cinderace can temporarily become Normal-type after Protect/Feint/other type-changing behavior, but it still threatens Lucario with Fire coverage such as Pyro Ball. v19 therefore scores Cinderace as a Fire/Steel/Fighting/Normal move threat profile rather than trusting a single visible/current type.

Expected logs:
- v19_team
- v19_score
- v19_gate blocked / allowed
- switch_policy_v19

Use switch_policy_mode = 7 with the same v18 config block.
