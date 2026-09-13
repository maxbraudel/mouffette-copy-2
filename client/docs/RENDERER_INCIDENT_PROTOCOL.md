# Qt Quick rendering incident protocol

For a crash, startup failure, data-affecting regression, broken remote scene,
or severe input/rendering issue:

1. stop the affected release promotion;
2. preserve logs, scene payload, platform, scale factor, screen topology, and
   reproduction steps;
3. revert or patch the offending change;
4. run the native CTest suite, `QmlArchitectureGate`, deterministic input and
   schema checks, then the affected platform E2E scenario;
5. record the root cause and add a focused regression test.

Do not restore a second renderer or introduce a runtime fallback as incident
mitigation.
