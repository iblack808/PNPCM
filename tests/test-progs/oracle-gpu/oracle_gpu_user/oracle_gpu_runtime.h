#ifndef ORACLE_GPU_RUNTIME_H
#define ORACLE_GPU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "oracle_gpu_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct OracleGpuInput
{
    uint64_t phys_addr;
    uint64_t bytes;
};

struct OracleGpuMappedRegion
{
    void *map_base;
    size_t map_length;
    void *ptr;
    uint64_t phys_addr;
    size_t bytes;
};

struct OracleGpuRuntimeConfig
{
    uint64_t mmio_phys_base;
    size_t mmio_size;
    uint64_t descriptor_phys_addr;
    size_t descriptor_region_bytes;
    uint64_t completion_flag_phys_addr;
    size_t completion_region_bytes;
    uint32_t poll_timeout_ms;
};

struct OracleGpuSubmitResult
{
    uint64_t status;
    uint64_t command_count;
    uint64_t generic_command_count;
    uint64_t completed_count;
    uint64_t invalid_command_count;
    uint64_t dma_read_bytes;
    uint64_t dma_write_bytes;
    uint32_t completion_flag_value;
};

struct OracleGpuRuntime
{
    int mem_fd;
    uint64_t mmio_phys_base;
    size_t mmio_size;
    uint64_t descriptor_phys_addr;
    size_t descriptor_region_bytes;
    uint64_t completion_flag_phys_addr;
    size_t completion_region_bytes;
    uint32_t default_poll_timeout_ms;
    void *mmio_map_base;
    size_t mmio_map_length;
    volatile uint8_t *mmio_base;
    void *descriptor_map_base;
    size_t descriptor_map_length;
    volatile struct OracleGPUCommand *descriptor;
    void *completion_map_base;
    size_t completion_map_length;
    volatile uint32_t *completion_flag;
    char last_error[160];
};

#define ORACLE_GPU_RUNTIME_DEFAULT_MMIO_BASE 0xC1000000ULL
#define ORACLE_GPU_RUNTIME_DEFAULT_MMIO_SIZE 0x1000ULL
#define ORACLE_GPU_RUNTIME_DEFAULT_POLL_TIMEOUT_MS 1000u

int oracle_gpu_init(struct OracleGpuRuntime *rt,
                    const struct OracleGpuRuntimeConfig *config);
int oracle_gpu_map_region(struct OracleGpuRuntime *rt, uint64_t phys_addr,
                          size_t bytes, struct OracleGpuMappedRegion *region);
void oracle_gpu_unmap_region(struct OracleGpuMappedRegion *region);
int oracle_gpu_submit_generic(
    struct OracleGpuRuntime *rt,
    const struct OracleGpuInput *inputs,
    uint32_t num_inputs,
    uint64_t dst_phys_addr,
    uint64_t dst_bytes,
    uint32_t result_policy,
    uint64_t oracle_result_phys_addr,
    uint64_t oracle_result_bytes,
    uint64_t compute_latency_ns,
    uint64_t user_tag,
    struct OracleGpuSubmitResult *result);
void oracle_gpu_close(struct OracleGpuRuntime *rt);
const char *oracle_gpu_last_error(const struct OracleGpuRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif
