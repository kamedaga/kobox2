# Tests

Controller tests cover configuration rejection, lifecycle transitions,
generation rollover, host failure, and crash/restart. Protocol tests cover
round trips, bounds, schema mismatch, reserved fields, region rights, and
message envelopes.

On Linux, the test host starts a separate sandbox process, transfers one shared
memory object and four notification objects over a bootstrap socket, and checks
the shared-memory acknowledgement and notification. Restart exercises
revocation, process exit, fresh objects, and a new generation. Device-specific
acceptance tests remain in the host OS repository.
