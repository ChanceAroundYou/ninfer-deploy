# Model artifacts

## Stable location

The production model belongs under:

```text
~/llm-deploy/models/
```

The current production artifact name is `swift-qwen38-q4km.ninfer`. The model file is a runtime asset and is never committed to this repository.

## Migration and verification

1. Copy the active model to its stable destination; do not move or remove the only working copy.
2. Compute SHA-256 for both source and destination and require exact equality.
3. Update the candidate service configuration to the stable model path.
4. Complete the full operational verification sequence.
5. Delete the old copy only after the candidate is active, verified, and restart-tested.

Do not record cookies, download URLs, or other credentials in this document. `qdl.env` is protected and requires a separate authorization for deletion.
