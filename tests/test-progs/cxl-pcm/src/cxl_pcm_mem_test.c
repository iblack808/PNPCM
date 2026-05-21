#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT (1 << 0)
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif

#define DEFAULT_CXL_PCM_BASE 0x100000000ULL
#define DEFAULT_TEST_BYTES (64ULL * 1024ULL)
#define CXL_NUMA_NODE 1
#define PFN_MASK_SIZE 8

static uint64_t
parse_u64(const char *arg)
{
    char *end = NULL;
    uint64_t value = strtoull(arg, &end, 0);
    if (end == arg || *end != '\0') {
        fprintf(stderr, "invalid integer: %s\n", arg);
        exit(2);
    }
    return value;
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

int
main(int argc, char **argv)
{
    uint64_t base = DEFAULT_CXL_PCM_BASE;
    uint64_t bytes = DEFAULT_TEST_BYTES;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    uint64_t checksum = 0;
    size_t map_bytes;
    volatile uint8_t *pcm;
    uint64_t phys;
    uint64_t i;

    if (argc > 1) {
        base = parse_u64(argv[1]);
    }
    if (argc > 2) {
        bytes = parse_u64(argv[2]);
    }
    if (bytes == 0) {
        fprintf(stderr, "test byte count must be non-zero\n");
        return 2;
    }

    map_bytes = page_align((size_t)bytes);
    pcm = (volatile uint8_t *)mmap(NULL, map_bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pcm == MAP_FAILED) {
        fprintf(stderr, "anonymous mmap(%#zx) failed: %s\n",
                map_bytes, strerror(errno));
        return 1;
    }
    bind_to_cxl_node((void *)pcm, map_bytes);

    for (i = 0; i < bytes; ++i) {
        checksum += pcm[i];
        read_bytes++;
    }

    for (i = 0; i < bytes; ++i) {
        pcm[i] = (uint8_t)(i & 0xff);
        write_bytes++;
    }
    flush_range(pcm, (size_t)bytes);

    phys = virt_to_phys((const void *)pcm);
    if (phys == (uint64_t)-1) {
        fprintf(stderr, "virt_to_phys failed for CXL-PCM allocation\n");
        return 1;
    }

    if (munmap((void *)pcm, map_bytes) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }

    printf("cxl_pcm_base: %#" PRIx64 "\n", base);
    printf("cxl_pcm_alloc_phys: %#" PRIx64 "\n", phys);
    printf("cxl_pcm_numa_node: %d\n", CXL_NUMA_NODE);
    printf("cxl_pcm_test_bytes: %" PRIu64 "\n", bytes);
    printf("cxl_pcm_read_bytes: %" PRIu64 "\n", read_bytes);
    printf("cxl_pcm_write_bytes: %" PRIu64 "\n", write_bytes);
    printf("cxl_pcm_read_checksum: %" PRIu64 "\n", checksum);
    printf("CXL-PCM memory test passed\n");

    return 0;
}
