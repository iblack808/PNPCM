#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dev/oracle_gpu_protocol.h"

#define ORACLE_GPU_MMIO_ADDR 0xC1000000ULL
#define ORACLE_GPU_MMIO_SIZE 0x1000ULL

#define ORACLE_GPU_SCRATCH_BASE 0x30000000ULL
#define ORACLE_GPU_DESC_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x0000ULL)
#define ORACLE_GPU_SRC_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x1000ULL)
#define ORACLE_GPU_DST_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x2000ULL)
#define ORACLE_GPU_FLAG_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x3000ULL)

static void *
map_physical(int fd, uint64_t phys_addr, size_t size)
{
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     (off_t)phys_addr);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap(%#" PRIx64 ", %#zx) failed: %s\n",
                phys_addr, size, strerror(errno));
        exit(1);
    }
    return ptr;
}

static inline void
mmio_write64(volatile uint8_t *mmio, uint64_t offset, uint64_t value)
{
    *(volatile uint64_t *)(mmio + offset) = value;
}

static inline uint64_t
mmio_read64(volatile uint8_t *mmio, uint64_t offset)
{
    return *(volatile uint64_t *)(mmio + offset);
}

int
main(void)
{
    const char *message = "oracle-gpu-fs-smoke";
    const uint64_t bytes = strlen(message) + 1;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(/dev/mem) failed: %s\n", strerror(errno));
        return 1;
    }

    volatile uint8_t *mmio =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_MMIO_ADDR,
                                         ORACLE_GPU_MMIO_SIZE);
    volatile struct OracleGPUCommand *desc =
        (volatile struct OracleGPUCommand *)map_physical(
            fd, ORACLE_GPU_DESC_ADDR, 0x1000);
    volatile uint8_t *src =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_SRC_ADDR, 0x1000);
    volatile uint8_t *dst =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_DST_ADDR, 0x1000);
    volatile uint32_t *completion_flag =
        (volatile uint32_t *)map_physical(fd, ORACLE_GPU_FLAG_ADDR, 0x1000);

    for (uint64_t i = 0; i < bytes; ++i) {
        src[i] = (uint8_t)message[i];
        dst[i] = 0;
    }
    *completion_flag = 0;

    memset((void *)desc, 0, sizeof(*desc));
    desc->magic = ORACLE_GPU_CMD_MAGIC;
    desc->version = ORACLE_GPU_CMD_VERSION;
    desc->op_type = ORACLE_GPU_OP_GENERIC;
    desc->num_inputs = 1;
    desc->result_policy = ORACLE_GPU_RESULT_COPY_ORACLE;
    desc->dst_addr = ORACLE_GPU_DST_ADDR;
    desc->dst_bytes = bytes;
    desc->oracle_result_addr = ORACLE_GPU_SRC_ADDR;
    desc->oracle_result_bytes = bytes;
    desc->completion_flag_addr = ORACLE_GPU_FLAG_ADDR;
    desc->compute_latency_ns = 500;
    desc->user_tag = 0x534d4f4b45ULL;
    desc->inputs[0].addr = ORACLE_GPU_SRC_ADDR;
    desc->inputs[0].bytes = bytes;

    mmio_write64(mmio, ORACLE_GPU_REG_DESC_ADDR_LO,
                 (uint32_t)(ORACLE_GPU_DESC_ADDR & 0xffffffffULL));
    mmio_write64(mmio, ORACLE_GPU_REG_DESC_ADDR_HI,
                 (uint32_t)(ORACLE_GPU_DESC_ADDR >> 32));
    mmio_write64(mmio, ORACLE_GPU_REG_DOORBELL, 1);

    while (*completion_flag != 1) {
        uint64_t status = mmio_read64(mmio, ORACLE_GPU_REG_STATUS);
        if (status & ORACLE_GPU_STATUS_ERROR) {
            fprintf(stderr, "OracleGPU status error: %#" PRIx64 "\n", status);
            return 2;
        }
    }

    if (memcmp((const void *)src, (const void *)dst, bytes) != 0) {
        fprintf(stderr, "copy mismatch: src='%s' dst='%s'\n",
                (const char *)src, (const char *)dst);
        return 3;
    }

    printf("OracleGPU FS smoke test passed: '%s'\n", (const char *)dst);
    return 0;
}
