#define _GNU_SOURCE

#include "oracle_gpu_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void
oracle_gpu_reset_runtime(struct OracleGpuRuntime *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->mem_fd = -1;
}

static int
oracle_gpu_fail(struct OracleGpuRuntime *rt, int err, const char *fmt, ...)
{
    va_list ap;

    if (rt != NULL) {
        va_start(ap, fmt);
        vsnprintf(rt->last_error, sizeof(rt->last_error), fmt, ap);
        va_end(ap);
    }

    errno = err;
    return -1;
}

static int
oracle_gpu_map_fd_region(int mem_fd, uint64_t phys_addr, size_t bytes,
                         volatile void **mapped_ptr, void **map_base,
                         size_t *map_length)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    const uint64_t page_mask = (uint64_t)page_size - 1ULL;
    uint64_t page_base;
    size_t page_offset;
    size_t length;
    void *base;

    if (page_size <= 0 || bytes == 0) {
        errno = EINVAL;
        return -1;
    }

    page_base = phys_addr & ~page_mask;
    page_offset = (size_t)(phys_addr - page_base);
    length = page_offset + bytes;

    base = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd,
                (off_t)page_base);
    if (base == MAP_FAILED) {
        return -1;
    }

    *map_base = base;
    *map_length = length;
    *mapped_ptr = (volatile uint8_t *)base + page_offset;
    return 0;
}

static inline void
oracle_gpu_mmio_write64(volatile uint8_t *mmio_base, uint64_t offset,
                        uint64_t value)
{
    *(volatile uint64_t *)(mmio_base + offset) = value;
}

static inline uint64_t
oracle_gpu_mmio_read64(volatile uint8_t *mmio_base, uint64_t offset)
{
    return *(volatile uint64_t *)(mmio_base + offset);
}

static void
oracle_gpu_fill_submit_result(struct OracleGpuRuntime *rt,
                              struct OracleGpuSubmitResult *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->status = oracle_gpu_mmio_read64(rt->mmio_base,
                                            ORACLE_GPU_REG_STATUS);
    result->command_count = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_COMMAND_COUNT);
    result->generic_command_count = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_GENERIC_COMMAND_COUNT);
    result->completed_count = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_COMPLETED_COUNT);
    result->invalid_command_count = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_INVALID_COMMAND_COUNT);
    result->dma_read_bytes = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_DMA_READ_BYTES);
    result->dma_write_bytes = oracle_gpu_mmio_read64(
        rt->mmio_base, ORACLE_GPU_REG_DMA_WRITE_BYTES);
    result->completion_flag_value = *rt->completion_flag;
}

const char *
oracle_gpu_last_error(const struct OracleGpuRuntime *rt)
{
    if (rt == NULL || rt->last_error[0] == '\0') {
        return "no error";
    }

    return rt->last_error;
}

int
oracle_gpu_init(struct OracleGpuRuntime *rt,
                const struct OracleGpuRuntimeConfig *config)
{
    if (rt == NULL || config == NULL) {
        errno = EINVAL;
        return -1;
    }

    oracle_gpu_reset_runtime(rt);

    if (config->mmio_phys_base == 0 || config->mmio_size < 8 ||
        config->descriptor_phys_addr == 0 ||
        config->descriptor_region_bytes < sizeof(struct OracleGPUCommand) ||
        config->completion_flag_phys_addr == 0 ||
        config->completion_region_bytes < sizeof(uint32_t)) {
        return oracle_gpu_fail(rt, EINVAL,
            "invalid OracleGPU runtime configuration");
    }

    rt->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (rt->mem_fd < 0) {
        return oracle_gpu_fail(rt, errno,
            "open(/dev/mem) failed: %s", strerror(errno));
    }

    if (oracle_gpu_map_fd_region(rt->mem_fd, config->mmio_phys_base,
                                 config->mmio_size,
                                 (volatile void **)&rt->mmio_base,
                                 &rt->mmio_map_base,
                                 &rt->mmio_map_length) != 0) {
        oracle_gpu_close(rt);
        return oracle_gpu_fail(rt, errno,
            "mmap(mmio=%#llx, size=%#zx) failed: %s",
            (unsigned long long)config->mmio_phys_base, config->mmio_size,
            strerror(errno));
    }

    if (oracle_gpu_map_fd_region(rt->mem_fd, config->descriptor_phys_addr,
                                 config->descriptor_region_bytes,
                                 (volatile void **)&rt->descriptor,
                                 &rt->descriptor_map_base,
                                 &rt->descriptor_map_length) != 0) {
        oracle_gpu_close(rt);
        return oracle_gpu_fail(rt, errno,
            "mmap(descriptor=%#llx, size=%#zx) failed: %s",
            (unsigned long long)config->descriptor_phys_addr,
            config->descriptor_region_bytes, strerror(errno));
    }

    if (oracle_gpu_map_fd_region(rt->mem_fd,
                                 config->completion_flag_phys_addr,
                                 config->completion_region_bytes,
                                 (volatile void **)&rt->completion_flag,
                                 &rt->completion_map_base,
                                 &rt->completion_map_length) != 0) {
        oracle_gpu_close(rt);
        return oracle_gpu_fail(rt, errno,
            "mmap(completion=%#llx, size=%#zx) failed: %s",
            (unsigned long long)config->completion_flag_phys_addr,
            config->completion_region_bytes, strerror(errno));
    }

    rt->mmio_phys_base = config->mmio_phys_base;
    rt->mmio_size = config->mmio_size;
    rt->descriptor_phys_addr = config->descriptor_phys_addr;
    rt->descriptor_region_bytes = config->descriptor_region_bytes;
    rt->completion_flag_phys_addr = config->completion_flag_phys_addr;
    rt->completion_region_bytes = config->completion_region_bytes;
    rt->default_poll_timeout_ms = config->poll_timeout_ms;
    if (rt->default_poll_timeout_ms == 0) {
        rt->default_poll_timeout_ms =
            ORACLE_GPU_RUNTIME_DEFAULT_POLL_TIMEOUT_MS;
    }

    return 0;
}

int
oracle_gpu_map_region(struct OracleGpuRuntime *rt, uint64_t phys_addr,
                      size_t bytes, struct OracleGpuMappedRegion *region)
{
    volatile void *mapped_ptr;

    if (rt == NULL || region == NULL || rt->mem_fd < 0 || phys_addr == 0 ||
        bytes == 0) {
        return oracle_gpu_fail(rt, EINVAL,
            "invalid oracle_gpu_map_region arguments");
    }

    memset(region, 0, sizeof(*region));
    if (oracle_gpu_map_fd_region(rt->mem_fd, phys_addr, bytes, &mapped_ptr,
                                 &region->map_base,
                                 &region->map_length) != 0) {
        return oracle_gpu_fail(rt, errno,
            "mmap(region=%#llx, size=%#zx) failed: %s",
            (unsigned long long)phys_addr, bytes, strerror(errno));
    }

    region->ptr = (void *)mapped_ptr;
    region->phys_addr = phys_addr;
    region->bytes = bytes;
    return 0;
}

void
oracle_gpu_unmap_region(struct OracleGpuMappedRegion *region)
{
    if (region == NULL) {
        return;
    }

    if (region->map_base != NULL) {
        munmap(region->map_base, region->map_length);
    }

    memset(region, 0, sizeof(*region));
}

int
oracle_gpu_submit_generic(struct OracleGpuRuntime *rt,
                          const struct OracleGpuInput *inputs,
                          uint32_t num_inputs,
                          uint64_t dst_phys_addr,
                          uint64_t dst_bytes,
                          uint32_t result_policy,
                          uint64_t oracle_result_phys_addr,
                          uint64_t oracle_result_bytes,
                          uint64_t compute_latency_ns,
                          uint64_t user_tag,
                          struct OracleGpuSubmitResult *result)
{
    struct OracleGPUCommand cmd;
    struct timespec ts;
    uint64_t deadline_ns;
    uint64_t now_ns;
    uint64_t status;
    uint32_t completion;
    uint32_t i;

    if (rt == NULL || inputs == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rt->mem_fd < 0 || rt->mmio_base == NULL || rt->descriptor == NULL ||
        rt->completion_flag == NULL) {
        return oracle_gpu_fail(rt, EINVAL, "runtime is not initialized");
    }

    if (num_inputs == 0 || num_inputs > ORACLE_GPU_MAX_INPUTS ||
        dst_phys_addr == 0 || dst_bytes == 0) {
        return oracle_gpu_fail(rt, EINVAL,
            "invalid OracleGPU generic submit arguments");
    }

    for (i = 0; i < num_inputs; ++i) {
        if (inputs[i].phys_addr == 0 || inputs[i].bytes == 0) {
            return oracle_gpu_fail(rt, EINVAL,
                "input[%u] has null address or zero bytes", i);
        }
    }

    if (result_policy == ORACLE_GPU_RESULT_COPY_ORACLE) {
        if (oracle_result_phys_addr == 0 || oracle_result_bytes != dst_bytes) {
            return oracle_gpu_fail(rt, EINVAL,
                "COPY_ORACLE requires oracle_result_phys_addr and matching size");
        }
    } else if (result_policy != ORACLE_GPU_RESULT_ZERO_FILL &&
               result_policy != ORACLE_GPU_RESULT_PATTERN_FILL &&
               result_policy != ORACLE_GPU_RESULT_KV_SIZE_PATTERN) {
        return oracle_gpu_fail(rt, EINVAL,
            "unsupported result_policy %u", result_policy);
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = ORACLE_GPU_CMD_MAGIC;
    cmd.version = ORACLE_GPU_CMD_VERSION;
    cmd.op_type = ORACLE_GPU_OP_GENERIC;
    cmd.num_inputs = num_inputs;
    cmd.result_policy = result_policy;
    cmd.dst_addr = dst_phys_addr;
    cmd.dst_bytes = dst_bytes;
    cmd.oracle_result_addr = oracle_result_phys_addr;
    cmd.oracle_result_bytes = oracle_result_bytes;
    cmd.compute_latency_ns = compute_latency_ns;
    cmd.completion_flag_addr = rt->completion_flag_phys_addr;
    cmd.user_tag = user_tag;

    for (i = 0; i < num_inputs; ++i) {
        cmd.inputs[i].addr = inputs[i].phys_addr;
        cmd.inputs[i].bytes = inputs[i].bytes;
    }

    *rt->completion_flag = 0;
    memcpy((void *)rt->descriptor, &cmd, sizeof(cmd));

    oracle_gpu_mmio_write64(rt->mmio_base, ORACLE_GPU_REG_DESC_ADDR_LO,
                            (uint32_t)(rt->descriptor_phys_addr &
                                       0xffffffffULL));
    oracle_gpu_mmio_write64(rt->mmio_base, ORACLE_GPU_REG_DESC_ADDR_HI,
                            (uint32_t)(rt->descriptor_phys_addr >> 32));
    oracle_gpu_mmio_write64(rt->mmio_base, ORACLE_GPU_REG_DOORBELL, 1);

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        oracle_gpu_fill_submit_result(rt, result);
        return oracle_gpu_fail(rt, errno,
            "clock_gettime failed: %s", strerror(errno));
    }

    now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    deadline_ns =
        now_ns + (uint64_t)rt->default_poll_timeout_ms * 1000000ULL;

    for (uint32_t spins = 0;; ++spins) {
        completion = *rt->completion_flag;
        status = oracle_gpu_mmio_read64(rt->mmio_base, ORACLE_GPU_REG_STATUS);

        if ((status & ORACLE_GPU_STATUS_ERROR) != 0) {
            oracle_gpu_fill_submit_result(rt, result);
            return oracle_gpu_fail(rt, EIO,
                "OracleGPU reported status error %#llx",
                (unsigned long long)status);
        }

        if (completion == 1) {
            break;
        }

        if ((spins & 0x3ffu) == 0) {
            if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
                oracle_gpu_fill_submit_result(rt, result);
                return oracle_gpu_fail(rt, errno,
                    "clock_gettime failed: %s", strerror(errno));
            }

            now_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                     (uint64_t)ts.tv_nsec;
            if (now_ns > deadline_ns) {
                oracle_gpu_fill_submit_result(rt, result);
                return oracle_gpu_fail(rt, ETIMEDOUT,
                    "OracleGPU completion timed out after %u ms",
                    rt->default_poll_timeout_ms);
            }

            sched_yield();
        }
    }

    oracle_gpu_fill_submit_result(rt, result);
    rt->last_error[0] = '\0';
    return 0;
}

void
oracle_gpu_close(struct OracleGpuRuntime *rt)
{
    if (rt == NULL) {
        return;
    }

    if (rt->mmio_map_base != NULL) {
        munmap(rt->mmio_map_base, rt->mmio_map_length);
    }
    if (rt->descriptor_map_base != NULL) {
        munmap(rt->descriptor_map_base, rt->descriptor_map_length);
    }
    if (rt->completion_map_base != NULL) {
        munmap(rt->completion_map_base, rt->completion_map_length);
    }
    if (rt->mem_fd >= 0) {
        close(rt->mem_fd);
    }

    oracle_gpu_reset_runtime(rt);
}
