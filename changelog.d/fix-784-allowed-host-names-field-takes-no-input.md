### Fixed
- **The Host check settings now look locked when the environment fixes them**, AlpacaHTTP web UI, issue #784. With `ALPACAHTTP_ALLOWED_HOSTS` or `ALPACAHTTP_HOST_CHECK` set, the "Allowed host names" field and the "Restrict Host names" toggle were read-only but looked editable, so they seemed to take no input. A locked field is now dimmed with a dashed border, and a note under it names the variable and says to change it in the systemd drop-in.

### Changed
- **`/deploy-test` no longer installs the `ALPACAHTTP_ALLOWED_HOSTS=.lan` systemd drop-in**, issue #784. The drop-in made the web UI "Allowed host names" field read-only. Step 4 now removes the old `allowed-hosts.conf` if a rig has it and reloads systemd before the restart; `.lan` is allowed through the web UI field or `http.allowed_hosts`. The `/conformu` notes say the same.
