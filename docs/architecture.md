# Architecture boundary

## Controller

The kobox2 controller is a library used by a host-owned service such as PachaOS
`gpud`. It owns lifecycle state, validates an opaque role manifest, establishes
channels, transfers bounded resources, and supervises restart generations.

It does not own the device data path and does not understand Linux APIs.

## Linux sandbox

The GPL-2.0-only sandbox is a separate process. It owns the `.so`/`.ko`
self-loader, Linux core primitives and subsystem state, device-facing queues,
DMA mappings, IRQ handling, and Linux driver objects.

Each role receives only its declared resources and loads only its declared
dependency closure. GPU and storage roles do not share Linux runtime state,
DMA domains, generations, or device capabilities.

## Host-owned integration

The host OS owns service names, process bootstrap, role manifests, device
selection, capability policy, and packaging. A PachaOS port therefore stays in
the PachaOS repository rather than becoming a dependency of this repository.

The initial development order is:

1. sandbox lifecycle and a fixture module;
2. virtio-gpu VirGL command submission and render-node operation;
3. Mesa, Xorg and Xfce acceleration through PachaOS `gpud`;
4. NVMe and ext4 through the same controller boundary;
5. AMDGPU on RX 9060 XT as the long-term GPU profile.

Virtio-gpu 2D rendering is not a milestone or fallback path. The VirGL gate
will require real 3D submission and reject llvmpipe/swrast fallback.
