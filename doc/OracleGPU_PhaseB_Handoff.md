# OracleGPU and CXL-PCM Handoff

This document records the current SimCXL state after the OracleGPU Phase B
generic-command work and the first system-level CXL-PCM memory tier work. It is
intended for a future Codex agent or engineer to continue without re-discovering
the project context.

Current status:

- OracleGPU is implemented as a generic GPU-operation proxy.
- A guest-side OracleGPU runtime shim exists and has passed FS guest tests.
- CXL-PCM exists in two selectable backend variants:
  - `CXL-PCM_from_dram`: custom DRAM-path-derived memory object.
  - `CXL-PCM_from_nvm`: gem5 NVMInterface/MemCtrl-derived backend.
- Both CXL-PCM variants have passed:
  - CPU load/store CXL-PCM test.
  - OracleGPU DMA read/write CXL-PCM test.

Deliberately not implemented:

- No attention-specific OracleGPU command.
- No real attention, matmul, QKV, sequence, or head semantics.
- No PyTorch or vLLM integration.
- No scheduler.
- No sealed KV store.
- No near-memory attention.
- No PCM cell-level physics or wear leveling.

## OracleGPU Generic Device

The old OracleGPU implementation only supported a single copy-like command.
The current implementation generalizes it into a generic operation proxy:

1. Read a command descriptor from guest memory.
2. Validate the descriptor.
3. Issue DMA reads for multiple input memory segments.
4. Wait for `compute_latency_ns`.
5. Produce output according to `result_policy`.
6. Write `completion_flag_addr = 1`.
7. Update MMIO-visible counters, stats, and debug logs.

OracleGPU does not understand any model semantics. Input buffers are used to
generate memory-system traffic. For `ZERO_FILL` and `PATTERN_FILL`, input
payload content is ignored after DMA traffic is generated.

Main files:

- `src/dev/oracle_gpu.hh`
- `src/dev/oracle_gpu.cc`
- `src/dev/oracle_gpu_protocol.h`

Important implementation detail:

- OracleGPU inherits from `DmaDevice` and uses the wrapped gem5 DMA engine
  helpers (`dmaRead` / `dmaWrite`). It is not just a bare `DmaPort` interface.

### OracleGPU Protocol

Shared descriptor header:

- `src/dev/oracle_gpu_protocol.h`

Current descriptor fields:

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

### OracleGPU Stats and MMIO Counters

Current stats in `OracleGPUStats`:

- `commandCount`
- `genericCommandCount`
- `dmaReadBytes`
- `dmaWriteBytes`
- `completedCount`
- `invalidCommandCount`

MMIO readback registers expose:

- descriptor address low/high
- status
- `commandCount`
- `completedCount`
- `dmaReadBytes`
- `dmaWriteBytes`
- `genericCommandCount`
- `invalidCommandCount`

### OracleGPU Guest Tests

Guest test sources:

- `tests/test-progs/oracle-gpu/src/oracle_gpu_fs_test.c`
- `tests/test-progs/oracle-gpu/src/oracle_gpu_generic_test.c`
- `tests/test-progs/oracle-gpu/src/oracle_gpu_cxl_pcm_test.c`
- `tests/test-progs/oracle-gpu/src/Makefile`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/oracle_gpu_runtime_test.c`

Generated binaries:

- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_fs_test`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_generic_test`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_cxl_pcm_test`
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_runtime_test`

Do not use `tests/test-progs/oracle-gpu/src/oracle_gpu_cxl_pcm_test` as a
binary path. That `src/` directory binary was a redundant implicit-make output
and has been removed. The official output is under `bin/x86/linux/`.

Build command:

```bash
make -C tests/test-progs/oracle-gpu/src
make -C tests/test-progs/oracle-gpu/oracle_gpu_user
```

## Guest-Side OracleGPU Runtime Shim

Runtime shim directory:

- `tests/test-progs/oracle-gpu/oracle_gpu_user/`

Files:

- `oracle_gpu_abi.h`
- `oracle_gpu_runtime.h`
- `oracle_gpu_runtime.c`
- `oracle_gpu_runtime_test.c`
- `Makefile`
- `README.md`

This is a guest Linux user-space runtime library. It is not a gem5 SimObject
and is not part of the host-side OracleGPU model.

ABI handling:

- The device-owned ABI remains `src/dev/oracle_gpu_protocol.h`.
- `oracle_gpu_user/oracle_gpu_abi.h` includes that header, or an installed copy
  named `oracle_gpu_protocol.h`, to avoid maintaining a second descriptor
  layout.

Current runtime API:

- `oracle_gpu_init`
- `oracle_gpu_map_region`
- `oracle_gpu_unmap_region`
- `oracle_gpu_submit_generic`
- `oracle_gpu_close`
- `oracle_gpu_last_error`

Current limitation:

- OracleGPU descriptors use guest physical addresses.
- There is no kernel driver or allocator for arbitrary framework-owned virtual
  tensors yet.
- Future framework integration needs a controlled physical allocator, kernel
  driver, or explicit virtual-to-physical strategy.

Runtime install command, assuming the guest root filesystem is mounted at
`local_mnt/`:

```bash
sudo make -C tests/test-progs/oracle-gpu/oracle_gpu_user \
  DESTDIR=/home/tyb/workspace/SimCXL/local_mnt install
```

## CXL-PCM Overview

CXL-PCM is modeled as a system-level CXL-attached memory tier in x86 FS mode.
The current implementation intentionally avoids PCM cell modeling and focuses
on system-visible timing, bandwidth, addressability, and stats.

Common CXL-PCM behavior:

- Exposes an independent CXL-attached memory range.
- Default base: `0x100000000`.
- Default size: `1GiB`.
- Linux sees this memory through E820 `range_type = 20`, resulting in NUMA
  node 1 allocation in the current guest kernel.
- The PCI CXL device BAR0 is a separate MMIO window assigned by Linux, usually
  `0x140000000-0x17fffffff` for a 1GiB CXL memory size.
- The guest tests allocate CXL memory through anonymous `mmap`, bind it to NUMA
  node 1 with `mbind`, then use `/proc/self/pagemap` to obtain guest physical
  addresses.
- The tests do not rely on `/dev/cxl_mem0` for allocation.
- CPU load/store and OracleGPU DMA both access the same CXL physical memory
  range.

Main board/config files:

- `src/python/gem5/components/boards/x86_board_oracle_gpu.py`
- `configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py`

The board owns the common CXL path:

- Optional `cxl_pcm_memory` parameter.
- `CXLMemCtrl` attached as a PCI CXL memory device.
- `CXLBridge` for the non-Ruby path.
- CXL memory range appended to E820 as `range_type = 20`.
- CXL memory controllers appended to `self.memories` so the physical memory
  system knows about them.
- OracleGPU DMA port is connected through the same IO bus path and can reach
  CXL memory.

The FS config selects the backend:

```bash
--cxl-pcm-backend from_dram
--cxl-pcm-backend from_nvm
```

Hard-coded resource paths in the current CXL-PCM FS script:

- `KERNEL_PATH = /data/tyb/gem5/vmlinux`
- `DISK_IMAGE_PATH = /data/tyb/gem5/parsec.img`

These paths are intentionally hard-coded for the current local setup. Refactor
them into arguments only if needed.

## CXL-PCM_from_dram

This backend keeps the original custom CXL-PCM implementation but renames it to
make clear that it is DRAM-path-derived.

Files:

- `src/mem/CXLPCMFromDRAMMemory.py`
- `src/mem/cxl_pcm_from_dram_memory.hh`
- `src/mem/cxl_pcm_from_dram_memory.cc`
- `src/python/gem5/components/memory/cxl_pcm_from_dram.py`

Build registration:

- `src/mem/SConscript`
  - `SimObject('CXLPCMFromDRAMMemory.py', ...)`
  - `Source('cxl_pcm_from_dram_memory.cc')`
  - `DebugFlag('CXLPCMFromDRAM')`
- `src/python/SConscript`
  - `gem5/components/memory/cxl_pcm_from_dram.py`

Class names:

- SimObject: `CXLPCMFromDRAMMemory`
- Python memory wrapper: `SingleChannelCXLPCMFromDRAM`

Modeling approach:

- Directly inherits from `AbstractMemory`.
- Owns a `ResponsePort`.
- Implements timing request handling internally.
- Supports independently configurable:
  - `read_latency`
  - `write_latency`
  - `latency_var`
  - `read_bandwidth`
  - `write_bandwidth`
- Defaults:
  - read latency: `150ns`
  - write latency: `500ns`
  - read bandwidth: `8GiB/s`
  - write bandwidth: `2GiB/s`
- Write latency is intentionally higher than read latency.

Stats:

- `board.cxl_pcm_memory.module.pcmReadBytes`
- `board.cxl_pcm_memory.module.pcmWriteBytes`
- `board.cxl_pcm_memory.module.pcmReadRequests`
- `board.cxl_pcm_memory.module.pcmWriteRequests`
- `board.cxl_pcm_memory.module.totalPcmWrites`
- AbstractMemory per-requestor stats, e.g.:
  - `board.cxl_pcm_memory.module.bytesRead::oracle_gpu`
  - `board.cxl_pcm_memory.module.bytesWritten::oracle_gpu`

This backend is the easiest one to extend if future work needs PCM-specific
custom counters with explicit names.

## CXL-PCM_from_nvm

This backend reuses gem5's native NVM media model under the same CXL-attached
memory path.

Files:

- `src/python/gem5/components/memory/cxl_pcm_from_nvm.py`

Build registration:

- `src/python/SConscript`
  - `gem5/components/memory/cxl_pcm_from_nvm.py`

Class name:

- Python memory wrapper: `SingleChannelCXLPCMFromNVM`

Modeling approach:

- Instantiates `NVM_2400_1x64`.
- Connects it to a standard `MemCtrl`.
- Uses the same board-level `CXLMemCtrl` and CXL memory range plumbing as
  `from_dram`.
- Configures:
  - `tREAD` from `--cxl-pcm-read-latency`
  - `tWRITE` from `--cxl-pcm-write-latency`
  - `tBURST` from the lower of read/write bandwidths
  - `static_frontend_latency`
  - `static_backend_latency`
- Defaults:
  - read latency: `150ns`
  - write latency: `500ns`
  - read bandwidth: `8GiB/s`
  - write bandwidth: `2GiB/s`

Important limitation:

- The NVM backend has separate read/write media latencies through
  `tREAD/tWRITE`.
- Bandwidth is currently approximated through a shared `tBURST` channel timing,
  using the lower of read/write bandwidth. It does not yet enforce separate
  read bandwidth and write bandwidth channels inside `NVMInterface`.

Stats:

- MemCtrl system-interface stats:
  - `board.cxl_pcm_memory.mem_ctrl.readReqs`
  - `board.cxl_pcm_memory.mem_ctrl.writeReqs`
  - `board.cxl_pcm_memory.mem_ctrl.bytesReadSys`
  - `board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys`
- NVM media stats:
  - `board.cxl_pcm_memory.nvm.nvmBytesRead`
  - `board.cxl_pcm_memory.nvm.nvmBytesWritten`
  - `board.cxl_pcm_memory.nvm.readBursts`
  - `board.cxl_pcm_memory.nvm.writeBursts`
- AbstractMemory per-requestor stats on the NVM object:
  - `board.cxl_pcm_memory.nvm.bytesRead::oracle_gpu`
  - `board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu`
  - `board.cxl_pcm_memory.nvm.numReads::oracle_gpu`
  - `board.cxl_pcm_memory.nvm.numWrites::oracle_gpu`

There is no `totalPcmWrites` stat with that exact name in `from_nvm`. Use one
of these depending on the question:

- `board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys` for system-side accepted
  write traffic.
- `board.cxl_pcm_memory.nvm.nvmBytesWritten` for media-side NVM write traffic.
- `board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu` for OracleGPU-written
  bytes to the NVM-backed CXL range.

## CXL-PCM Guest Tests

CPU CXL-PCM memory test:

- Source: `tests/test-progs/cxl-pcm/src/cxl_pcm_mem_test.c`
- Binary: `tests/test-progs/cxl-pcm/bin/x86/linux/cxl_pcm_mem_test`

OracleGPU CXL-PCM DMA test:

- Source: `tests/test-progs/oracle-gpu/src/oracle_gpu_cxl_pcm_test.c`
- Binary: `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_cxl_pcm_test`

Build commands:

```bash
make -C tests/test-progs/cxl-pcm/src
make -C tests/test-progs/oracle-gpu/src
```

Guest image install commands for the current local image:

```bash
sudo mount -o loop,offset=$((2048*512)) /data/tyb/gem5/parsec.img local_mnt/
sudo mkdir -p local_mnt/usr/local/bin
sudo cp tests/test-progs/cxl-pcm/bin/x86/linux/cxl_pcm_mem_test \
  local_mnt/usr/local/bin/
sudo cp tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_cxl_pcm_test \
  local_mnt/usr/local/bin/
sudo chmod +x local_mnt/usr/local/bin/cxl_pcm_mem_test
sudo chmod +x local_mnt/usr/local/bin/oracle_gpu_cxl_pcm_test
sync
sudo umount local_mnt/
```

The CXL-PCM FS script runs:

```bash
/usr/local/bin/cxl_pcm_mem_test 0x100000000 65536
/usr/local/bin/oracle_gpu_cxl_pcm_test
```

Important cache-coherence note:

- The OracleGPU CXL-PCM test uses `clflush` before DMA so OracleGPU reads fresh
  input data.
- It also flushes the destination buffer after the completion flag is observed
  and before CPU-side validation. This is required for the NVM backend and is a
  safer DMA test pattern generally.

Expected guest success strings:

- `CXL-PCM memory test passed`
- `OracleGPU CXL-PCM DMA test passed`

OracleGPU CXL-PCM DMA byte math:

- Descriptor read from DRAM scratch: `200` bytes.
- CXL input reads: `64 + 64 = 128` bytes.
- Total OracleGPU DMA read bytes: `328`.
- CXL output write: `128` bytes.
- DRAM completion flag write: `4` bytes.
- Total OracleGPU DMA write bytes: `132`.

## CXL-PCM Build and Run Commands

Because `CXLPCMMemory` was renamed to `CXLPCMFromDRAMMemory`, a full rebuild is
recommended after pulling these changes:

```bash
scons -c build/X86
scons build/X86/gem5.opt -j$(nproc)
```

Run `from_dram`:

```bash
sudo build/X86/gem5.opt -d m5out/cxl_pcm_from_dram \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_dram
```

Run `from_nvm`:

```bash
sudo build/X86/gem5.opt -d m5out/cxl_pcm_from_nvm \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_nvm
```

Useful success checks:

```bash
grep -R "CXL-PCM memory test passed\\|OracleGPU CXL-PCM DMA test passed" \
  m5out/cxl_pcm_from_dram m5out/cxl_pcm_from_nvm
```

Useful `from_dram` stats check:

```bash
grep -E "board.cxl_pcm_memory.module.pcm|board.cxl_pcm_memory.module.bytes.*oracle_gpu|board.oracle_gpu" \
  m5out/cxl_pcm_from_dram/stats.txt
```

Useful `from_nvm` stats check:

```bash
grep -E "board.cxl_pcm_memory.*(nvmBytesRead|nvmBytesWritten|readReqs|writeReqs|bytesReadSys|bytesWrittenSys|bytesRead::oracle_gpu|bytesWritten::oracle_gpu|numReads::oracle_gpu|numWrites::oracle_gpu)|board.oracle_gpu" \
  m5out/cxl_pcm_from_nvm/stats.txt
```

## Verified CXL-PCM Results

Both backends were observed passing after the guest image had the latest
`oracle_gpu_cxl_pcm_test` binary.

Common OracleGPU stats for both backends:

- `board.oracle_gpu.commandCount = 1`
- `board.oracle_gpu.genericCommandCount = 1`
- `board.oracle_gpu.dmaReadBytes = 328`
- `board.oracle_gpu.dmaWriteBytes = 132`
- `board.oracle_gpu.completedCount = 1`
- `board.oracle_gpu.invalidCommandCount = 0`

Observed `from_dram` stats:

- `board.cxl_pcm_memory.module.pcmReadBytes = 148096`
- `board.cxl_pcm_memory.module.pcmWriteBytes = 65920`
- `board.cxl_pcm_memory.module.pcmReadRequests = 2314`
- `board.cxl_pcm_memory.module.pcmWriteRequests = 1030`
- `board.cxl_pcm_memory.module.totalPcmWrites = 65920`
- `board.cxl_pcm_memory.module.bytesRead::oracle_gpu = 128`
- `board.cxl_pcm_memory.module.bytesWritten::oracle_gpu = 128`

Observed `from_nvm` stats:

- `board.cxl_pcm_memory.mem_ctrl.readReqs = 1935`
- `board.cxl_pcm_memory.mem_ctrl.writeReqs = 1030`
- `board.cxl_pcm_memory.mem_ctrl.bytesReadSys = 123840`
- `board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys = 65920`
- `board.cxl_pcm_memory.nvm.nvmBytesRead = 89472`
- `board.cxl_pcm_memory.nvm.nvmBytesWritten = 63360`
- `board.cxl_pcm_memory.nvm.bytesRead::oracle_gpu = 128`
- `board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu = 128`
- `board.cxl_pcm_memory.nvm.numReads::oracle_gpu = 2`
- `board.cxl_pcm_memory.nvm.numWrites::oracle_gpu = 2`

The difference between `bytesReadSys` and `nvmBytesRead` is expected: MemCtrl
system-side accounting and NVM media burst accounting are different views of
traffic, and reads may be served from the write queue.

## Existing OracleGPU FS Scripts

Other OracleGPU FS scripts still exist:

- `configs/example/gem5_library/x86-oracle-gpu-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-generic-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-runtime-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py`

The smoke and generic scripts inject guest test binaries at boot. The runtime
and CXL-PCM scripts expect `/usr/local/bin/...` binaries inside the guest image.

Previously observed OracleGPU-only results:

- Smoke test:
  - `OracleGPU FS smoke test passed: 'oracle-gpu-fs-smoke'`
  - `dmaReadBytes = 240`
  - `dmaWriteBytes = 24`
- Generic test:
  - `OracleGPU generic test passed with 3 inputs`
  - `dmaReadBytes = 392`
  - `dmaWriteBytes = 132`
- Runtime shim test:
  - `OracleGPU runtime test passed with 3 inputs`
  - `dmaReadBytes = 352`
  - `dmaWriteBytes = 100`

## Warnings That Were Seen and Considered Harmless

These appeared in successful runs and do not indicate current test failure:

- `PowerState: Already in the requested power state, request ignored`
- `instruction 'fwait' unimplemented`
- `User-specified generator/function list for the exit event 'exit' has ended`
- `hack: Pretending totalOps is equivalent to totalInsts()`
- CXL PCI message: `can't find IRQ for PCI INT A; probably buggy MP table`

## Recommended Next Steps

Near-term engineering cleanup:

1. Decide whether to commit binary test outputs or keep only sources plus build
   instructions. The current tree includes generated guest binaries under
   `tests/test-progs/.../bin/x86/linux/`.
2. Add a dedicated timing experiment that changes CXL-PCM latency/bandwidth
   parameters and compares simulated time.
3. Add a dedicated `ZERO_FILL` OracleGPU test.
4. Add a dynamic-link test for `liboraclegpu.so` if shared-library deployment
   matters.
5. Consider making kernel/disk image paths configurable if this will run on
   machines other than the current `/data/tyb/gem5/...` setup.

Potential future CXL-PCM work:

1. If strict separate read bandwidth and write bandwidth are required for
   `from_nvm`, extend `NVMInterface`/`MemCtrl` rather than only setting shared
   `tBURST`.
2. If endurance analysis should use one uniform stat name across both backends,
   add explicit PCM alias stats to `from_nvm`.
3. Add regression tests that grep for both CPU and OracleGPU CXL stats.
4. Keep `range_type = 20`; do not revert to reserved `range_type = 2`. The
   current tests depend on Linux exposing the CXL memory as a NUMA memory node.

Out of scope until explicitly requested:

- LLM-like KV offload baseline.
- Inference framework integration.
- Attention-specific OracleGPU commands.
- Near-memory attention.
- PCM cell physics or wear leveling.
