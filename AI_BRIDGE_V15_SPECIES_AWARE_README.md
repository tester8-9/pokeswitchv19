# AI Bridge V15 - first species-aware switch gate

V14 mapped useful runtime species fields:

- `cs_78` points at a switch-in candidate object; in Ian's first window it identified Lucario.
- `cs_80` points at the active Pokémon being evaluated; in Ian's first window it identified Mienshao, later Gallade.
- move-evaluation target objects expose opponent/ally species at `eval_target + 0x70`.

V15 adds a first hard-coded species-aware safety gate:

1. Do not switch out Mienshao while it has a positive-scoring move window, because turn-1 Fake Out / immediate tempo is valuable.
2. Do not switch Lucario into a visible Cinderace board unless the move-score position is actually terrible.

This is not the final universal VGC switch scorer yet. It is the first playable proof that the bridge can use actual runtime species identities instead of only action IDs.
