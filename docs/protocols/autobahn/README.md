# Autobahn WebSocket Reports

This directory contains the Autobahn fuzzing-client config and should hold the
generated reports for WebSocket production-readiness verification.

Run command:

```bash
sudo -n docker run --rm --network host \
  -v "$PWD/docs/protocols/autobahn:/reports" \
  crossbario/autobahn-testsuite \
  wstest -m fuzzingclient -s /reports/fuzzingclient.json
```

Current status: not executed in this environment because pulling the Docker image
failed with a refused connection to Docker Hub. No conformance report is archived
yet.
