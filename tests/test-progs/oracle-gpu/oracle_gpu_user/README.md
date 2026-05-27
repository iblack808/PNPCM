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
- `oracle_gpu_kv_offload_baseline.c`: FS-mode KV offload baseline driver
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
- `tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_kv_offload_baseline`

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
- the example test and KV baseline driver into `/usr/local/bin`

## KV Offload Baseline Driver

`oracle_gpu_kv_offload_baseline` is the first FS-mode system baseline for
LLM-like KV offload experiments. It does not implement attention, matmul,
PyTorch/vLLM integration, a scheduler, or any OracleGPU attention-specific
command. It only constructs a generic OracleGPU command with three input
segments:

- input0: Q buffer in DDR scratch memory
- input1: K cache buffer in the configured CXL-PCM physical range
- input2: V cache buffer in the configured CXL-PCM physical range
- output: attention-output-shaped buffer in DDR scratch memory

K/V are physical segments inside the configured CXL-PCM range. The driver does
not initialize their payload from the CPU for `ZERO_FILL`, `PATTERN_FILL`,
`COPY_ORACLE`, or `KV_SIZE_PATTERN`; the K/V addresses exist to generate
OracleGPU DMA reads against CXL-PCM. The driver checks that the physical K/V
ranges fit inside
`--cxl-base` and `--cxl-size` before submitting the command.

Default CXL-PCM range:

- base: `0x100000000`
- size: `0x40000000` (1 GiB)

The defaults match the current local FS script, but both values are runtime
arguments:

```bash
/usr/local/bin/oracle_gpu_kv_offload_baseline \
  --cxl-base 0x100000000 \
  --cxl-size 0x40000000 \
  --seq-len 8192 \
  --num-kv-heads 8 \
  --head-dim 128 \
  --dtype-bytes 2 \
  --compute-latency-ns 500 \
  --result-policy pattern
```

To verify that a single KV baseline command reads the full K/V payload and
writes back a specified matrix generated inside OracleGPU, use
`--result-policy kv-size-pattern`. OracleGPU fills the output matrix with a
deterministic byte pattern derived from `K_bytes`, `V_bytes`, and
`K_plus_V_bytes`, then writes it to DDR scratch memory. The guest independently
regenerates the expected bytes and verifies the output with `memcmp`.

For the default shape, the driver prints:

- `K_bytes = 16777216`
- `V_bytes = 16777216`
- `K_plus_V_bytes = 33554432`
- `expected_pcm_read_transactions_128B = 262144`
- `expected_pcm_write_transactions_128B = 0`, because output defaults to DDR

The driver also prints total expected traffic across
`num_layers * output_tokens` generic commands:

- `expected_Q_read_bytes_total`
- `expected_K_read_bytes_total`
- `expected_V_read_bytes_total`
- `expected_payload_dma_read_bytes`
- `expected_oracle_result_read_bytes_total`
- `expected_oraclegpu_dma_read_bytes_including_descriptors`
- `expected_output_write_bytes_total`
- `expected_oraclegpu_dma_write_bytes_including_completion`
- `expected_cxl_pcm_read_bytes`
- `expected_cxl_pcm_write_bytes`

`expected_payload_dma_read_bytes` is the expected Q/K/V payload traffic.
OracleGPU's MMIO/stat counter also includes descriptor DMA reads, so compare
that counter against
`expected_oraclegpu_dma_read_bytes_including_descriptors`.

Useful options:

```bash
/usr/local/bin/oracle_gpu_kv_offload_baseline --help
```

The scratch physical addresses are configurable when a board script changes
the reserved DDR area:

```bash
--descriptor-phys 0x30000000
--completion-phys 0x30001000
--q-phys 0x30002000
--output-phys 0x30003000
```

If `--output-phys` is omitted, it is placed after the Q scratch mapping. K/V
start at `--cxl-base + --cxl-offset`; the driver aligns K/V starts and sizes to
128B where possible and checks that the resulting ranges fit inside
`--cxl-size`.

## Installing the KV Baseline into a Guest Image

Assuming the FS guest root filesystem is mounted at `local_mnt/`:

```bash
make -C tests/test-progs/oracle-gpu/oracle_gpu_user
sudo make -C tests/test-progs/oracle-gpu/oracle_gpu_user \
  DESTDIR=/home/tyb/workspace/SimCXL/local_mnt install
sync
```

Or copy only the static binary:

```bash
sudo cp tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_kv_offload_baseline \
  local_mnt/usr/local/bin/
sudo chmod +x local_mnt/usr/local/bin/oracle_gpu_kv_offload_baseline
sync
```

## Checking gem5 Stats

For OracleGPU traffic, check:

- `board.oracle_gpu.dmaReadBytes`
- `board.oracle_gpu.dmaWriteBytes`
- `board.oracle_gpu.genericCommandCount`
- `board.oracle_gpu.lastComputeLatencyTicks`
- `board.oracle_gpu.lastComputeStartTick`
- `board.oracle_gpu.lastComputeDoneTick`
- `board.oracle_gpu.lastComputeObservedTicks`

For the DRAM-derived CXL-PCM backend, check:

- `board.cxl_pcm_memory.module.pcmReadBytes`
- `board.cxl_pcm_memory.module.pcmWriteBytes`
- `board.cxl_pcm_memory.module.pcmReadRequests`
- `board.cxl_pcm_memory.module.pcmWriteRequests`
- `board.cxl_pcm_memory.module.bytesRead::oracle_gpu`
- `board.cxl_pcm_memory.module.bytesWritten::oracle_gpu`

For the NVM-derived CXL-PCM backend, check:

- `board.cxl_pcm_memory.mem_ctrl.readReqs`
- `board.cxl_pcm_memory.mem_ctrl.writeReqs`
- `board.cxl_pcm_memory.mem_ctrl.bytesReadSys`
- `board.cxl_pcm_memory.mem_ctrl.bytesWrittenSys`
- `board.cxl_pcm_memory.mem_ctrl.requestorReadBytes::oracle_gpu`
- `board.cxl_pcm_memory.nvm.nvmBytesRead`
- `board.cxl_pcm_memory.nvm.nvmBytesWritten`
- `board.cxl_pcm_memory.nvm.readBursts`
- `board.cxl_pcm_memory.nvm.writeBursts`
- `board.cxl_pcm_memory.nvm.bytesRead::oracle_gpu`
- `board.cxl_pcm_memory.nvm.bytesWritten::oracle_gpu`
- `board.cxl_pcm_memory.nvm.logicalPcmReadTransactions128B`
- `board.cxl_pcm_memory.nvm.logicalPcmWriteTransactions128B`
- `board.cxl_pcm_memory.nvm.requestorLogicalPcmReadTransactions128B::oracle_gpu`
- `board.cxl_pcm_memory.nvm.requestorLogicalPcmWriteTransactions128B::oracle_gpu`

In KVM/atomic-mode runs, the NVM-based CXL-PCM controller disables memory
backdoors so OracleGPU DMA reads still enter the CXL/NVM accounting path. For
host-side traffic in KVM mode, compare `mem_ctrl.bytesReadSys`,
`mem_ctrl.requestorReadBytes::oracle_gpu`, and
`nvm.bytesRead::oracle_gpu` against the driver's expected CXL-PCM read bytes.
Timing-mode media counters such as `nvmBytesRead` and `readBursts` are still
useful when running without `--no-cpu-switch`. The
`logicalPcm*Transactions128B` counters are KVM/atomic-mode logical media
transaction counters. They coalesce adjacent 64B host DMA requests that touch
the same 128B PCM block, so they should match the driver's
`expected_pcm_*_transactions_128B` values for streaming K/V reads.

With default DDR output, CXL-PCM write traffic from OracleGPU should be zero.
The CXL-PCM read traffic should track K+V reads, while OracleGPU DMA read
traffic should track Q+K+V plus descriptor reads. Larger
`--compute-latency-ns` values should increase simulated completion time without
changing the expected traffic bytes. In KVM FS-mode runs, use the OracleGPU
compute stats rather than whole-system `finalTick` for latency validation:
`lastComputeLatencyTicks` and `lastComputeObservedTicks` should match, and both
should equal `compute_latency_ns * 1000` when gem5 runs with its default 1 THz
tick frequency.

OracleGPU issues generic input DMA reads in 64B chunks, matching the current
host/cache-line request granularity. The CXL-PCM backend remains responsible
for modeling 128B PCM media accesses.

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
