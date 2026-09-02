# Host actions

`libkobox2` emits one action at a time. Every action carries a nonzero token and
sandbox generation. The host reports completion with the same values before
delivering another command or event.

| Action | Input | Successful completion |
|---|---|---|
| `ALLOCATE_RESOURCES` | launch digests, limits and flags | new `resource_set_id` |
| `LAUNCH_SANDBOX` | launch digests and `resource_set_id` | new `sandbox_id` |
| `TRANSFER_RESOURCES` | both IDs | no payload |
| `REVOKE_RESOURCES` | resource ID and live sandbox ID, if any | no payload |
| `RESET_RESOURCES` | resource ID | no payload |
| `TERMINATE_SANDBOX` | sandbox ID | no payload |
| `RELEASE_RESOURCES` | resource ID | no payload |

IDs are nonzero, opaque, scoped to one controller and generation, and never
contain a native handle. Only allocation and launch return IDs. Failed actions
return `RESOURCE_DENIED`, `RESOURCE_EXHAUSTED`, or `HOST_FAILURE` with zero IDs.

Start orders allocation, launch, transfer, and handshake. Stop and restart order
revocation, required reset, termination, and release. Restart then begins a new
generation with new IDs.
