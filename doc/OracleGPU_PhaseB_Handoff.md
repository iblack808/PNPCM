# OracleGPU and CXL-PCM Handoff

Last updated: 2026-05-24

This document records the current SimCXL state after the OracleGPU Phase B
generic-command work and the first system-level CXL-PCM memory tier work. It is
intended for a future Codex agent or engineer to continue without re-discovering
the project context.

Current status:

- OracleGPU is implemented as a generic GPU-operation proxy.
- A guest-side OracleGPU runtime shim exists and has passed FS guest tests.
- CXL-PCM exists in two selectable backend variants:
  - `CXL-PCM_from_dram`: custom DRAM-path-derived memory object.
  - `CXL-PCM_from_nvm`: CXL-PCM-specific NVMInterface/MemCtrl-derived backend.
- Both CXL-PCM variants have passed:
  - CPU load/store CXL-PCM test.
  - OracleGPU DMA read/write CXL-PCM test.
- Both CXL-PCM variants support independently configurable read/write latency
  and independently configurable read/write bandwidth.
- Both CXL-PCM variants now model a 128B internal PCM media read/write
  granularity.
- Native gem5 `DRAMInterface` and `NVMInterface` are intentionally restored and
  left unchanged. CXL-PCM-specific behavior lives in CXL-PCM-specific files.

Current accepted baseline:

- x86 FS boots with DDR plus a separate 1GiB CXL-PCM range at `0x100000000`.
- Linux exposes the CXL range as NUMA node 1 through E820 `range_type = 20`.
- CPU guest test accesses the CXL-PCM range and updates CXL-PCM stats.
- OracleGPU DMA reads and writes the same CXL-PCM range and updates both
  OracleGPU DMA stats and CXL-PCM per-requestor stats.
- `from_dram` and `from_nvm` are selected only by
  `--cxl-pcm-backend from_dram|from_nvm`.

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

Current CXL-PCM-specific source files:

- `src/mem/CXLPCMFromDRAMMemory.py`
- `src/mem/cxl_pcm_from_dram_memory.hh`
- `src/mem/cxl_pcm_from_dram_memory.cc`
- `src/mem/CXLPCMFromNVMInterface.py`
- `src/mem/cxl_pcm_from_nvm_interface.hh`
- `src/mem/cxl_pcm_from_nvm_interface.cc`
- `src/python/gem5/components/memory/cxl_pcm_from_dram.py`
- `src/python/gem5/components/memory/cxl_pcm_from_nvm.py`

Files that should remain unmodified for this CXL-PCM modeling work:

- `src/mem/DRAMInterface.py`
- `src/mem/dram_interface.hh`
- `src/mem/dram_interface.cc`
- `src/mem/NVMInterface.py`
- `src/mem/nvm_interface.hh`
- `src/mem/nvm_interface.cc`

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
- `CXL_PCM_SIZE = "1GiB"`
- `CXL_PCM_BASE = 0x100000000`
- `CXL_PCM_READ_LATENCY = "150ns"`
- `CXL_PCM_WRITE_LATENCY = "500ns"`
- `CXL_PCM_READ_BANDWIDTH = "8GiB/s"`
- `CXL_PCM_WRITE_BANDWIDTH = "2GiB/s"`
- `CXL_PCM_TEST_BYTES = 64 * 1024`
- `CXL-PCM internal media granularity = 128B`

These values are intentionally hard-coded for the current local setup and test
baseline. Refactor them into arguments only if needed.

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
  - `media_granularity`
- Defaults:
  - read latency: `150ns`
  - write latency: `500ns`
  - read bandwidth: `8GiB/s`
  - write bandwidth: `2GiB/s`
  - media granularity: `128B`
- Write latency is intentionally higher than read latency.
- CPU/DMA requests may still arrive as 64B cache-line-sized packets, but the
  CXL-PCM media bandwidth occupancy and explicit PCM byte/write stats are
  rounded up per request to the 128B media granule.

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
memory path, but through a CXL-PCM-specific copy of the NVM interface. The
native gem5 `NVMInterface` and `DRAMInterface` files are intentionally left
unchanged.

Files:

- `src/mem/CXLPCMFromNVMInterface.py`
- `src/mem/cxl_pcm_from_nvm_interface.hh`
- `src/mem/cxl_pcm_from_nvm_interface.cc`
- `src/python/gem5/components/memory/cxl_pcm_from_nvm.py`

Build registration:

- `src/mem/SConscript`
  - `SimObject('CXLPCMFromNVMInterface.py', ...)`
  - `Source('cxl_pcm_from_nvm_interface.cc')`
- `src/python/SConscript`
  - `gem5/components/memory/cxl_pcm_from_nvm.py`

Class name:

- SimObject: `CXLPCMFromNVMInterface`
- media preset: `CXLPCMFromNVM_2400_1x64`
- Python memory wrapper: `SingleChannelCXLPCMFromNVM`

Modeling approach:

- Instantiates `CXLPCMFromNVM_2400_1x64`.
- Connects it to a standard `MemCtrl`.
- Uses the same board-level `CXLMemCtrl` and CXL memory range plumbing as
  `from_dram`.
- Configures:
  - `tREAD` from `CXL_PCM_READ_LATENCY`
  - `tWRITE` from `CXL_PCM_WRITE_LATENCY`
  - `tREAD_BURST` from the configured read bandwidth
  - `tWRITE_BURST` from the configured write bandwidth
  - `tBURST` from the faster of the two bandwidths for legacy/default
    bookkeeping
  - `static_frontend_latency`
  - `static_backend_latency`
- Defaults:
  - read latency: `150ns`
  - write latency: `500ns`
  - read bandwidth: `8GiB/s`
  - write bandwidth: `2GiB/s`
  - media granularity: `128B`, implemented as `device_bus_width = 64` and
    `burst_length = 16`

Important bandwidth implementation note:

- The NVM backend now has separate read/write media latencies through
  `tREAD/tWRITE`.
- It also has separate data burst durations through `tREAD_BURST` and
  `tWRITE_BURST`.
- `CXLPCMFromNVMInterface::doBurstAccess()` selects the burst duration by
  request type, so CXL-PCM_from_nvm read bandwidth and write bandwidth are
  independently configurable.
- `CXLPCMFromNVMInterface::commandOffset()` returns the smaller of the two
  burst durations so the controller scheduler wakes early enough for the
  faster direction.
- The native gem5 `NVMInterface` does not have these extra burst parameters.
  Do not add them back to native NVM unless the project explicitly decides to
  change global gem5 behavior.

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

Rebuild gem5 after changing any CXL-PCM SimObject or C++ implementation.
Incremental build is normally sufficient:

```bash
scons build/X86/gem5.opt -j$(nproc)
```

Use `scons -c build/X86` only if stale generated SimObject code or build-cache
state is suspected.

Run `from_dram`:

```bash
sudo build/X86/gem5.opt -d m5out/cxl_pcm_from_dram_128b \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_dram
```

Run `from_nvm`:

```bash
sudo build/X86/gem5.opt -d m5out/cxl_pcm_from_nvm_128b \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_nvm
```

Useful success checks:

```bash
grep -R "CXL-PCM memory test passed\\|OracleGPU CXL-PCM DMA test passed" \
  m5out/cxl_pcm_from_dram_128b m5out/cxl_pcm_from_nvm_128b
```

Useful `from_dram` stats check:

```bash
grep -E "board.cxl_pcm_memory.module.pcm|board.cxl_pcm_memory.module.bytes.*oracle_gpu|board.oracle_gpu" \
  m5out/cxl_pcm_from_dram_128b/stats.txt
```

Useful `from_nvm` stats check:

```bash
grep -E "board.cxl_pcm_memory.*(nvmBytesRead|nvmBytesWritten|readReqs|writeReqs|bytesReadSys|bytesWrittenSys|bytesRead::oracle_gpu|bytesWritten::oracle_gpu|numReads::oracle_gpu|numWrites::oracle_gpu)|board.oracle_gpu" \
  m5out/cxl_pcm_from_nvm_128b/stats.txt
```

Useful config checks:

```bash
grep -n -A30 "\[board.cxl_pcm_memory.module\]" \
  m5out/cxl_pcm_from_dram_128b/config.ini
grep -n -A40 "\[board.cxl_pcm_memory.nvm\]" \
  m5out/cxl_pcm_from_nvm_128b/config.ini
```

Expected backend types:

- `from_dram`: `type=CXLPCMFromDRAMMemory`
- `from_nvm`: `type=CXLPCMFromNVMInterface`

Expected default asymmetric bandwidth/timing facts:

- `from_dram`: `read_latency=150000`, `write_latency=500000`
- `from_dram`: `read_bandwidth=116`, `write_bandwidth=466`
- `from_dram`: `media_granularity=128`
- `from_nvm`: `tREAD=150000`, `tWRITE=500000`
- `from_nvm`: `device_bus_width=64`, `burst_length=16`
- `from_nvm`: `tREAD_BURST` should be about `14901` ticks
- `from_nvm`: `tWRITE_BURST` should be about `59605` ticks

## CXL-PCM Verification Status

Both backends were observed passing after the 128B media granularity update.
The guest image had the latest `oracle_gpu_cxl_pcm_test` binary.

Latest checked output directories:

- `m5out/cxl_pcm_from_dram_128b`
- `m5out/cxl_pcm_from_nvm_128b`

Latest checked config facts:

- `from_dram`: `board.cxl_pcm_memory.module.type = CXLPCMFromDRAMMemory`
- `from_dram`: CXL-PCM range `0x100000000-0x140000000`
- `from_dram`: `read_latency = 150000`, `write_latency = 500000`
- `from_dram`: `read_bandwidth = 116`, `write_bandwidth = 466`
- `from_dram`: `media_granularity = 128`
- `from_nvm`: `board.cxl_pcm_memory.nvm.type = CXLPCMFromNVMInterface`
- `from_nvm`: CXL-PCM range `0x100000000-0x140000000`
- `from_nvm`: `device_bus_width = 64`, `burst_length = 16`
- `from_nvm`: `tREAD = 150000`, `tWRITE = 500000`
- `from_nvm`: `tREAD_BURST = 14901`, `tWRITE_BURST = 59605`

Common OracleGPU stats for both backends:

- `board.oracle_gpu.commandCount = 1`
- `board.oracle_gpu.genericCommandCount = 1`
- `board.oracle_gpu.dmaReadBytes = 328`
- `board.oracle_gpu.dmaWriteBytes = 132`
- `board.oracle_gpu.completedCount = 1`
- `board.oracle_gpu.invalidCommandCount = 0`

Latest observed `from_dram` stats:

- `board.cxl_pcm_memory.module.pcmReadBytes = 301056`
- `board.cxl_pcm_memory.module.pcmWriteBytes = 131840`
- `board.cxl_pcm_memory.module.pcmReadRequests = 2352`
- `board.cxl_pcm_memory.module.pcmWriteRequests = 1030`
- `board.cxl_pcm_memory.module.totalPcmWrites = 131840`
- `board.cxl_pcm_memory.module.bytesRead::oracle_gpu = 128`
- `board.cxl_pcm_memory.module.bytesWritten::oracle_gpu = 128`

The explicit `from_dram` PCM byte stats are media-granule bytes. In the latest
run, writes were exactly `131840 / 1030 = 128B` per write request. Reads were
`301056 / 2352 = 128B` per read request.

Latest observed `from_nvm` stats:

- `board.cxl_pcm_memory.mem_ctrl.readReqs = 2350`
- `board.cxl_pcm_memory.mem_ctrl.writeReqs = 1030`
- `board.cxl_pcm_memory.mem_ctrl.readBursts = 2350`
- `board.cxl_pcm_memory.mem_ctrl.writeBursts = 1030`
- `board.cxl_pcm_memory.mem_ctrl.mergedWrBursts = 516`
- `board.cxl_pcm_memory.mem_ctrl.bytesReadSys = 150400`
- `board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys = 65920`
- `board.cxl_pcm_memory.nvm.readBursts = 2074`
- `board.cxl_pcm_memory.nvm.writeBursts = 466`
- `board.cxl_pcm_memory.nvm.nvmBytesRead = 265472`
- `board.cxl_pcm_memory.nvm.nvmBytesWritten = 59648`
- `board.cxl_pcm_memory.nvm.bytesRead::oracle_gpu = 128`
- `board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu = 128`
- `board.cxl_pcm_memory.nvm.numReads::oracle_gpu = 2`
- `board.cxl_pcm_memory.nvm.numWrites::oracle_gpu = 2`

The latest `from_nvm` media burst stats confirm the 128B granularity:

- `nvmBytesRead / readBursts = 265472 / 2074 = 128B`
- `nvmBytesWritten / writeBursts = 59648 / 466 = 128B`

Important `from_nvm` accounting caveat:

- `mem_ctrl.bytesReadSys` and `mem_ctrl.bytesWrittenSys` are system request
  bytes, still 64B per guest cache-line request in the current tests.
- `mem_ctrl.readBursts` and `mem_ctrl.writeBursts` count controller-side
  128B-granule burst demand before some write merging.
- `nvm.nvmBytesRead` and `nvm.nvmBytesWritten` count bursts actually issued to
  the NVM interface.
- Native `MemCtrl` can merge writes in the write queue and can satisfy reads
  from queued writes, so `nvm.writeBursts` can be lower than
  `mem_ctrl.writeBursts`. In the latest run, 516 controller write bursts were
  merged before reaching the NVM interface.

Acceptance checklist status:

- CXL-attached memory range: done.
- Configurable capacity: done through `CXL_PCM_SIZE`.
- Configurable read/write latency: done for both backends.
- Configurable read/write bandwidth: done for both backends.
- Read/write asymmetry with slower default writes: done.
- Internal 128B PCM media granularity: implemented and FS-validated.
- PCM read/write byte stats: done.
  - `from_dram` has explicit PCM stats.
  - `from_nvm` uses MemCtrl and NVM media stats.
- PCM read/write request stats: done.
- Total PCM writes for endurance analysis: done explicitly for `from_dram`;
  use `nvmBytesWritten` or `bytesWrittenSys` for `from_nvm`.
- CPU load/store access: tested and passing.
- OracleGPU DMA read/write access: tested and passing.
- DDR and CXL-PCM stats distinguishable: done by separate stat paths.
- Existing OracleGPU smoke/generic/runtime paths: not intentionally changed by
  the CXL-PCM backend split.

Timing sensitivity status:

- The successful baseline runs used asymmetric defaults
  (`8GiB/s` read, `2GiB/s` write; `150ns` read, `500ns` write).
- Config inspection confirms the asymmetric values reach both backends.
- A dedicated parameter sweep that compares simulated time under different
  latency/bandwidth constants has not yet been added. This is the next step if
  quantitative timing sensitivity needs to be shown in a reproducible script.

## Existing FS Scripts

Other OracleGPU FS scripts still exist and were not intentionally changed by
the CXL-PCM backend work:

- `configs/example/gem5_library/x86-oracle-gpu-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-generic-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-runtime-fs.py`
- `configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py`

The smoke and generic scripts inject guest test binaries at boot. The runtime
and CXL-PCM scripts expect `/usr/local/bin/...` binaries inside the guest image.

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

1. If endurance analysis should use one uniform stat name across both backends,
   add explicit PCM alias stats to `from_nvm`.
2. Add regression tests that grep for both CPU and OracleGPU CXL stats.
3. Add a dedicated timing experiment that separately sweeps
   `CXL_PCM_READ_BANDWIDTH` and `CXL_PCM_WRITE_BANDWIDTH` for `from_nvm`.
4. Keep `range_type = 20`; do not revert to reserved `range_type = 2`. The
   current tests depend on Linux exposing the CXL memory as a NUMA memory node.

Out of scope until explicitly requested:

- LLM-like KV offload baseline.
- Inference framework integration.
- Attention-specific OracleGPU commands.
- Near-memory attention.
- PCM cell physics or wear leveling.
