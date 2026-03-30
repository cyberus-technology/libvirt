# AGENTS.md

## For Humans

This file defines the minimal rules for LLMs/agents helping the Cyberus
Technology Cloud Team debug and modify this repository.

This repository is a Cyberus Technology fork of libvirt. It is maintained
independently from upstream, but most changes will likely be upstreamed
later. Upstream currently has no policy regarding LLMs/agents. Every developer
**must take full responsibility** for their work and the LLMs/agents they work
with, including whether and how upstream accepts such work. So, until the
situation with upstream is clearer, please use LLMs/agents primarily for
prototyping, debugging, and as a sparring partner rather than for production
work or work likely to be proposed upstream.

The goal is not to prescribe detailed style, but to keep agent work safe,
correct, reviewable, and compatible with CI and normal engineering practice,
while leaving room for developer preferences. 

## For Agents

### Scope

We primarily develop the `ch` driver in `./src/ch`, the libvirt driver for
the Cloud Hypervisor VMM on Linux/KVM. We have full control over that driver.
Outside `./src/ch`, changes should usually be limited to bug fixes.

### Priorities

Our main goals are production-grade VM lifecycle management and live migration.

Libvirt is a C project using `glib` and its own helpers to provide
abstractions such as a ref-counted object model.

### Expectations

Help prevent common C footguns. In particular:

- Ensure proper cleanup on all failure paths, typically via `goto` labels.
- Prevent memory leaks, null pointer dereferences, and race conditions.
- Use libvirt's job model correctly for synchronous and asynchronous work.
- Look at existing libvirt code for patterns when needed.

### Code Style

- There are no lint, formatting, or styling tools in this repo
- Stick to the conventional style in other parts of the code base

### Reference Implementation

Most of our code is inspired by the QEMU backend, which is the closest match
to what we need to build. When in doubt, check how QEMU does it.

### Commit Style

Make sure commits follow these rules:

- Keep history reviewable and structured
- Show clear progression from A to B
- Avoid monolithic commits
- Every commit must be bisectable


### Building / Testing

Execute 

```
meson setup build \
    -Dtests=disabled \
    -Dexpensive_tests=disabled \
    -Ddocs=disabled \
    -Ddriver_ch=enabled \
    -Ddriver_qemu=disabled \
    -Ddriver_bhyve=disabled \
    -Ddriver_esx=disabled \
    -Ddriver_hyperv=disabled \
    -Ddriver_libxl=disabled \
    -Ddriver_lxc=disabled \
    -Ddriver_openvz=disabled \
    -Ddriver_secrets=disabled \
    -Ddriver_vbox=disabled \
    -Ddriver_vmware=disabled \
    -Ddriver_vz=disabled \
    -Dstorage_dir=disabled \
    -Dstorage_disk=disabled \
    -Dstorage_fs=enabled \
    -Dstorage_gluster=disabled \
    -Dstorage_iscsi=disabled \
    -Dstorage_iscsi_direct=disabled \
    -Dstorage_lvm=disabled \
    -Dstorage_mpath=disabled
```

in the local `nix develop` shell to build libvirt with just the `ch` backend.
There are no unit tests we use.

If this doesn't work, use the Nix build (takes fairly long, no incremental 
rebuilds): `nix build -L .#libvirt`
