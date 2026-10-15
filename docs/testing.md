# Deployment testing

## Required production acceptance

A deployment candidate passes only when all checks succeed:

1. The exact user service is active.
2. The engine health endpoint returns success.
3. The WebUI status endpoint reports a running, non-error state.
4. A real completion request returns a valid response.
5. A controlled service restart completes, and checks 1–4 pass again.

## Isolated tests

Temporary tests may use an available isolated GPU and disposable port. Do not retain a permanent test service, old request/response captures, or historical benchmark output. Keep only scripts that remain necessary to repeat the current acceptance checks.

## Cache-clear changes

When changing the cache-clear API or WebUI actions, test successful idle clearing, busy refusal, drain/abort behavior where supported, status lookup, and metric/error reporting. Tests must prove a negative case as well as a successful operation.
