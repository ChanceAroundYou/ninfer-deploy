# Operations

## Read-only checks

Use the service manager, listener inspection, GPU process inspection, engine health/metrics endpoint, and WebUI status endpoint to establish the active deployment before changing it.

## Change procedure

1. Record the active service, executable, model path, runtime dependencies, and configuration.
2. Prepare a replacement without modifying the active production path.
3. Schedule a maintenance window.
4. Switch the service to the replacement using its exact service unit; do not use broad process matching.
5. Verify service state, engine health, WebUI status, and a real generation request.
6. Perform one controlled service restart and repeat the checks.
7. Only then mark replaced binaries, environments, models, or staging files as deletion candidates.

## Rollback

Keep the last known-good active deployment untouched until the replacement has passed every verification step. If verification fails, restore the prior service configuration and report the failed check; do not delete either candidate while the cause is unresolved.

## Secrets

Never inspect or publish `qdl.env` contents. It is neither a deployment input stored in Git nor a routine cleanup target.
