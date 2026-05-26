# OracleGPU KV Offload Baseline Runbook

Last updated: 2026-05-26

This runbook records the current validated path for the first system baseline:
an FS-mode, guest-side LLM-like KV offload driver using OracleGPU generic
commands and CXL-PCM-backed K/V cache buffers.

## Scope

Current primary path:

- Backend: `--cxl-pcm-backend from_nvm`
- CPU mode: KVM, using `--no-cpu-switch`
- CXL-PCM range: `0x100000000` through `0x13fffffff`
- K/V cache: physical ranges inside CXL-PCM
- Q/output: DDR scratch physical ranges
- OracleGPU command: generic command only

The driver does not implement attention, matmul, Q/K/V semantics inside
OracleGPU, PyTorch/vLLM integration, a scheduler, a sealed KV store, or a
near-memory attention engine.

The `from_dram` backend is not the near-term experiment path and has not been
kept in lockstep with the KVM/atomic accounting fixes described here.

## Files

Guest driver and runtime:

- `tests/test-progs/oracle-gpu/oracle_gpu_user/oracle_gpu_runtime.h`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/oracle_gpu_runtime.c`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/oracle_gpu_kv_offload_baseline.c`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/Makefile`
- `tests/test-progs/oracle-gpu/oracle_gpu_user/README.md`

Host-side OracleGPU and CXL-PCM changes relevant to this baseline:

- `src/dev/oracle_gpu.hh`
- `src/dev/oracle_gpu.cc`
- `src/mem/mem_ctrl.cc`
- `src/mem/mem_interface.hh`
- `src/mem/cxl_pcm_from_nvm_interface.hh`
- `src/mem/cxl_pcm_from_nvm_interface.cc`
- `src/python/gem5/components/memory/cxl_pcm_from_nvm.py`
- `configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py`

## Build And Install

Rebuild gem5 after changing C++ code:

```bash
scons build/X86/gem5.opt -j$(nproc)
```

Build the guest runtime and KV baseline driver:

```bash
make -C tests/test-progs/oracle-gpu/oracle_gpu_user
```

Install into a mounted guest rootfs, if the binary changed:

```bash
sudo make -C tests/test-progs/oracle-gpu/oracle_gpu_user \
  DESTDIR=/home/tyb/workspace/SimCXL/local_mnt install
sync
```

Pure gem5 C++ or config-script changes do not require copying the guest binary
again.

## Run Commands

Small KVM smoke for the KV baseline only:

```bash
sudo build/X86/gem5.opt -d m5out/kv_nvm_seq64_kvm \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_nvm \
  --skip-smoke-tests \
  --kv-seq-len 64 \
  --kv-compute-latency-ns 500 \
  --no-cpu-switch
```

Smoke regression plus KV baseline:

```bash
sudo build/X86/gem5.opt -d m5out/kv_nvm_seq64_with_smoke_kvm \
  configs/example/gem5_library/x86-oracle-gpu-cxl-pcm-fs.py \
  --cxl-pcm-backend from_nvm \
  --kv-seq-len 64 \
  --kv-compute-latency-ns 500 \
  --no-cpu-switch
```

Large KV runs use the same command with `--kv-seq-len 4096`, `8192`, or
`16384`.

## Default Shape And Expected Values

Default parameters:

- `batch_size = 1`
- `num_layers = 1`
- `output_tokens = 1`
- `num_kv_heads = 8`
- `head_dim = 128`
- `dtype_bytes = 2`
- `q_bytes = 2048`
- `output_bytes = 2048`
- OracleGPU descriptor DMA read bytes per command: `200`
- OracleGPU completion write bytes per command: `4`

Expected no-smoke values:

| seq_len | K bytes | V bytes | K+V bytes | 128B read txns | OracleGPU DMA read bytes | OracleGPU DMA write bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 131072 | 131072 | 262144 | 2048 | 264392 | 2052 |
| 4096 | 8388608 | 8388608 | 16777216 | 131072 | 16779464 | 2052 |
| 8192 | 16777216 | 16777216 | 33554432 | 262144 | 33556680 | 2052 |
| 16384 | 33554432 | 33554432 | 67108864 | 524288 | 67111112 | 2052 |

For no-smoke runs, expected CXL-PCM writes are zero because the KV baseline
places output in DDR.

With the existing smoke tests enabled, add the OracleGPU CXL-PCM smoke command:

- `+328` OracleGPU DMA read bytes
- `+132` OracleGPU DMA write bytes
- `+128` CXL-PCM read bytes
- `+128` CXL-PCM write bytes
- `+2` logical 128B PCM read transactions, because smoke uses two separate
  64B CXL-PCM input buffers
- `+1` logical 128B PCM write transaction

## Stats To Check

OracleGPU:

```text
board.oracle_gpu.genericCommandCount
board.oracle_gpu.completedCount
board.oracle_gpu.invalidCommandCount
board.oracle_gpu.dmaReadBytes
board.oracle_gpu.dmaWriteBytes
board.oracle_gpu.lastComputeLatencyTicks
board.oracle_gpu.lastComputeObservedTicks
```

CXL-PCM from_nvm, KVM/atomic:

```text
board.cxl_pcm_memory.mem_ctrl.bytesReadSys
board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys
board.cxl_pcm_memory.mem_ctrl.requestorReadBytes::oracle_gpu
board.cxl_pcm_memory.mem_ctrl.requestorWriteBytes::oracle_gpu
board.cxl_pcm_memory.nvm.bytesRead::oracle_gpu
board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu
board.cxl_pcm_memory.nvm.logicalPcmReadTransactions128B
board.cxl_pcm_memory.nvm.logicalPcmWriteTransactions128B
board.cxl_pcm_memory.nvm.requestorLogicalPcmReadTransactions128B::oracle_gpu
board.cxl_pcm_memory.nvm.requestorLogicalPcmWriteTransactions128B::oracle_gpu
```

In KVM/atomic mode, timing-mode media counters such as
`board.cxl_pcm_memory.nvm.readBursts`,
`board.cxl_pcm_memory.nvm.writeBursts`, and
`board.cxl_pcm_memory.nvm.nvmBytesRead` remain zero. Use
`bytesRead::oracle_gpu` for actual CXL-PCM byte traffic and
`logicalPcmReadTransactions128B` for logical 128B PCM transaction count.

## Validated Runs

Observed no-smoke run:

- `OracleGPU KV offload baseline passed`
- `dmaReadBytes = 264392`
- `dmaWriteBytes = 2052`
- `bytesReadSys = 262144`
- `bytesWrittenSys = 0`
- `logicalPcmReadTransactions128B = 2048`
- `logicalPcmWriteTransactions128B = 0`
- `lastComputeLatencyTicks = 500000`
- `lastComputeObservedTicks = 500000`

Observed smoke plus KV run:

- `CXL-PCM memory test passed`
- `OracleGPU CXL-PCM DMA test passed`
- `OracleGPU KV offload baseline passed`
- `genericCommandCount = 2`
- `completedCount = 2`
- `invalidCommandCount = 0`
- `dmaReadBytes = 264720`
- `dmaWriteBytes = 2184`
- `bytesReadSys = 262272`
- `bytesWrittenSys = 128`
- `logicalPcmReadTransactions128B = 2050`
- `logicalPcmWriteTransactions128B = 1`

Earlier large no-smoke NVM/KVM runs also validated byte traffic for
`seq_len = 4096`, `8192`, and `16384`. Those were not rerun after adding the
logical transaction stat because the `seq_len = 64` run exercises the same
transaction-accounting path.

## Notes

OracleGPU generic input DMA currently issues 64B chunks. This models host-side
request granularity. The CXL-PCM logical PCM transaction stats coalesce
adjacent accesses to the same 128B block, so two adjacent 64B DMA reads from
one 128B block count as one logical PCM read transaction.

Whole-system `finalTick` is not a reliable compute-latency check in KVM FS
runs because it includes boot, disk, guest script, and m5-exit noise. Use
`board.oracle_gpu.lastComputeLatencyTicks` and
`board.oracle_gpu.lastComputeObservedTicks` instead.
