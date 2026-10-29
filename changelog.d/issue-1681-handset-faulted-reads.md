### Breaking changes
- **SynScan and Celestron NexStar state reads now report communication faults** (AlpacaCore, issue #1681): pulse, slew, park/home, tracking, rate, pier-side and mount-derived site/time reads no longer answer while a link fault is latched. **After upgrading:** handle the communication error and retry after the handset responds or reconnect explicitly.

### Fixed
- **SynScan and Celestron NexStar refuse faulted commanded-state reads** (AlpacaCore, issue #1681): operational getters check the existing link-health latch without sending a device command, rate checks use the narrow fault mutex, and position replies still restore service automatically; safety-stop commands retain their existing paths. See Breaking changes for the client-visible consequence.
