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

#define ORACLE_GPU_MMIO_ADDR 0xC1000000ULL
#define ORACLE_GPU_MMIO_SIZE 0x1000ULL

#define ORACLE_GPU_DESC_ADDR 0x30000000ULL
#define ORACLE_GPU_FLAG_ADDR 0x30001000ULL
#define ORACLE_GPU_Q_ADDR 0x30100000ULL
#define ORACLE_GPU_K_ADDR 0x32000000ULL
#define ORACLE_GPU_V_ADDR 0x34000000ULL
#define ORACLE_GPU_OUT_ADDR 0x36000000ULL

#define REG_DESC_ADDR_LO 0x00
#define REG_DESC_ADDR_HI 0x08
#define REG_DOORBELL 0x10
#define REG_STATUS 0x18

#define ORACLE_GPU_OP_DECODE_ATTENTION 2U
#define STATUS_ERROR (1ULL << 2)

struct oracle_gpu_copy_cmd {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint64_t bytes;
};

struct oracle_gpu_decode_attention_cmd {
    uint64_t q_addr;
    uint64_t q_bytes;
    uint64_t k_cache_addr;
    uint64_t k_cache_bytes;
    uint64_t v_cache_addr;
    uint64_t v_cache_bytes;
    uint64_t out_addr;
    uint64_t out_bytes;
    uint32_t batch_size;
    uint32_t seq_len;
    uint32_t num_q_heads;
    uint32_t num_kv_heads;
    uint32_t head_dim;
    uint32_t dtype_bytes;
    uint32_t layer_id;
    uint32_t request_id;
};

struct oracle_gpu_desc {
    uint32_t op;
    uint32_t reserved0;
    uint64_t compute_latency_ns;
    uint64_t completion_flag_addr;
    union {
        struct oracle_gpu_copy_cmd copy;
        struct oracle_gpu_decode_attention_cmd decode_attention;
    };
};

_Static_assert(sizeof(struct oracle_gpu_desc) == 120,
               "oracle_gpu_desc layout must match OracleGPU");

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

static size_t
round_up_page(size_t size)
{
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    return ((size + page_size - 1) / page_size) * page_size;
}

static uint8_t
expected_output_byte(uint32_t layer_id, uint32_t request_id, uint64_t index)
{
    return (uint8_t)((layer_id + request_id + index) & 0xffU);
}

int
main(void)
{
    const uint32_t batch_size = 1;
    const uint32_t seq_len = 8192;
    const uint32_t num_q_heads = 8;
    const uint32_t num_kv_heads = 8;
    const uint32_t head_dim = 128;
    const uint32_t dtype_bytes = 2;
    const uint32_t layer_id = 7;
    const uint32_t request_id = 42;
    const uint64_t q_bytes =
        (uint64_t)batch_size * num_q_heads * head_dim * dtype_bytes;
    const uint64_t k_cache_bytes =
        (uint64_t)seq_len * num_kv_heads * head_dim * dtype_bytes;
    const uint64_t v_cache_bytes = k_cache_bytes;
    const uint64_t out_bytes =
        (uint64_t)batch_size * num_q_heads * head_dim * dtype_bytes;
    const uint64_t total_read_bytes = q_bytes + k_cache_bytes + v_cache_bytes;
    const uint64_t total_write_bytes = out_bytes + sizeof(uint32_t);
    const uint64_t compute_latency_ns = 200000;
    const size_t q_map_size = round_up_page((size_t)q_bytes);
    const size_t k_map_size = round_up_page((size_t)k_cache_bytes);
    const size_t v_map_size = round_up_page((size_t)v_cache_bytes);
    const size_t out_map_size = round_up_page((size_t)out_bytes);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(/dev/mem) failed: %s\n", strerror(errno));
        return 1;
    }

    volatile uint8_t *mmio =
        (volatile uint8_t *)map_physical(fd, ORACLE_GPU_MMIO_ADDR,
                                         ORACLE_GPU_MMIO_SIZE);
    struct oracle_gpu_desc *desc =
        (struct oracle_gpu_desc *)map_physical(fd, ORACLE_GPU_DESC_ADDR, 0x1000);
    uint32_t *completion_flag =
        (uint32_t *)map_physical(fd, ORACLE_GPU_FLAG_ADDR, 0x1000);
    uint8_t *q_buf = (uint8_t *)map_physical(fd, ORACLE_GPU_Q_ADDR, q_map_size);
    uint8_t *k_buf = (uint8_t *)map_physical(fd, ORACLE_GPU_K_ADDR, k_map_size);
    uint8_t *v_buf = (uint8_t *)map_physical(fd, ORACLE_GPU_V_ADDR, v_map_size);
    uint8_t *out_buf =
        (uint8_t *)map_physical(fd, ORACLE_GPU_OUT_ADDR, out_map_size);

    for (uint64_t i = 0; i < q_bytes; ++i) {
        q_buf[i] = (uint8_t)(i & 0xffU);
    }
    for (uint64_t i = 0; i < k_cache_bytes; ++i) {
        k_buf[i] = (uint8_t)((i + 0x11U) & 0xffU);
    }
    for (uint64_t i = 0; i < v_cache_bytes; ++i) {
        v_buf[i] = (uint8_t)((i + 0x5aU) & 0xffU);
    }
    memset(out_buf, 0, out_map_size);
    memset(desc, 0, sizeof(*desc));
    *completion_flag = 0;

    desc->op = ORACLE_GPU_OP_DECODE_ATTENTION;
    desc->compute_latency_ns = compute_latency_ns;
    desc->completion_flag_addr = ORACLE_GPU_FLAG_ADDR;
    desc->decode_attention.q_addr = ORACLE_GPU_Q_ADDR;
    desc->decode_attention.q_bytes = q_bytes;
    desc->decode_attention.k_cache_addr = ORACLE_GPU_K_ADDR;
    desc->decode_attention.k_cache_bytes = k_cache_bytes;
    desc->decode_attention.v_cache_addr = ORACLE_GPU_V_ADDR;
    desc->decode_attention.v_cache_bytes = v_cache_bytes;
    desc->decode_attention.out_addr = ORACLE_GPU_OUT_ADDR;
    desc->decode_attention.out_bytes = out_bytes;
    desc->decode_attention.batch_size = batch_size;
    desc->decode_attention.seq_len = seq_len;
    desc->decode_attention.num_q_heads = num_q_heads;
    desc->decode_attention.num_kv_heads = num_kv_heads;
    desc->decode_attention.head_dim = head_dim;
    desc->decode_attention.dtype_bytes = dtype_bytes;
    desc->decode_attention.layer_id = layer_id;
    desc->decode_attention.request_id = request_id;

    printf("Submitting decode-attention command:\n");
    printf("  seq_len=%u num_q_heads=%u num_kv_heads=%u head_dim=%u dtype_bytes=%u\n",
           seq_len, num_q_heads, num_kv_heads, head_dim, dtype_bytes);
    printf("Expected DMA reads: q=%" PRIu64 " k=%" PRIu64 " v=%" PRIu64
           " total=%" PRIu64 "\n",
           q_bytes, k_cache_bytes, v_cache_bytes, total_read_bytes);
    printf("Expected DMA writes: out=%" PRIu64 " completion=%zu total=%" PRIu64
           "\n",
           out_bytes, sizeof(uint32_t), total_write_bytes);

    *(volatile uint64_t *)(mmio + REG_DESC_ADDR_LO) =
        (uint32_t)(ORACLE_GPU_DESC_ADDR & 0xffffffffULL);
    *(volatile uint64_t *)(mmio + REG_DESC_ADDR_HI) =
        (uint32_t)(ORACLE_GPU_DESC_ADDR >> 32);
    *(volatile uint64_t *)(mmio + REG_DOORBELL) = 1;

    while (*completion_flag != 1) {
        uint64_t status = *(volatile uint64_t *)(mmio + REG_STATUS);
        if (status & STATUS_ERROR) {
            fprintf(stderr, "OracleGPU status error: %#" PRIx64 "\n", status);
            return 2;
        }
    }

    for (uint64_t i = 0; i < out_bytes; ++i) {
        const uint8_t expected =
            expected_output_byte(layer_id, request_id, i);
        if (out_buf[i] != expected) {
            fprintf(stderr,
                    "output mismatch at byte %" PRIu64 ": got %#x expected %#x\n",
                    i, out_buf[i], expected);
            return 3;
        }
    }

    printf("OracleGPU decode-attention test passed: completion=%u out_bytes=%" PRIu64
           " total_kv_read=%" PRIu64 "\n",
           *completion_flag, out_bytes, k_cache_bytes + v_cache_bytes);
    return 0;
}
