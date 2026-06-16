# AI Bridge v18 — dynamic trainer-team switch inference

V18 removes the Ian-only action-to-species mapping used in v17.

Instead, it builds a static trainer-team table from the trainer dump and resolves the active CPU team at runtime by matching the active species plus the candidate fallback species. Then it treats switch candidate action IDs as party-slot destinations.

This is meant to generalize across ordinary trainer/gym battles:

- if the opposing team changes, the action -> switch-in species mapping should follow that trainer's team;
- if the player team changes, visible target species are read dynamically from the live battle target object;
- if a species or team cannot be resolved, the scorer stays conservative rather than inventing a switch.

V18 still does not yet read every runtime party slot directly from memory; it uses a generated trainer-team map as the bridge. The next improvement would be to locate the true party array pointer in battle memory.
