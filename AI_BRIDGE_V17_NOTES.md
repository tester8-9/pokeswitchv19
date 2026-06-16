# AI Bridge V17 notes

V17 keeps the broad v16 matchup scorer but fixes one important issue from the v16 log: Fake Out was being treated as a global positive move and could block switches for the other active Pokémon. V17 narrows the Fake Out tempo block to active Mienshao only until the exact move-owner field is mapped.

V17 also adds broader role/matchup heuristics:
- penalize switch-ins that are threatened and have no offensive upside;
- reward pivots that improve defensive fit and threaten at least one visible opponent;
- reward switching more when the active Pokémon is in danger and the best move score is weak;
- keep blocking Lucario/Cinderace-style bad pivots through the type chart.

This is still not direct execution of btl_ai_pokechange.amx. It is a source-level bridge scorer inspired by the same kind of logic.
