# Contributing

## Versions and branches

Graph Sync, Graph Mail and their shared core (plasma-graphcore-linux) keep one version series.

- The suite shares **major.minor**; the patch number may differ per app (one app at 0.11.1 next to
  another at 0.11.0).
- Within a release series (0.11.x) everything the apps share stays compatible: the shared core's
  library interface (ABI and API), the D-Bus interfaces, and the configuration, cache and state
  formats (new settings keys are fine; older versions ignore them). A patch release holds fixes,
  and small features only when they change none of these. Anything else waits for the next minor
  (0.12.0) or major (1.0.0).
- Each series has a maintenance branch named after it (`0.11`). Fixes go there and are merged into
  `main`; `main` holds the next series.

## Building and testing

See README.md. `ctest --test-dir build` must pass.
