# Contributing

Changes are welcome when they preserve the correctness contract.

## Required sequence

1. Add or update a deterministic test for the changed invariant.
2. Implement the smallest contract change.
3. Run `make test` or CMake plus CTest.
4. Run `make validate-shaders` when `glslangValidator` is installed.
5. Keep shader binding changes synchronized with `docs/integration.md`.
6. Record user-visible behavior in `CHANGELOG.md`.

## Non-negotiable rules

- FULL is authoritative; relay output is conditional acceleration.
- A failed proof must never advance history.
- Old EIN generations must never address recycled resources.
- Unknown transitions use a conservative barrier until the backend specializes them.
- Optimization may increase native refinement work, but it may not weaken validation silently.
- LAN discovery is advisory; it never grants execution authority.
- Remote output must be locally hashed and proven before its lease can complete.

Use focused commits and do not mix formatting-only changes with behavior changes.
