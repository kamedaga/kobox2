# Linux test host

The test adapter executes the `libkobox2` host-action contract on Linux. It
allocates a shared-memory object and notification objects, starts a separate
sandbox process, and transfers those handles over a sequenced-packet bootstrap
socket.

The child validates the channel schema, maps the transferred memory, records its
generation in shared memory, and signals the transferred notification object.
Revocation makes the child unmap and close every transferred handle before it
acknowledges and exits. Restart allocates fresh handles and IDs for the next
generation.

This adapter verifies the controller/host boundary. Production isolation and
capability enforcement belong to each host port.
