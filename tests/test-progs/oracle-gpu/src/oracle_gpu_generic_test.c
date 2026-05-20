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
#define ORACLE_GPU_INPUT0_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x1000ULL)
#define ORACLE_GPU_INPUT1_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x2000ULL)
#define ORACLE_GPU_INPUT2_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x3000ULL)
#define ORACLE_GPU_DST_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x4000ULL)
#define ORACLE_GPU_FLAG_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x5000ULL)

#define INPUT0_BYTES 64u
#define INPUT1_BYTES 96u
#define INPUT2_BYTES 32u
#define DST_BYTES 128u

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

static void
fill_buffer(volatile uint8_t *buf, size_t bytes, uint8_t seed)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static int
check_pattern(volatile uint8_t *buf, size_t bytes, uint8_t pattern)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (buf[i] != pattern) {
            fprintf(stderr,
                    "pattern mismatch at byte %zu: expected %#x got %#x\n",
                    i, pattern, buf[i]);
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
    const uint64_t expected_write_bytes = DST_BYTES + sizeof(uint32_t);
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
    volatile uint8_t *input0 =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_INPUT0_ADDR, 0x1000);
    volatile uint8_t *input1 =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_INPUT1_ADDR, 0x1000);
    volatile uint8_t *input2 =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_INPUT2_ADDR, 0x1000);
    volatile uint8_t *dst =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_DST_ADDR, 0x1000);
    volatile uint32_t *completion_flag =
        (volatile uint32_t *)map_physical(fd, ORACLE_GPU_FLAG_ADDR, 0x1000);

    fill_buffer(input0, INPUT0_BYTES, 0x10);
    fill_buffer(input1, INPUT1_BYTES, 0x40);
    fill_buffer(input2, INPUT2_BYTES, 0x80);
    memset((void *)dst, 0xcc, DST_BYTES);
    *completion_flag = 0;

    memset((void *)desc, 0, sizeof(*desc));
    desc->magic = ORACLE_GPU_CMD_MAGIC;
    desc->version = ORACLE_GPU_CMD_VERSION;
    desc->op_type = ORACLE_GPU_OP_GENERIC;
    desc->num_inputs = 3;
    desc->result_policy = ORACLE_GPU_RESULT_PATTERN_FILL;
    desc->dst_addr = ORACLE_GPU_DST_ADDR;
    desc->dst_bytes = DST_BYTES;
    desc->compute_latency_ns = 2000;
    desc->completion_flag_addr = ORACLE_GPU_FLAG_ADDR;
    desc->user_tag = 0x47454e45524943ULL;
    desc->inputs[0].addr = ORACLE_GPU_INPUT0_ADDR;
    desc->inputs[0].bytes = INPUT0_BYTES;
    desc->inputs[1].addr = ORACLE_GPU_INPUT1_ADDR;
    desc->inputs[1].bytes = INPUT1_BYTES;
    desc->inputs[2].addr = ORACLE_GPU_INPUT2_ADDR;
    desc->inputs[2].bytes = INPUT2_BYTES;

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

    if (check_pattern(dst, DST_BYTES, ORACLE_GPU_PATTERN_BYTE) != 0) {
        return 3;
    }

    printf("expected read bytes: %" PRIu64 "\n", expected_read_bytes);
    printf("expected write bytes: %" PRIu64 "\n", expected_write_bytes);
    printf("command count: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_COMMAND_COUNT));
    printf("generic command count: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_GENERIC_COMMAND_COUNT));
    printf("dma read bytes: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_DMA_READ_BYTES));
    printf("dma write bytes: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_DMA_WRITE_BYTES));
    printf("completed count: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_COMPLETED_COUNT));
    printf("invalid command count: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_INVALID_COMMAND_COUNT));

    if (mmio_read64(mmio, ORACLE_GPU_REG_COMMAND_COUNT) != 1 ||
        mmio_read64(mmio, ORACLE_GPU_REG_GENERIC_COMMAND_COUNT) != 1 ||
        mmio_read64(mmio, ORACLE_GPU_REG_COMPLETED_COUNT) != 1 ||
        mmio_read64(mmio, ORACLE_GPU_REG_INVALID_COMMAND_COUNT) != 0 ||
        mmio_read64(mmio, ORACLE_GPU_REG_DMA_READ_BYTES) !=
            expected_read_bytes ||
        mmio_read64(mmio, ORACLE_GPU_REG_DMA_WRITE_BYTES) !=
            expected_write_bytes) {
        fprintf(stderr, "OracleGPU MMIO stats mismatch\n");
        return 4;
    }

    printf("OracleGPU generic test passed with %u inputs\n", desc->num_inputs);
    return 0;
}
