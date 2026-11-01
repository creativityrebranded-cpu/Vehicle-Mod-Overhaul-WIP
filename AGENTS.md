# Development requirements

Read docs/DEVELOPMENT_MANDATE.md before changing this mod. All updates must remain backwards compatible with prior versions: preserve existing saves, data/settings, route groups, content identities and supported public integrations. Use non-destructive migrations/adapters; do not silently reset user data, require a new game, or add avoidable interface breaks. A version bump does not waive compatibility. Any unavoidable exception requires explicit user approval before shipping.

Traffic restores the individual game save's immutable snapshot and edits a separate working copy. Preserve older saved profile formats; never claim old history exists if it was not captured. Keep player release DLLs (FVO_RELEASE=ON) separate from diagnostic local test DLLs (FVO_RELEASE=OFF). Offline tests do not establish in-game acceptance.
