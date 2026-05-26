#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oracle_gpu_runtime.h"

#define DEFAULT_SCRATCH_BASE 0x30000000ULL
#define DEFAULT_DESCRIPTOR_ADDR (DEFAULT_SCRATCH_BASE + 0x0000ULL)
#define DEFAULT_COMPLETION_ADDR (DEFAULT_SCRATCH_BASE + 0x1000ULL)
#define DEFAULT_Q_ADDR (DEFAULT_SCRATCH_BASE + 0x2000ULL)

#define DEFAULT_CXL_BASE 0x100000000ULL
#define DEFAULT_CXL_SIZE (1ULL << 30)
#define DEFAULT_BATCH_SIZE 1ULL
#define DEFAULT_NUM_LAYERS 1ULL
#define DEFAULT_OUTPUT_TOKENS 1ULL
#define DEFAULT_SEQ_LEN 8192ULL
#define DEFAULT_NUM_KV_HEADS 8ULL
#define DEFAULT_HEAD_DIM 128ULL
#define DEFAULT_DTYPE_BYTES 2ULL
#define DEFAULT_COMPUTE_LATENCY_NS 500ULL
#define PCM_MEDIA_GRANULARITY_BYTES 128ULL

struct BaselineConfig
{
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t descriptor_phys;
    uint64_t completion_phys;
    uint64_t q_phys;
    uint64_t output_phys;
    int output_phys_set;
    uint64_t cxl_base;
    uint64_t cxl_size;
    uint64_t cxl_offset;
    uint64_t batch_size;
    uint64_t num_layers;
    uint64_t output_tokens;
    uint64_t seq_len;
    uint64_t num_kv_heads;
    uint64_t head_dim;
    uint64_t dtype_bytes;
    uint64_t compute_latency_ns;
    uint32_t result_policy;
    uint32_t poll_timeout_ms;
};

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "OracleGPU FS-mode KV offload baseline. Q/output are DDR scratch\n"
        "buffers; K/V are physical segments inside the CXL-PCM range.\n"
        "\n"
        "Options:\n"
        "  --mmio-base ADDR          OracleGPU MMIO base "
        "(default %#llx)\n"
        "  --mmio-size BYTES         OracleGPU MMIO size "
        "(default %#llx)\n"
        "  --descriptor-phys ADDR    DDR descriptor physical address "
        "(default %#llx)\n"
        "  --completion-phys ADDR    DDR completion flag physical address "
        "(default %#llx)\n"
        "  --q-phys ADDR             DDR Q buffer physical address "
        "(default %#llx)\n"
        "  --output-phys ADDR        DDR output buffer physical address "
        "(default follows Q buffer)\n"
        "  --cxl-base ADDR           CXL-PCM physical base "
        "(default %#llx)\n"
        "  --cxl-size BYTES          CXL-PCM range size "
        "(default %#llx)\n"
        "  --cxl-offset BYTES        Offset inside CXL-PCM for K/V "
        "(default 0)\n"
        "  --batch-size N            Batch size (default %llu)\n"
        "  --num-layers N            Generic commands per output token "
        "(default %llu)\n"
        "  --output-tokens N         Decode output tokens to simulate "
        "(default %llu)\n"
        "  --seq-len N               KV sequence length (default %llu)\n"
        "  --num-kv-heads N          KV heads (default %llu)\n"
        "  --head-dim N              Head dimension (default %llu)\n"
        "  --dtype-bytes N           Element bytes (default %llu)\n"
        "  --compute-latency-ns N    OracleGPU compute latency per command "
        "(default %llu)\n"
        "  --result-policy zero|pattern\n"
        "                            Output policy (default pattern)\n"
        "  --poll-timeout-ms N       Runtime completion timeout "
        "(default %u)\n"
        "  -h, --help                Show this help\n",
        prog,
        (unsigned long long)ORACLE_GPU_RUNTIME_DEFAULT_MMIO_BASE,
        (unsigned long long)ORACLE_GPU_RUNTIME_DEFAULT_MMIO_SIZE,
        (unsigned long long)DEFAULT_DESCRIPTOR_ADDR,
        (unsigned long long)DEFAULT_COMPLETION_ADDR,
        (unsigned long long)DEFAULT_Q_ADDR,
        (unsigned long long)DEFAULT_CXL_BASE,
        (unsigned long long)DEFAULT_CXL_SIZE,
        (unsigned long long)DEFAULT_BATCH_SIZE,
        (unsigned long long)DEFAULT_NUM_LAYERS,
        (unsigned long long)DEFAULT_OUTPUT_TOKENS,
        (unsigned long long)DEFAULT_SEQ_LEN,
        (unsigned long long)DEFAULT_NUM_KV_HEADS,
        (unsigned long long)DEFAULT_HEAD_DIM,
        (unsigned long long)DEFAULT_DTYPE_BYTES,
        (unsigned long long)DEFAULT_COMPUTE_LATENCY_NS,
        ORACLE_GPU_RUNTIME_DEFAULT_POLL_TIMEOUT_MS);
}

static uint64_t
parse_u64_arg(const char *name, const char *arg)
{
    char *end = NULL;
    uint64_t value;

    errno = 0;
    value = strtoull(arg, &end, 0);
    if (errno != 0 || end == arg || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, arg);
        exit(2);
    }

    return value;
}

static uint32_t
parse_u32_arg(const char *name, const char *arg)
{
    uint64_t value = parse_u64_arg(name, arg);

    if (value > UINT_MAX) {
        fprintf(stderr, "%s is too large for uint32_t: %" PRIu64 "\n",
                name, value);
        exit(2);
    }

    return (uint32_t)value;
}

static int
checked_align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t rounded;

    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return -1;
    }
    if (value > UINT64_MAX - (alignment - 1)) {
        return -1;
    }

    rounded = (value + alignment - 1) & ~(alignment - 1);
    *out = rounded;
    return 0;
}

static uint64_t
ceil_div_u64(uint64_t value, uint64_t divisor)
{
    if (value == 0) {
        return 0;
    }

    return ((value - 1) / divisor) + 1;
}

static int
checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) {
        return -1;
    }

    *out = a * b;
    return 0;
}

static int
checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) {
        return -1;
    }

    *out = a + b;
    return 0;
}

static size_t
checked_size(uint64_t value, const char *name)
{
    if (value > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "%s exceeds size_t: %" PRIu64 "\n", name, value);
        exit(2);
    }

    return (size_t)value;
}

static int
range_contains(uint64_t base, uint64_t size, uint64_t addr, uint64_t bytes)
{
    uint64_t end;
    uint64_t range_end;

    if (bytes == 0 || checked_add_u64(addr, bytes, &end) != 0 ||
        checked_add_u64(base, size, &range_end) != 0) {
        return 0;
    }

    return addr >= base && end <= range_end;
}

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
                    "output mismatch at byte %zu: expected %#x got %#x\n",
                    i, ORACLE_GPU_PATTERN_BYTE, buf[i]);
            return -1;
        }
    }

    return 0;
}

static int
check_zero_fill(const uint8_t *buf, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (buf[i] != 0) {
            fprintf(stderr, "output mismatch at byte %zu: expected 0 got %#x\n",
                    i, buf[i]);
            return -1;
        }
    }

    return 0;
}

static void
init_config(struct BaselineConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->mmio_base = ORACLE_GPU_RUNTIME_DEFAULT_MMIO_BASE;
    cfg->mmio_size = ORACLE_GPU_RUNTIME_DEFAULT_MMIO_SIZE;
    cfg->descriptor_phys = DEFAULT_DESCRIPTOR_ADDR;
    cfg->completion_phys = DEFAULT_COMPLETION_ADDR;
    cfg->q_phys = DEFAULT_Q_ADDR;
    cfg->cxl_base = DEFAULT_CXL_BASE;
    cfg->cxl_size = DEFAULT_CXL_SIZE;
    cfg->batch_size = DEFAULT_BATCH_SIZE;
    cfg->num_layers = DEFAULT_NUM_LAYERS;
    cfg->output_tokens = DEFAULT_OUTPUT_TOKENS;
    cfg->seq_len = DEFAULT_SEQ_LEN;
    cfg->num_kv_heads = DEFAULT_NUM_KV_HEADS;
    cfg->head_dim = DEFAULT_HEAD_DIM;
    cfg->dtype_bytes = DEFAULT_DTYPE_BYTES;
    cfg->compute_latency_ns = DEFAULT_COMPUTE_LATENCY_NS;
    cfg->result_policy = ORACLE_GPU_RESULT_PATTERN_FILL;
    cfg->poll_timeout_ms = ORACLE_GPU_RUNTIME_DEFAULT_POLL_TIMEOUT_MS;
}

static void
parse_args(int argc, char **argv, struct BaselineConfig *cfg)
{
    enum {
        OPT_MMIO_BASE = 1000,
        OPT_MMIO_SIZE,
        OPT_DESCRIPTOR_PHYS,
        OPT_COMPLETION_PHYS,
        OPT_Q_PHYS,
        OPT_OUTPUT_PHYS,
        OPT_CXL_BASE,
        OPT_CXL_SIZE,
        OPT_CXL_OFFSET,
        OPT_BATCH_SIZE,
        OPT_NUM_LAYERS,
        OPT_OUTPUT_TOKENS,
        OPT_SEQ_LEN,
        OPT_NUM_KV_HEADS,
        OPT_HEAD_DIM,
        OPT_DTYPE_BYTES,
        OPT_COMPUTE_LATENCY_NS,
        OPT_RESULT_POLICY,
        OPT_POLL_TIMEOUT_MS
    };
    static const struct option long_options[] = {
        {"mmio-base", required_argument, NULL, OPT_MMIO_BASE},
        {"mmio-size", required_argument, NULL, OPT_MMIO_SIZE},
        {"descriptor-phys", required_argument, NULL, OPT_DESCRIPTOR_PHYS},
        {"completion-phys", required_argument, NULL, OPT_COMPLETION_PHYS},
        {"q-phys", required_argument, NULL, OPT_Q_PHYS},
        {"output-phys", required_argument, NULL, OPT_OUTPUT_PHYS},
        {"cxl-base", required_argument, NULL, OPT_CXL_BASE},
        {"cxl-size", required_argument, NULL, OPT_CXL_SIZE},
        {"cxl-offset", required_argument, NULL, OPT_CXL_OFFSET},
        {"batch-size", required_argument, NULL, OPT_BATCH_SIZE},
        {"num-layers", required_argument, NULL, OPT_NUM_LAYERS},
        {"output-tokens", required_argument, NULL, OPT_OUTPUT_TOKENS},
        {"seq-len", required_argument, NULL, OPT_SEQ_LEN},
        {"num-kv-heads", required_argument, NULL, OPT_NUM_KV_HEADS},
        {"head-dim", required_argument, NULL, OPT_HEAD_DIM},
        {"dtype-bytes", required_argument, NULL, OPT_DTYPE_BYTES},
        {"compute-latency-ns", required_argument, NULL,
         OPT_COMPUTE_LATENCY_NS},
        {"result-policy", required_argument, NULL, OPT_RESULT_POLICY},
        {"poll-timeout-ms", required_argument, NULL, OPT_POLL_TIMEOUT_MS},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        switch (opt) {
          case OPT_MMIO_BASE:
            cfg->mmio_base = parse_u64_arg("mmio-base", optarg);
            break;
          case OPT_MMIO_SIZE:
            cfg->mmio_size = parse_u64_arg("mmio-size", optarg);
            break;
          case OPT_DESCRIPTOR_PHYS:
            cfg->descriptor_phys = parse_u64_arg("descriptor-phys", optarg);
            break;
          case OPT_COMPLETION_PHYS:
            cfg->completion_phys = parse_u64_arg("completion-phys", optarg);
            break;
          case OPT_Q_PHYS:
            cfg->q_phys = parse_u64_arg("q-phys", optarg);
            break;
          case OPT_OUTPUT_PHYS:
            cfg->output_phys = parse_u64_arg("output-phys", optarg);
            cfg->output_phys_set = 1;
            break;
          case OPT_CXL_BASE:
            cfg->cxl_base = parse_u64_arg("cxl-base", optarg);
            break;
          case OPT_CXL_SIZE:
            cfg->cxl_size = parse_u64_arg("cxl-size", optarg);
            break;
          case OPT_CXL_OFFSET:
            cfg->cxl_offset = parse_u64_arg("cxl-offset", optarg);
            break;
          case OPT_BATCH_SIZE:
            cfg->batch_size = parse_u64_arg("batch-size", optarg);
            break;
          case OPT_NUM_LAYERS:
            cfg->num_layers = parse_u64_arg("num-layers", optarg);
            break;
          case OPT_OUTPUT_TOKENS:
            cfg->output_tokens = parse_u64_arg("output-tokens", optarg);
            break;
          case OPT_SEQ_LEN:
            cfg->seq_len = parse_u64_arg("seq-len", optarg);
            break;
          case OPT_NUM_KV_HEADS:
            cfg->num_kv_heads = parse_u64_arg("num-kv-heads", optarg);
            break;
          case OPT_HEAD_DIM:
            cfg->head_dim = parse_u64_arg("head-dim", optarg);
            break;
          case OPT_DTYPE_BYTES:
            cfg->dtype_bytes = parse_u64_arg("dtype-bytes", optarg);
            break;
          case OPT_COMPUTE_LATENCY_NS:
            cfg->compute_latency_ns =
                parse_u64_arg("compute-latency-ns", optarg);
            break;
          case OPT_RESULT_POLICY:
            if (strcmp(optarg, "zero") == 0) {
                cfg->result_policy = ORACLE_GPU_RESULT_ZERO_FILL;
            } else if (strcmp(optarg, "pattern") == 0) {
                cfg->result_policy = ORACLE_GPU_RESULT_PATTERN_FILL;
            } else {
                fprintf(stderr, "invalid result-policy: %s\n", optarg);
                exit(2);
            }
            break;
          case OPT_POLL_TIMEOUT_MS:
            cfg->poll_timeout_ms = parse_u32_arg("poll-timeout-ms", optarg);
            break;
          case 'h':
            usage(argv[0]);
            exit(0);
          default:
            usage(argv[0]);
            exit(2);
        }
    }
}

static int
compute_buffer_sizes(const struct BaselineConfig *cfg, uint64_t *q_bytes,
                     uint64_t *k_bytes, uint64_t *v_bytes,
                     uint64_t *output_bytes)
{
    uint64_t token_vector_bytes;
    uint64_t kv_bytes;

    if (cfg->batch_size == 0 || cfg->num_layers == 0 ||
        cfg->output_tokens == 0 || cfg->seq_len == 0 ||
        cfg->num_kv_heads == 0 || cfg->head_dim == 0 ||
        cfg->dtype_bytes == 0) {
        fprintf(stderr, "batch/layer/token/shape parameters must be non-zero\n");
        return -1;
    }

    if (checked_mul_u64(cfg->batch_size, cfg->num_kv_heads,
                        &token_vector_bytes) != 0 ||
        checked_mul_u64(token_vector_bytes, cfg->head_dim,
                        &token_vector_bytes) != 0 ||
        checked_mul_u64(token_vector_bytes, cfg->dtype_bytes,
                        &token_vector_bytes) != 0) {
        fprintf(stderr, "Q/output byte calculation overflowed\n");
        return -1;
    }

    if (checked_mul_u64(token_vector_bytes, cfg->seq_len, &kv_bytes) != 0) {
        fprintf(stderr, "K/V byte calculation overflowed\n");
        return -1;
    }

    *q_bytes = token_vector_bytes;
    *output_bytes = token_vector_bytes;
    *k_bytes = kv_bytes;
    *v_bytes = kv_bytes;
    return 0;
}

int
main(int argc, char **argv)
{
    struct BaselineConfig cfg;
    struct OracleGpuRuntime rt;
    struct OracleGpuRuntimeConfig rt_cfg;
    struct OracleGpuMappedRegion q_region;
    struct OracleGpuMappedRegion output_region;
    struct OracleGpuInput inputs[3];
    struct OracleGpuSubmitResult result;
    uint64_t q_bytes;
    uint64_t k_bytes;
    uint64_t v_bytes;
    uint64_t output_bytes;
    uint64_t q_map_bytes;
    uint64_t output_map_bytes;
    uint64_t k_map_bytes;
    uint64_t v_map_bytes;
    uint64_t kv_total_map_bytes;
    uint64_t k_phys;
    uint64_t v_phys;
    uint64_t output_end;
    uint64_t command_count;
    uint64_t per_command_input_bytes;
    uint64_t total_q_read_bytes;
    uint64_t total_k_read_bytes;
    uint64_t total_v_read_bytes;
    uint64_t total_output_write_bytes;
    uint64_t total_payload_read_bytes;
    uint64_t expected_dma_read_with_desc;
    uint64_t expected_dma_write_with_completion;
    uint64_t expected_pcm_read_bytes;
    uint64_t expected_pcm_write_bytes;
    uint64_t expected_pcm_read_transactions;
    uint64_t expected_pcm_write_transactions;
    uint64_t k_plus_v_bytes;
    uint64_t command_idx;
    int rc = 1;

    init_config(&cfg);
    parse_args(argc, argv, &cfg);

    if (compute_buffer_sizes(&cfg, &q_bytes, &k_bytes, &v_bytes,
                             &output_bytes) != 0) {
        return 2;
    }

    if (checked_align_up_u64(q_bytes, 4096, &q_map_bytes) != 0 ||
        checked_align_up_u64(output_bytes, 4096, &output_map_bytes) != 0 ||
        checked_align_up_u64(k_bytes, PCM_MEDIA_GRANULARITY_BYTES,
                             &k_map_bytes) != 0 ||
        checked_align_up_u64(v_bytes, PCM_MEDIA_GRANULARITY_BYTES,
                             &v_map_bytes) != 0 ||
        checked_add_u64(k_map_bytes, v_map_bytes, &kv_total_map_bytes) != 0 ||
        checked_add_u64(k_bytes, v_bytes, &k_plus_v_bytes) != 0) {
        fprintf(stderr, "buffer size alignment calculation overflowed\n");
        return 2;
    }

    if (!cfg.output_phys_set) {
        if (checked_add_u64(cfg.q_phys, q_map_bytes, &cfg.output_phys) != 0) {
            fprintf(stderr, "default output physical address overflowed\n");
            return 2;
        }
        if (checked_align_up_u64(cfg.output_phys, 4096,
                                 &cfg.output_phys) != 0) {
            fprintf(stderr, "default output physical alignment overflowed\n");
            return 2;
        }
    }

    if (checked_add_u64(cfg.output_phys, output_map_bytes, &output_end) != 0) {
        fprintf(stderr, "output physical range overflowed\n");
        return 2;
    }

    if (checked_align_up_u64(cfg.cxl_offset, PCM_MEDIA_GRANULARITY_BYTES,
                             &cfg.cxl_offset) != 0 ||
        checked_add_u64(cfg.cxl_base, cfg.cxl_offset, &k_phys) != 0 ||
        checked_add_u64(k_phys, k_map_bytes, &v_phys) != 0) {
        fprintf(stderr, "CXL K/V physical address calculation overflowed\n");
        return 2;
    }

    if (cfg.cxl_offset > cfg.cxl_size ||
        kv_total_map_bytes > cfg.cxl_size - cfg.cxl_offset) {
        fprintf(stderr,
                "CXL-PCM range too small: need %#" PRIx64
                " bytes at offset %#" PRIx64 ", range size %#" PRIx64 "\n",
                kv_total_map_bytes, cfg.cxl_offset, cfg.cxl_size);
        return 2;
    }

    if (checked_mul_u64(cfg.num_layers, cfg.output_tokens, &command_count) !=
        0) {
        fprintf(stderr, "command count overflowed\n");
        return 2;
    }

    if (checked_add_u64(q_bytes, k_plus_v_bytes,
                        &per_command_input_bytes) != 0) {
        fprintf(stderr, "per-command input byte calculation overflowed\n");
        return 2;
    }

    if (checked_mul_u64(q_bytes, command_count, &total_q_read_bytes) != 0 ||
        checked_mul_u64(k_bytes, command_count, &total_k_read_bytes) != 0 ||
        checked_mul_u64(v_bytes, command_count, &total_v_read_bytes) != 0 ||
        checked_mul_u64(output_bytes, command_count,
                        &total_output_write_bytes) != 0 ||
        checked_mul_u64(per_command_input_bytes, command_count,
                        &total_payload_read_bytes) != 0 ||
        checked_mul_u64(sizeof(struct OracleGPUCommand), command_count,
                        &expected_dma_read_with_desc) != 0 ||
        checked_add_u64(expected_dma_read_with_desc, total_payload_read_bytes,
                        &expected_dma_read_with_desc) != 0 ||
        checked_mul_u64(sizeof(uint32_t), command_count,
                        &expected_dma_write_with_completion) != 0 ||
        checked_add_u64(expected_dma_write_with_completion,
                        total_output_write_bytes,
                        &expected_dma_write_with_completion) != 0) {
        fprintf(stderr, "expected traffic calculation overflowed\n");
        return 2;
    }

    if (checked_add_u64(total_k_read_bytes, total_v_read_bytes,
                        &expected_pcm_read_bytes) != 0) {
        fprintf(stderr, "expected CXL-PCM read byte calculation overflowed\n");
        return 2;
    }
    if (range_contains(cfg.cxl_base, cfg.cxl_size, cfg.q_phys, q_bytes)) {
        if (checked_add_u64(expected_pcm_read_bytes, total_q_read_bytes,
                            &expected_pcm_read_bytes) != 0) {
            fprintf(stderr,
                    "expected CXL-PCM read byte calculation overflowed\n");
            return 2;
        }
    }
    expected_pcm_write_bytes = 0;
    if (range_contains(cfg.cxl_base, cfg.cxl_size, cfg.output_phys,
                       output_bytes)) {
        expected_pcm_write_bytes += total_output_write_bytes;
    }
    expected_pcm_read_transactions =
        ceil_div_u64(expected_pcm_read_bytes, PCM_MEDIA_GRANULARITY_BYTES);
    expected_pcm_write_transactions =
        ceil_div_u64(expected_pcm_write_bytes, PCM_MEDIA_GRANULARITY_BYTES);

    memset(&rt, 0, sizeof(rt));
    memset(&rt_cfg, 0, sizeof(rt_cfg));
    memset(&q_region, 0, sizeof(q_region));
    memset(&output_region, 0, sizeof(output_region));
    memset(&result, 0, sizeof(result));

    rt_cfg.mmio_phys_base = cfg.mmio_base;
    rt_cfg.mmio_size = checked_size(cfg.mmio_size, "mmio-size");
    rt_cfg.descriptor_phys_addr = cfg.descriptor_phys;
    rt_cfg.descriptor_region_bytes = 0x1000;
    rt_cfg.completion_flag_phys_addr = cfg.completion_phys;
    rt_cfg.completion_region_bytes = 0x1000;
    rt_cfg.poll_timeout_ms = cfg.poll_timeout_ms;

    if (oracle_gpu_init(&rt, &rt_cfg) != 0) {
        fprintf(stderr, "oracle_gpu_init failed: %s\n",
                oracle_gpu_last_error(&rt));
        return 1;
    }

    if (oracle_gpu_map_region(&rt, cfg.q_phys, checked_size(q_map_bytes,
                              "Q map bytes"), &q_region) != 0 ||
        oracle_gpu_map_region(&rt, cfg.output_phys,
                              checked_size(output_map_bytes,
                                           "output map bytes"),
                              &output_region) != 0) {
        fprintf(stderr, "oracle_gpu_map_region failed: %s\n",
                oracle_gpu_last_error(&rt));
        goto cleanup;
    }

    if (!range_contains(cfg.cxl_base, cfg.cxl_size, k_phys,
                        kv_total_map_bytes)) {
        fprintf(stderr,
                "K/V allocation is outside CXL-PCM range: phys=%#" PRIx64
                " bytes=%#" PRIx64 " cxl=[%#" PRIx64 ", %#" PRIx64 ")\n",
                k_phys, kv_total_map_bytes, cfg.cxl_base,
                cfg.cxl_base + cfg.cxl_size);
        goto cleanup;
    }

    fill_buffer((uint8_t *)q_region.ptr, checked_size(q_bytes, "Q bytes"),
                0x41);
    memset(output_region.ptr, 0xcc, checked_size(output_bytes,
                                                "output bytes"));

    inputs[0].phys_addr = q_region.phys_addr;
    inputs[0].bytes = q_bytes;
    inputs[1].phys_addr = k_phys;
    inputs[1].bytes = k_bytes;
    inputs[2].phys_addr = v_phys;
    inputs[2].bytes = v_bytes;

    for (command_idx = 0; command_idx < command_count; ++command_idx) {
        if (oracle_gpu_submit_generic(&rt, inputs, 3, output_region.phys_addr,
                                      output_bytes, cfg.result_policy, 0, 0,
                                      cfg.compute_latency_ns,
                                      0x4b564f46464c4400ULL | command_idx,
                                      &result) != 0) {
            fprintf(stderr,
                    "oracle_gpu_submit_generic failed at command %" PRIu64
                    ": %s\n", command_idx, oracle_gpu_last_error(&rt));
            goto cleanup;
        }
    }

    if (cfg.result_policy == ORACLE_GPU_RESULT_PATTERN_FILL &&
        check_pattern_fill((const uint8_t *)output_region.ptr,
                           checked_size(output_bytes, "output bytes")) != 0) {
        goto cleanup;
    }
    if (cfg.result_policy == ORACLE_GPU_RESULT_ZERO_FILL &&
        check_zero_fill((const uint8_t *)output_region.ptr,
                        checked_size(output_bytes, "output bytes")) != 0) {
        goto cleanup;
    }

    printf("oracle_gpu_kv_offload_baseline\n");
    printf("batch_size: %" PRIu64 "\n", cfg.batch_size);
    printf("num_layers: %" PRIu64 "\n", cfg.num_layers);
    printf("output_tokens: %" PRIu64 "\n", cfg.output_tokens);
    printf("seq_len: %" PRIu64 "\n", cfg.seq_len);
    printf("num_kv_heads: %" PRIu64 "\n", cfg.num_kv_heads);
    printf("head_dim: %" PRIu64 "\n", cfg.head_dim);
    printf("dtype_bytes: %" PRIu64 "\n", cfg.dtype_bytes);
    printf("compute_latency_ns: %" PRIu64 "\n", cfg.compute_latency_ns);
    printf("result_policy: %s\n",
           cfg.result_policy == ORACLE_GPU_RESULT_ZERO_FILL ?
           "ZERO_FILL" : "PATTERN_FILL");
    printf("command_count: %" PRIu64 "\n", command_count);
    printf("cxl_pcm_base: %#" PRIx64 "\n", cfg.cxl_base);
    printf("cxl_pcm_size: %#" PRIx64 "\n", cfg.cxl_size);
    printf("q_phys: %#" PRIx64 "\n", q_region.phys_addr);
    printf("k_cache_phys: %#" PRIx64 "\n", k_phys);
    printf("v_cache_phys: %#" PRIx64 "\n", v_phys);
    printf("output_phys: %#" PRIx64 "\n", output_region.phys_addr);
    printf("k_cache_in_cxl_pcm: %s\n",
           range_contains(cfg.cxl_base, cfg.cxl_size, k_phys,
                          k_bytes) ? "yes" : "no");
    printf("v_cache_in_cxl_pcm: %s\n",
           range_contains(cfg.cxl_base, cfg.cxl_size, v_phys,
                          v_bytes) ? "yes" : "no");
    printf("q_bytes: %" PRIu64 "\n", q_bytes);
    printf("K_bytes: %" PRIu64 "\n", k_bytes);
    printf("V_bytes: %" PRIu64 "\n", v_bytes);
    printf("K_plus_V_bytes: %" PRIu64 "\n", k_plus_v_bytes);
    printf("output_bytes: %" PRIu64 "\n", output_bytes);
    printf("expected_Q_read_bytes_total: %" PRIu64 "\n",
           total_q_read_bytes);
    printf("expected_K_read_bytes_total: %" PRIu64 "\n",
           total_k_read_bytes);
    printf("expected_V_read_bytes_total: %" PRIu64 "\n",
           total_v_read_bytes);
    printf("expected_payload_dma_read_bytes: %" PRIu64 "\n",
           total_payload_read_bytes);
    printf("expected_oraclegpu_dma_read_bytes_including_descriptors: %"
           PRIu64 "\n", expected_dma_read_with_desc);
    printf("expected_output_write_bytes_total: %" PRIu64 "\n",
           total_output_write_bytes);
    printf("expected_oraclegpu_dma_write_bytes_including_completion: %"
           PRIu64 "\n", expected_dma_write_with_completion);
    printf("expected_cxl_pcm_read_bytes: %" PRIu64 "\n",
           expected_pcm_read_bytes);
    printf("expected_cxl_pcm_write_bytes: %" PRIu64 "\n",
           expected_pcm_write_bytes);
    printf("expected_pcm_read_transactions_128B: %" PRIu64 "\n",
           expected_pcm_read_transactions);
    printf("expected_pcm_write_transactions_128B: %" PRIu64 "\n",
           expected_pcm_write_transactions);
    printf("runtime_completion_flag: %" PRIu32 "\n",
           result.completion_flag_value);
    printf("runtime_status: %#" PRIx64 "\n", result.status);
    printf("runtime_dma_read_bytes: %" PRIu64 "\n", result.dma_read_bytes);
    printf("runtime_dma_write_bytes: %" PRIu64 "\n", result.dma_write_bytes);
    printf("OracleGPU KV offload baseline passed\n");

    rc = 0;

cleanup:
    oracle_gpu_unmap_region(&output_region);
    oracle_gpu_unmap_region(&q_region);
    oracle_gpu_close(&rt);
    (void)output_end;
    return rc;
}
