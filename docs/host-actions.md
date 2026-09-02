# Host actions

`libkobox2` emits one action at a time. Every action carries a nonzero token and
sandbox generation. The host reports completion with the same values before
delivering another command or event.

| Action | Input | Successful completion |
|---|---|---|
| `ALLOCATE_RESOURCES` | closure view, launch digests, limits and derived flags | new `resource_set_id` |
| `LAUNCH_SANDBOX` | closure view, launch digests and `resource_set_id` | new `sandbox_id` |
| `TRANSFER_RESOURCES` | both IDs | no payload |
| `QUIESCE_SANDBOX` | both IDs | process reported `STOPPED` and exited |
| `TERMINATE_SANDBOX` | `sandbox_id` | process exited |
| `REVOKE_RESOURCES` | `resource_set_id` | no payload |
| `RESET_RESOURCES` | resource ID | no payload |
| `REAP_SANDBOX` | exited `sandbox_id` | no payload |
| `RELEASE_RESOURCES` | resource ID | no payload |

IDs are nonzero, opaque, scoped to one controller and generation, and never
contain a native handle. Only allocation and launch return IDs. Failed actions
return `RESOURCE_DENIED`, `RESOURCE_EXHAUSTED`, or `HOST_FAILURE` with zero IDs.

Start orders allocation, launch, transfer, and handshake. Normal stop and restart
order quiesce, process exit, revocation, required reset, reap, and release. Fault
cleanup orders termination, process exit, revocation, required reset, reap, and
release. Restart then begins a new generation with new IDs.
