# Deployment and rebuild

## Scope

This document defines the current deployment layout. It does not contain credentials, model download URLs, or model files.

## Runtime layout on the AI host

```text
~/llm-deploy/
├── src/ninfer-fusion-kvmem/  # clean source checkout at a documented commit
├── runtime/
│   ├── webui-venv/           # dedicated WebUI Python environment
│   └── build-current/        # validated NInfer build
├── models/                   # production model artifacts; not versioned
├── webui/                    # deployed WebUI source and non-secret config
└── staging/                  # disposable download staging
```

## Rebuild rules

1. Check out the required source commit from this repository.
2. Create a new, separate build directory; never reuse an unknown build tree.
3. Recreate the dedicated WebUI environment from the versioned dependency definition.
4. Place the model in `~/llm-deploy/models/` and verify its SHA-256 before configuring the service.
5. Install the versioned systemd template only after filling non-secret host-specific paths.
6. Validate the candidate deployment according to `testing.md` before treating it as production.

Build outputs, Python environments, models, staging, and logs remain local runtime assets and are ignored by Git.
