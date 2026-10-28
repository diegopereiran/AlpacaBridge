### Fixed

- **HTTP server: a worker left over from before a restart no longer strands the next client** (AlpacaHTTP): a worker that outlived a restart could take the wake permit released for a queued connection and exit without passing it on, so that connection waited until another request arrived. The exiting worker now hands the permit back. Found by the nightly ASan job (`test_server_socket`, run 40775992105); a worker outlives a restart only on a fault path, so a normal management restart was not affected.
