# Standing compatibility requirement

Explicit user instruction, 2026-11-01: all future updates must remain backwards compatible with prior versions. Preserve existing saves, routes, route groups, settings, content identities and supported integrations. Use additive changes, compatibility adapters and non-destructive migrations. Do not silently reset data, require a new playthrough, or introduce an avoidable interface break. An unavoidable exception requires explicit user approval before shipping.

Traffic configuration must be restored from the individual game save's immutable snapshot, with a separate editable working copy. Continue reading/migrating older profile formats; do not claim historical snapshots exist where an older version did not record them. Retain supported API consumers and read-only integrations.

Intentional routes inside or through structures are allowed. Building intersections produce red clearance warnings, not route-editing or saving refusals. Proposed clearance adjustments remain optional. Keep separate validation for invalid coordinates and proven terrain violations.

Validate prior-version data, old/new save branching and supported integrations before release. Offline checks are not in-game proof. Release player DLLs with FVO_RELEASE=ON and retained crash reporting; keep diagnostic test DLLs FVO_RELEASE=OFF.
