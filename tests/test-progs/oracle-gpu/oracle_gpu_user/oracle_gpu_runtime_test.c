#define _GNU_SOURCE

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oracle_gpu_runtime.h"

#define ORACLE_GPU_SCRATCH_BASE 0x30000000ULL
#define ORACLE_GPU_DESCRIPTOR_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x0000ULL)
#define ORACLE_GPU_INPUT0_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x1000ULL)
#define ORACLE_GPU_INPUT1_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x2000ULL)
#define ORACLE_GPU_INPUT2_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x3000ULL)
#define ORACLE_GPU_OUTPUT_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x4000ULL)
#define ORACLE_GPU_COMPLETION_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x5000ULL)

#define INPUT0_BYTES 48u
#define INPUT1_BYTES 80u
#define INPUT2_BYTES 24u
#define OUTPUT_BYTES 96u

static void
fill_buffer(uint8_t *buf, size_t bytes, uint8_t seed)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static int
check_pattern_fill(const uint8_t *buf, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (buf[i] != ORACLE_GPU_PATTERN_BYTE) {
            fprintf(stderr,
                    "pattern-fill mismatch at byte %zu: expected %#x got %#x\n",
                    i, ORACLE_GPU_PATTERN_BYTE, buf[i]);
            return -1;
        }
    }

    return 0;
}

int
main(void)
{
    const uint64_t expected_read_bytes =
        sizeof(struct OracleGPUCommand) + INPUT0_BYTES + INPUT1_BYTES +
        INPUT2_BYTES;
    const uint64_t expected_write_bytes = OUTPUT_BYTES + sizeof(uint32_t);
    struct OracleGpuRuntime rt;
    struct OracleGpuRuntimeConfig config;
    struct OracleGpuMappedRegion input0_region;
    struct OracleGpuMappedRegion input1_region;
    struct OracleGpuMappedRegion input2_region;
    struct OracleGpuMappedRegion output_region;
    struct OracleGpuInput inputs[3];
    struct OracleGpuSubmitResult result;
    int rc = 1;

    memset(&rt, 0, sizeof(rt));
    memset(&config, 0, sizeof(config));
    memset(&input0_region, 0, sizeof(input0_region));
    memset(&input1_region, 0, sizeof(input1_region));
    memset(&input2_region, 0, sizeof(input2_region));
    memset(&output_region, 0, sizeof(output_region));
    memset(&result, 0, sizeof(result));

    config.mmio_phys_base = ORACLE_GPU_RUNTIME_DEFAULT_MMIO_BASE;
    config.mmio_size = ORACLE_GPU_RUNTIME_DEFAULT_MMIO_SIZE;
    config.descriptor_phys_addr = ORACLE_GPU_DESCRIPTOR_ADDR;
    config.descriptor_region_bytes = 0x1000;
    config.completion_flag_phys_addr = ORACLE_GPU_COMPLETION_ADDR;
    config.completion_region_bytes = 0x1000;
    config.poll_timeout_ms = ORACLE_GPU_RUNTIME_DEFAULT_POLL_TIMEOUT_MS;

    if (oracle_gpu_init(&rt, &config) != 0) {
        fprintf(stderr, "oracle_gpu_init failed: %s\n",
                oracle_gpu_last_error(&rt));
        return 1;
    }

    if (oracle_gpu_map_region(&rt, ORACLE_GPU_INPUT0_ADDR, 0x1000,
                              &input0_region) != 0 ||
        oracle_gpu_map_region(&rt, ORACLE_GPU_INPUT1_ADDR, 0x1000,
                              &input1_region) != 0 ||
        oracle_gpu_map_region(&rt, ORACLE_GPU_INPUT2_ADDR, 0x1000,
                              &input2_region) != 0 ||
        oracle_gpu_map_region(&rt, ORACLE_GPU_OUTPUT_ADDR, 0x1000,
                              &output_region) != 0) {
        fprintf(stderr, "oracle_gpu_map_region failed: %s\n",
                oracle_gpu_last_error(&rt));
        goto cleanup;
    }

    fill_buffer((uint8_t *)input0_region.ptr, INPUT0_BYTES, 0x11);
    fill_buffer((uint8_t *)input1_region.ptr, INPUT1_BYTES, 0x55);
    fill_buffer((uint8_t *)input2_region.ptr, INPUT2_BYTES, 0x99);
    memset(output_region.ptr, 0xcc, OUTPUT_BYTES);

    inputs[0].phys_addr = input0_region.phys_addr;
    inputs[0].bytes = INPUT0_BYTES;
    inputs[1].phys_addr = input1_region.phys_addr;
    inputs[1].bytes = INPUT1_BYTES;
    inputs[2].phys_addr = input2_region.phys_addr;
    inputs[2].bytes = INPUT2_BYTES;

    if (oracle_gpu_submit_generic(&rt, inputs, 3, output_region.phys_addr,
                                  OUTPUT_BYTES, ORACLE_GPU_RESULT_PATTERN_FILL,
                                  0, 0, 1500, 0x52554e54494d45ULL,
                                  &result) != 0) {
        fprintf(stderr, "oracle_gpu_submit_generic failed: %s\n",
                oracle_gpu_last_error(&rt));
        goto cleanup;
    }

    if (check_pattern_fill((const uint8_t *)output_region.ptr,
                           OUTPUT_BYTES) != 0) {
        goto cleanup;
    }

    printf("expected read bytes: %" PRIu64 "\n", expected_read_bytes);
    printf("expected write bytes: %" PRIu64 "\n", expected_write_bytes);
    printf("command count: %" PRIu64 "\n", result.command_count);
    printf("generic command count: %" PRIu64 "\n",
           result.generic_command_count);
    printf("dma read bytes: %" PRIu64 "\n", result.dma_read_bytes);
    printf("dma write bytes: %" PRIu64 "\n", result.dma_write_bytes);
    printf("completed count: %" PRIu64 "\n", result.completed_count);
    printf("invalid command count: %" PRIu64 "\n",
           result.invalid_command_count);

    if ((result.status & ORACLE_GPU_STATUS_DONE) == 0 ||
        result.completion_flag_value != 1 ||
        result.command_count != 1 ||
        result.generic_command_count != 1 ||
        result.completed_count != 1 ||
        result.invalid_command_count != 0 ||
        result.dma_read_bytes != expected_read_bytes ||
        result.dma_write_bytes != expected_write_bytes) {
        fprintf(stderr, "OracleGPU runtime stats mismatch\n");
        goto cleanup;
    }

    printf("OracleGPU runtime test passed with %u inputs\n", 3u);
    rc = 0;

cleanup:
    oracle_gpu_unmap_region(&output_region);
    oracle_gpu_unmap_region(&input2_region);
    oracle_gpu_unmap_region(&input1_region);
    oracle_gpu_unmap_region(&input0_region);
    oracle_gpu_close(&rt);
    return rc;
}
