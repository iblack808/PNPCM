# OracleGPU Guest Runtime Shim

This directory contains a guest-side OracleGPU runtime shim. It is not a gem5
SimObject and it is not part of the host-side OracleGPU device model. It is a
Linux user-space library intended to run inside the FS-mode guest.

## Scope

The runtime shim only wraps the generic OracleGPU submission path:

- open `/dev/mem`
- map the OracleGPU MMIO range
- map guest-physical scratch regions used for the descriptor and completion flag
- construct a generic OracleGPU command descriptor
- submit the command through the MMIO doorbell
- poll the completion flag synchronously
- return MMIO-visible status and counters

It does not implement attention, matmul, Q/K/V, sequence semantics, scheduler
logic, or any other operator-specific behavior.

## ABI Ownership

The device-owned ABI lives in:

- `src/dev/oracle_gpu_protocol.h`

This directory's `oracle_gpu_abi.h` is only a wrapper that includes the device
header. That keeps the command layout, result-policy constants, and MMIO
register offsets in one place.

If you copy this runtime into a guest rootfs or another repository, make sure
the exact same ABI header is installed alongside it. Do not fork the struct
definitions unless you are intentionally revving the OracleGPU ABI on both the
guest and device sides.

## Current Memory Model

The current runtime API uses guest physical addresses for command inputs and
outputs. That is intentional.

The OracleGPU device consumes guest physical addresses in its descriptor, and
this user-space shim currently has no kernel driver or stable virtual-to-
physical translation path for arbitrary `malloc()` buffers. For the current FS
tests, the expected flow is:

1. Reserve known guest-physical scratch addresses.
2. Map them with `/dev/mem`.
3. Fill those regions from user space.
4. Submit the physical addresses through the runtime API.

For future inference-framework integration, you will likely need either:

- a kernel driver for OracleGPU buffer management, or
- a controlled physical-memory allocator/translation layer.

## Files

- `oracle_gpu_abi.h`: shared ABI wrapper
- `oracle_gpu_runtime.h`: public runtime API
- `oracle_gpu_runtime.c`: runtime implementation
- `oracle_gpu_runtime_test.c`: example guest test using the runtime API
- `Makefile`: builds the library and test

## Build

Build the static library, shared library, and test binary:

```bash
cd tests/test-progs/oracle-gpu/oracle_gpu_user
make
```

Outputs:

- `tests/test-progs/oracle-gpu/lib/x86/linux/liboraclegpu.a`
- `tests/test-progs/oracle-gpu/lib/x86/linux/liboraclegpu.so`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_runtime_test`

The test binary is linked statically by default so it can be injected into the
FS-mode guest without depending on the guest image's glibc version.

Install into a rootfs staging directory:

```bash
cd tests/test-progs/oracle-gpu/oracle_gpu_user
make DESTDIR=/path/to/rootfs install
```

That installs:

- headers into `/usr/local/include/oraclegpu`
- libraries into `/usr/local/lib`
- the example test into `/usr/local/bin`

## Run the Test in FS Mode

After building the test binary, run it inside your existing OracleGPU FS-mode
environment the same way as the earlier guest tests, or use the example script:

- `configs/example/gem5_library/x86-oracle-gpu-runtime-fs.py`

The test:

1. initializes the runtime,
2. maps three input regions and one output region from known physical scratch
   addresses,
3. submits a generic command using `ORACLE_GPU_RESULT_ZERO_FILL`,
4. verifies the output buffer,
5. checks MMIO-visible counters.

## MMIO Configuration

The runtime needs:

- OracleGPU MMIO physical base
- OracleGPU MMIO size
- descriptor physical address and mapping size
- completion-flag physical address and mapping size

The current test uses:

- MMIO base: `0xC1000000`
- MMIO size: `0x1000`

Those defaults come from the current OracleGPU board wiring in gem5. If that
changes, pass the new values through `OracleGpuRuntimeConfig`.

## Guest Requirements

The current shim depends on `/dev/mem`, so the guest kernel must allow:

- opening `/dev/mem`
- mapping the OracleGPU MMIO window
- mapping the chosen guest-physical scratch ranges

If the guest kernel restricts `/dev/mem`, the runtime will fail during
`oracle_gpu_init()` or `oracle_gpu_map_region()`.

## Installing into a Guest Rootfs

One simple path is:

1. copy this directory into the guest rootfs build context,
2. run `make DESTDIR=/path/to/rootfs install`, or install the files manually,
3. link guest applications against `liboraclegpu.a` or `liboraclegpu.so`,
4. ensure the ABI header matches the OracleGPU device model in the gem5 tree
   used for the FS run.

If you package the shared library, also install it into a loader-visible path
inside the guest image.
