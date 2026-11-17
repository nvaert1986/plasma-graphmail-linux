# Contributing

## Versions and branches

Graph Sync, Graph Mail and their shared core (plasma-graphcore-linux) keep one version number,
major.minor.patch.

- Within a release series only the last digit changes, and only for fixes: in 0.11.x the
  configuration, the mail cache and every interface to the other apps of the suite stay compatible.
  New features wait for the next minor (0.12.0) or major (1.0.0).
- Each series has a maintenance branch named after it (`0.11`). Fixes go there and are merged into
  `main`; `main` holds the next series.

## Building and testing

See README.md. `ctest --test-dir build` must pass.
