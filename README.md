# NInfer deployment

This repository is the versioned source of truth for the NInfer deployment on the AI host. It contains the NInfer/KVMem source tree, the WebUI source, deployment definitions, and current operational documentation.

It does **not** contain models, build directories, virtual environments, download staging, logs, credentials, or historical benchmark output.

## Layout

- `src-tree/fusion-engine-src/` — NInfer/KVMem source and upstream build/test inputs.
- `deploy/webui/` — WebUI source and non-secret configuration template.
- `deploy/` — versioned runtime definitions, systemd templates, scripts, and minimal validation tests.
- `docs/` — current deployment, operations, model-artifact, testing, and design documentation.
- `LICENSE`, `NOTICE.md` — license and provenance notices.

## Operating rule

Production changes require a maintenance window and must validate service state, engine health, WebUI status, a real generation request, and restart recovery. See `docs/operations.md`.

The production model remains an AI-host runtime asset at `~/llm-deploy/models/`; never commit it or its download credentials.
