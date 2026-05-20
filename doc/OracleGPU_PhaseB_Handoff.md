# OracleGPU Phase B Handoff

This document summarizes the current OracleGPU state after the Phase B
"generic GPU operation proxy" refactor. It is intended for a future Codex
agent or engineer to continue implementation without re-discovering context.

## Scope completed in this phase

The old OracleGPU implementation only supported one `src_addr -> dst_addr`
copy-like command. The current implementation generalizes OracleGPU into a
generic GPU operation proxy:

1. Read a command descriptor from guest memory.
2. Validate the descriptor.
3. Issue DMA reads for multiple input memory segments.
4. Wait for `compute_latency_ns`.
5. Produce output according to `result_policy`.
6. Write `completion_flag_addr = 1`.
7. Update MMIO-visible counters, stats, and debug logs.

OracleGPU does **not** understand any attention/matmul/QKV/seq/head semantics.
The descriptor carries only generic memory segments plus output policy.

## Key design constraints already enforced

- No attention-specific command exists.
- No real attention or matmul implementation exists.
- No PyTorch or vLLM integration exists.
- No scheduler exists.
- No near-memory / CXL-PCM attention logic exists.

## Protocol and descriptor

Shared protocol header:

- `src/dev/oracle_gpu_protocol.h`

Current descriptor:

- `magic`
- `version`
- `op_type`
- `num_inputs`
- `result_policy`
- `dst_addr`
- `dst_bytes`
- `oracle_result_addr`
- `oracle_result_bytes`
- `compute_latency_ns`
- `completion_flag_addr`
- `user_tag`
- `inputs[ORACLE_GPU_MAX_INPUTS]`, each with:
  - `addr`
  - `bytes`

Current protocol constants:

- `ORACLE_GPU_CMD_MAGIC`
- `ORACLE_GPU_CMD_VERSION`
- `ORACLE_GPU_MAX_INPUTS = 8`
- `ORACLE_GPU_OP_GENERIC = 1`
- `ORACLE_GPU_RESULT_ZERO_FILL = 1`
- `ORACLE_GPU_RESULT_PATTERN_FILL = 2`
- `ORACLE_GPU_RESULT_COPY_ORACLE = 3`
- `ORACLE_GPU_PATTERN_BYTE = 0xa5`

The protocol header also defines MMIO register offsets and status bits so guest
tests and device code use the same constants.

## Device implementation

Main files:

- `src/dev/oracle_gpu.hh`
- `src/dev/oracle_gpu.cc`

Execution flow in the current code:

1. CPU writes descriptor address to existing MMIO registers.
2. CPU rings the existing doorbell register.
3. OracleGPU DMA-reads the descriptor from guest memory.
4. OracleGPU validates:
   - `magic`
   - `version`
   - `op_type`
   - `num_inputs`
   - `dst_addr`
   - `dst_bytes`
   - `completion_flag_addr`
   - per-input `addr` and `bytes`
   - `result_policy`
   - `oracle_result_*` if `COPY_ORACLE`
5. OracleGPU DMA-reads each input segment sequentially.
6. After all reads complete, OracleGPU schedules a compute delay:
   - `delay = sim_clock::as_int::ns * activeCmd.compute_latency_ns`
7. OracleGPU writes output according to `result_policy`:
   - `ZERO_FILL`: fill output with zeroes
   - `PATTERN_FILL`: fill output with `0xa5`
   - `COPY_ORACLE`: DMA-read `oracle_result_addr` and DMA-write it to `dst`
8. OracleGPU DMA-writes `completion_flag_addr = 1`.
9. OracleGPU updates status and stats.

Important implementation note:

- Input segments are used only to generate memory-system traffic.
- Input payload content is not consumed semantically.
- For `ZERO_FILL` and `PATTERN_FILL`, the read data is ignored after traffic is
  generated.

## Stats and MMIO-visible counters

Current stats in `OracleGPUStats`:

- `commandCount`
- `genericCommandCount`
- `dmaReadBytes`
- `dmaWriteBytes`
- `completedCount`
- `invalidCommandCount`

MMIO readback registers currently expose:

- descriptor address low/high
- status
- `commandCount`
- `completedCount`
- `dmaReadBytes`
- `dmaWriteBytes`
- `genericCommandCount`
- `invalidCommandCount`

## Guest tests

Guest test sources:

- `tests/test-progs/oracle-gpu/src/oracle_gpu_fs_test.c`
- `tests/test-progs/oracle-gpu/src/oracle_gpu_generic_test.c`
- `tests/test-progs/oracle-gpu/src/Makefile`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/oracle_gpu_runtime_test.c`

Generated binaries:

- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_fs_test`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_generic_test`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_runtime_test`

### Smoke test

Purpose:

- Preserve the original end-to-end OracleGPU smoke path.
- Validate that the refactor did not break command fetch, DMA, completion, or
  guest execution.

Current behavior:

- Uses the new generic descriptor.
- Uses `num_inputs = 1`.
- Uses `COPY_ORACLE`.
- `compute_latency_ns = 500`.

Expected success string:

- `OracleGPU FS smoke test passed: 'oracle-gpu-fs-smoke'`

### Generic test

Purpose:

- Validate the new generic multi-input descriptor and MMIO stats.

Current behavior:

- Uses `num_inputs = 3`.
- Uses 3 independent input buffers.
- Uses `PATTERN_FILL`.
- `compute_latency_ns = 2000`.
- Verifies output buffer contents.
- Verifies completion flag.
- Verifies MMIO counters.

Expected guest output:

- `expected read bytes: 392`
- `expected write bytes: 132`
- `command count: 1`
- `generic command count: 1`
- `dma read bytes: 392`
- `dma write bytes: 132`
- `completed count: 1`
- `invalid command count: 0`
- `OracleGPU generic test passed with 3 inputs`

Read/write byte math for the generic test:

- Descriptor read: 200 bytes
- Input reads: 64 + 96 + 32 = 192 bytes
- Total DMA read bytes: 392 bytes
- Output write: 128 bytes
- Completion flag write: 4 bytes
- Total DMA write bytes: 132 bytes

### Guest-side runtime shim test

Purpose:

- Validate the new guest-side OracleGPU runtime shim.
- Ensure user programs can submit generic OracleGPU commands through a C API
  instead of hand-writing the command descriptor or MMIO doorbell sequence.
- Validate the rootfs-installed binary path, not only host-side binary
  injection.

Current behavior:

- Uses `num_inputs = 3`.
- Uses 3 independent input buffers.
- Uses `PATTERN_FILL`.
- `compute_latency_ns = 1500`.
- Maps scratch buffers from the reserved guest physical range
  `0x30000000-0x30ffffff`.
- Verifies output buffer contents.
- Verifies completion flag and MMIO counters.
- Runs from `/usr/local/bin/oracle_gpu_runtime_test` inside the guest image.

Expected guest output:

- `expected read bytes: 352`
- `expected write bytes: 100`
- `command count: 1`
- `generic command count: 1`
- `dma read bytes: 352`
- `dma write bytes: 100`
- `completed count: 1`
- `invalid command count: 0`
- `OracleGPU runtime test passed with 3 inputs`

Read/write byte math for the runtime test:

- Descriptor read: 200 bytes
- Input reads: 48 + 80 + 24 = 152 bytes
- Total DMA read bytes: 352 bytes
- Output write: 96 bytes
- Completion flag write: 4 bytes
- Total DMA write bytes: 100 bytes

## Guest-side runtime shim

Runtime shim directory:

- `tests/test-progs/oracle-gpu/oracle_gpu_user/`

Files:

- `oracle_gpu_abi.h`
- `oracle_gpu_runtime.h`
- `oracle_gpu_runtime.c`
- `oracle_gpu_runtime_test.c`
- `Makefile`
- `README.md`

Important positioning:

- This is a guest Linux user-space runtime library.
- It is not a new gem5 SimObject.
- It is not part of the host-side OracleGPU device model.
- It is intended to be copied or installed into an FS-mode guest disk image or
  rootfs.
- It contains no attention/matmul/QKV/seq/head semantics.

ABI handling:

- The device-owned ABI remains `src/dev/oracle_gpu_protocol.h`.
- `oracle_gpu_user/oracle_gpu_abi.h` includes that header, or an installed copy
  named `oracle_gpu_protocol.h`, to avoid maintaining a second command layout.
- Any future ABI change must keep the device and guest runtime headers in sync.

Public runtime API:

- `oracle_gpu_init(struct OracleGpuRuntime *rt,
  const struct OracleGpuRuntimeConfig *config)`
- `oracle_gpu_map_region(struct OracleGpuRuntime *rt, uint64_t phys_addr,
  size_t bytes, struct OracleGpuMappedRegion *region)`
- `oracle_gpu_unmap_region(struct OracleGpuMappedRegion *region)`
- `oracle_gpu_submit_generic(struct OracleGpuRuntime *rt,
  const struct OracleGpuInput *inputs, uint32_t num_inputs,
  uint64_t dst_phys_addr, uint64_t dst_bytes, uint32_t result_policy,
  uint64_t oracle_result_phys_addr, uint64_t oracle_result_bytes,
  uint64_t compute_latency_ns, uint64_t user_tag,
  struct OracleGpuSubmitResult *result)`
- `oracle_gpu_close(struct OracleGpuRuntime *rt)`
- `oracle_gpu_last_error(const struct OracleGpuRuntime *rt)`

Runtime implementation summary:

1. Opens `/dev/mem`.
2. Maps the OracleGPU MMIO range.
3. Maps a guest-physical descriptor region.
4. Maps a guest-physical completion flag region.
5. Builds an `OracleGPUCommand` descriptor internally.
6. Writes descriptor address low/high MMIO registers.
7. Rings the doorbell.
8. Polls the completion flag until it becomes `1`, or until timeout/error.
9. Returns MMIO-visible status and counters in `OracleGpuSubmitResult`.

Current memory model:

- Runtime inputs and outputs are guest physical addresses.
- The helper `oracle_gpu_map_region()` maps fixed guest physical scratch
  regions through `/dev/mem`.
- This is sufficient for the current FS tests.
- It is not yet a general virtual-address buffer API for `malloc()` or
  framework-owned tensors.

Important current limitation:

- OracleGPU command descriptors consume guest physical addresses.
- There is no kernel driver or allocator layer yet to translate arbitrary
  user-space virtual addresses to guest physical addresses.
- Future inference-framework integration will need either a kernel driver,
  a controlled physical buffer allocator, or another explicit address
  translation/allocation strategy.

Runtime build outputs:

- `tests/test-progs/oracle-gpu/lib/x86/linux/liboraclegpu.a`
- `tests/test-progs/oracle-gpu/lib/x86/linux/liboraclegpu.so`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_runtime_test`

The runtime test binary is linked statically by default so it can run in guest
images with older glibc versions. The shared library is still built for future
guest programs that want dynamic linking.

Runtime build command:

```bash
make -C tests/test-progs/oracle-gpu/oracle_gpu_user
```

Runtime rootfs install command, assuming the guest image root filesystem is
mounted at `local_mnt/`:

```bash
sudo make -C tests/test-progs/oracle-gpu/oracle_gpu_user \
  DESTDIR=/home/tyb/workspace/SimCXL/local_mnt install
```

Installed guest paths:

- `/usr/local/include/oraclegpu/oracle_gpu_abi.h`
- `/usr/local/include/oraclegpu/oracle_gpu_runtime.h`
- `/usr/local/lib/liboraclegpu.a`
- `/usr/local/lib/liboraclegpu.so`
- `/usr/local/bin/oracle_gpu_runtime_test`

## FS run scripts

Run scripts:

- `configs/example/gem5_library/x86-oracle-gpu-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-generic-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-runtime-fs.py`

The smoke and generic scripts currently:

- Require X86
- Boot with `CPUTypes.KVM`
- Switch to `CPUTypes.TIMING`
- Inject the guest test binary into the disk image at boot time

The runtime script currently:

- Requires X86
- Boots with `CPUTypes.KVM`
- Switches to `CPUTypes.TIMING`
- Runs `/usr/local/bin/oracle_gpu_runtime_test` from the guest image
- Does not inject a host-side runtime test binary

Current script resource paths are hard-coded:

- `KERNEL_PATH = /data/tyb/gem5/vmlinux`
- `DISK_IMAGE_PATH = /data/tyb/gem5/parsec.img`

If these paths do not exist on another machine, the next agent should either:

- edit the script paths, or
- refactor them into arguments or environment variables

## Verified results from the current phase

These runs were observed as passing:

- `m5out-oracle-smoke`
- `m5out-oracle-generic`
- `m5out` from `x86-oracle-gpu-runtime-fs.py`

Observed smoke result:

- guest serial output contains:
  - `OracleGPU FS smoke test passed: 'oracle-gpu-fs-smoke'`

Observed generic result:

- guest serial output contains the expected stats values listed above
- `m5out-oracle-generic/stats.txt` shows:
  - `board.oracle_gpu.commandCount = 1`
  - `board.oracle_gpu.genericCommandCount = 1`
  - `board.oracle_gpu.dmaReadBytes = 392`
  - `board.oracle_gpu.dmaWriteBytes = 132`
  - `board.oracle_gpu.completedCount = 1`
  - `board.oracle_gpu.invalidCommandCount = 0`

Observed runtime shim result:

- guest serial output contains:
  - `expected read bytes: 352`
  - `expected write bytes: 100`
  - `command count: 1`
  - `generic command count: 1`
  - `dma read bytes: 352`
  - `dma write bytes: 100`
  - `completed count: 1`
  - `invalid command count: 0`
  - `OracleGPU runtime test passed with 3 inputs`
- `m5out/stats.txt` shows:
  - `board.oracle_gpu.commandCount = 1`
  - `board.oracle_gpu.genericCommandCount = 1`
  - `board.oracle_gpu.dmaReadBytes = 352`
  - `board.oracle_gpu.dmaWriteBytes = 100`
  - `board.oracle_gpu.completedCount = 1`
  - `board.oracle_gpu.invalidCommandCount = 0`

Observed smoke stats:

- `board.oracle_gpu.commandCount = 1`
- `board.oracle_gpu.genericCommandCount = 1`
- `board.oracle_gpu.dmaReadBytes = 240`
- `board.oracle_gpu.dmaWriteBytes = 24`
- `board.oracle_gpu.completedCount = 1`
- `board.oracle_gpu.invalidCommandCount = 0`

Smoke read/write byte math:

- Descriptor read: 200 bytes
- Input read: 20 bytes
- Oracle result read: 20 bytes
- Total read: 240 bytes
- Output write: 20 bytes
- Completion flag write: 4 bytes
- Total write: 24 bytes

## Build and test notes

Guest tests compile with:

- `make -C tests/test-progs/oracle-gpu/src`
- `make -C tests/test-progs/oracle-gpu/oracle_gpu_user`

Example run commands:

```bash
build/X86/gem5.opt --outdir m5out-oracle-smoke \
  configs/example/gem5_library/x86-oracle-gpu-fs.py

build/X86/gem5.opt --outdir m5out-oracle-generic \
  configs/example/gem5_library/x86-oracle-gpu-generic-fs.py

build/X86/gem5.opt --outdir m5out-oracle-runtime \
  configs/example/gem5_library/x86-oracle-gpu-runtime-fs.py
```

Useful debug run:

```bash
build/X86/gem5.opt \
  --debug-flags=OracleGPU \
  --debug-file=oracle_gpu.trace \
  --outdir m5out-oracle-generic-debug \
  configs/example/gem5_library/x86-oracle-gpu-generic-fs.py
```

## What is implemented vs what is only partially verified

Implemented:

- Multi-input generic command descriptor
- Generic DMA-read traffic generation
- Compute delay scheduling
- `ZERO_FILL`
- `PATTERN_FILL`
- `COPY_ORACLE`
- Completion flag writeback
- New stats and MMIO counters
- Preserved smoke path
- New generic test path
- Guest-side OracleGPU runtime shim
- Runtime static library and shared library build
- Rootfs install target for runtime headers, libraries, and test binary
- Runtime FS test path using guest-installed `/usr/local/bin/oracle_gpu_runtime_test`

Not yet explicitly verified with a dedicated experiment:

- A standalone `ZERO_FILL` guest test case
- A dedicated before/after timing experiment proving that different
  `compute_latency_ns` values measurably change simulated time
- Dynamic loading of `liboraclegpu.so` by a non-static guest test program

Note:

- `compute_latency_ns` is already implemented correctly in the device path.
- It is just not separately quantified by a dedicated acceptance test yet.
- The current runtime test binary is static, so it validates the runtime API and
  guest-installed binary path, but not the guest dynamic loader path for
  `liboraclegpu.so`.

## Recommended next steps for the next Codex agent

1. Add a standalone `ZERO_FILL` runtime or guest test.
2. Add a dedicated `compute_latency_ns` timing validation test.
3. Add a dynamic-link test for `liboraclegpu.so` if shared-library deployment
   matters for the next integration step.
4. Consider making kernel/disk image paths configurable.
5. Consider making runtime scratch addresses configurable instead of fixed in
   `oracle_gpu_runtime_test.c`.
6. Plan a buffer-management story for future framework integration:
   - kernel driver
   - controlled guest-physical allocator
   - or another explicit virtual-to-physical address path
7. If needed, add a non-KVM variant of the FS run scripts for environments
   without `/dev/kvm`.
8. If desired, expose more per-command observability:
   - current `compute_latency_ns`
   - current `user_tag`
   - maybe a command error code register instead of only incrementing
     `invalidCommandCount`

## Short status summary

Phase B is functionally in place and passing the main end-to-end checks.
OracleGPU now behaves as a generic GPU-operation proxy with multi-input DMA
traffic generation, compute delay, configurable output policy, completion
writeback, expanded stats, and a guest-side C runtime shim. The main remaining
work is stronger test coverage, dynamic-library validation if needed, and a
real buffer-management strategy for future inference-framework integration.
