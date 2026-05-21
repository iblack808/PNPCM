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
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "dev/oracle_gpu_protocol.h"

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT (1 << 0)
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif

#define ORACLE_GPU_MMIO_ADDR 0xC1000000ULL
#define ORACLE_GPU_MMIO_SIZE 0x1000ULL

#define ORACLE_GPU_SCRATCH_BASE 0x30000000ULL
#define ORACLE_GPU_DESC_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x0000ULL)
#define ORACLE_GPU_FLAG_ADDR (ORACLE_GPU_SCRATCH_BASE + 0x1000ULL)

#define CXL_PCM_BASE 0x100000000ULL
#define CXL_NUMA_NODE 1
#define PFN_MASK_SIZE 8

#define INPUT0_BYTES 64u
#define INPUT1_BYTES 64u
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

static size_t
page_align(size_t size)
{
    const size_t page_size = (size_t)getpagesize();
    return (size + page_size - 1) & ~(page_size - 1);
}

static void
bind_to_cxl_node(void *addr, size_t size)
{
    unsigned long nodemask = 1UL << CXL_NUMA_NODE;
    long ret = syscall(SYS_mbind, addr, size, MPOL_BIND, &nodemask,
                       sizeof(nodemask) * 8, MPOL_MF_STRICT | MPOL_MF_MOVE);
    if (ret != 0) {
        fprintf(stderr, "mbind(node=%d, size=%#zx) failed: %s\n",
                CXL_NUMA_NODE, size, strerror(errno));
        exit(1);
    }
}

static uint64_t
virt_to_phys(const void *virtaddr)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    uint64_t page;
    unsigned long virt_pfn;
    off_t offset;
    int page_size;

    if (fd < 0) {
        return (uint64_t)-1;
    }

    page_size = getpagesize();
    virt_pfn = (unsigned long)virtaddr / page_size;
    offset = (off_t)(sizeof(uint64_t) * virt_pfn);
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
        return (uint64_t)-1;
    }
    if (read(fd, &page, PFN_MASK_SIZE) != PFN_MASK_SIZE) {
        close(fd);
        return (uint64_t)-1;
    }
    close(fd);

    if ((page & 0x7fffffffffffffULL) == 0) {
        return (uint64_t)-1;
    }

    return ((page & 0x7fffffffffffffULL) * (uint64_t)page_size) +
        ((uint64_t)virtaddr % (uint64_t)page_size);
}

static void
flush_range(volatile uint8_t *buf, size_t bytes)
{
    const size_t line_size = 64;
    uintptr_t start = (uintptr_t)buf & ~(uintptr_t)(line_size - 1);
    uintptr_t end = (uintptr_t)buf + bytes;
    uintptr_t p;

    for (p = start; p < end; p += line_size) {
        __builtin_ia32_clflush((const void *)p);
    }
    __sync_synchronize();
}

static volatile uint8_t *
alloc_cxl_pcm(size_t bytes)
{
    volatile uint8_t *ptr;
    size_t map_bytes = page_align(bytes);

    ptr = (volatile uint8_t *)mmap(NULL, map_bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "anonymous mmap(%#zx) failed: %s\n",
                map_bytes, strerror(errno));
        exit(1);
    }
    bind_to_cxl_node((void *)ptr, map_bytes);
    return ptr;
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
        sizeof(struct OracleGPUCommand) + INPUT0_BYTES + INPUT1_BYTES;
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
    volatile uint32_t *completion_flag =
        (volatile uint32_t *)map_physical(fd, ORACLE_GPU_FLAG_ADDR, 0x1000);
    volatile uint8_t *input0 = alloc_cxl_pcm(INPUT0_BYTES);
    volatile uint8_t *input1 = alloc_cxl_pcm(INPUT1_BYTES);
    volatile uint8_t *dst = alloc_cxl_pcm(DST_BYTES);
    uint64_t input0_phys;
    uint64_t input1_phys;
    uint64_t dst_phys;

    fill_buffer(input0, INPUT0_BYTES, 0x21);
    fill_buffer(input1, INPUT1_BYTES, 0x61);
    memset((void *)dst, 0xcc, DST_BYTES);
    flush_range(input0, INPUT0_BYTES);
    flush_range(input1, INPUT1_BYTES);
    flush_range(dst, DST_BYTES);
    *completion_flag = 0;

    input0_phys = virt_to_phys((const void *)input0);
    input1_phys = virt_to_phys((const void *)input1);
    dst_phys = virt_to_phys((const void *)dst);
    if (input0_phys == (uint64_t)-1 || input1_phys == (uint64_t)-1 ||
        dst_phys == (uint64_t)-1) {
        fprintf(stderr, "virt_to_phys failed for CXL-PCM buffers\n");
        return 1;
    }

    memset((void *)desc, 0, sizeof(*desc));
    desc->magic = ORACLE_GPU_CMD_MAGIC;
    desc->version = ORACLE_GPU_CMD_VERSION;
    desc->op_type = ORACLE_GPU_OP_GENERIC;
    desc->num_inputs = 2;
    desc->result_policy = ORACLE_GPU_RESULT_PATTERN_FILL;
    desc->dst_addr = dst_phys;
    desc->dst_bytes = DST_BYTES;
    desc->compute_latency_ns = 500;
    desc->completion_flag_addr = ORACLE_GPU_FLAG_ADDR;
    desc->user_tag = 0x43584c50434dULL;
    desc->inputs[0].addr = input0_phys;
    desc->inputs[0].bytes = INPUT0_BYTES;
    desc->inputs[1].addr = input1_phys;
    desc->inputs[1].bytes = INPUT1_BYTES;

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

    printf("oracle_gpu_cxl_pcm_input0_phys: %#" PRIx64 "\n", input0_phys);
    printf("oracle_gpu_cxl_pcm_input1_phys: %#" PRIx64 "\n", input1_phys);
    printf("oracle_gpu_cxl_pcm_dst_phys: %#" PRIx64 "\n", dst_phys);
    printf("oracle_gpu_cxl_pcm_input_read_bytes: %u\n",
           INPUT0_BYTES + INPUT1_BYTES);
    printf("oracle_gpu_cxl_pcm_output_write_bytes: %u\n", DST_BYTES);
    printf("dma read bytes: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_DMA_READ_BYTES));
    printf("dma write bytes: %" PRIu64 "\n",
           mmio_read64(mmio, ORACLE_GPU_REG_DMA_WRITE_BYTES));

    if (mmio_read64(mmio, ORACLE_GPU_REG_DMA_READ_BYTES) !=
            expected_read_bytes ||
        mmio_read64(mmio, ORACLE_GPU_REG_DMA_WRITE_BYTES) !=
            expected_write_bytes) {
        fprintf(stderr, "OracleGPU CXL-PCM DMA stats mismatch\n");
        return 4;
    }

    printf("OracleGPU CXL-PCM DMA test passed\n");
    return 0;
}
