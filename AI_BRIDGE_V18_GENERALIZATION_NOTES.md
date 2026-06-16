# AI Bridge V18 - Generalization notes

V17 proved row-specific switch scoring works, but it still had an Ian-specific action-to-party-slot fallback.

V18 expands the species-name and species-type coverage substantially so the scorer can recognize many more Sword/Shield/Galar species in runtime logs. This is a step toward cross-battle behavior, but the fully generic version still needs a battle-roster cache that maps each trainer party slot to species at runtime.

Current behavior goal:
- Use runtime candidate pointers when available.
- Use broad species/type lookup instead of only Ian/Cinderace/Grimmsnarl.
- Continue blocking known bad defensive switches.
- Keep logging v17_score/v17_gate lines for tuning.

Future V19 target:
- Build a per-battle roster cache:
  party slot -> species -> types -> current HP/status/role
- Stop relying on Ian-specific party ordering.
- Use the roster cache for any trainer, any player team, and any opponent team.
