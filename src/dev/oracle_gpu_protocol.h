#ifndef __DEV_ORACLE_GPU_PROTOCOL_H__
#define __DEV_ORACLE_GPU_PROTOCOL_H__

#include <stdint.h>

#define ORACLE_GPU_CMD_MAGIC 0x4f475055u
#define ORACLE_GPU_CMD_VERSION 1u

#define ORACLE_GPU_MAX_INPUTS 8u

#define ORACLE_GPU_OP_GENERIC 1u

#define ORACLE_GPU_RESULT_ZERO_FILL 1u
#define ORACLE_GPU_RESULT_PATTERN_FILL 2u
#define ORACLE_GPU_RESULT_COPY_ORACLE 3u
#define ORACLE_GPU_RESULT_KV_SIZE_PATTERN 4u

#define ORACLE_GPU_PATTERN_BYTE 0xa5u

#define ORACLE_GPU_REG_DESC_ADDR_LO 0x00u
#define ORACLE_GPU_REG_DESC_ADDR_HI 0x08u
#define ORACLE_GPU_REG_DOORBELL 0x10u
#define ORACLE_GPU_REG_STATUS 0x18u
#define ORACLE_GPU_REG_COMMAND_COUNT 0x20u
#define ORACLE_GPU_REG_COMPLETED_COUNT 0x28u
#define ORACLE_GPU_REG_DMA_READ_BYTES 0x30u
#define ORACLE_GPU_REG_DMA_WRITE_BYTES 0x38u
#define ORACLE_GPU_REG_GENERIC_COMMAND_COUNT 0x40u
#define ORACLE_GPU_REG_INVALID_COMMAND_COUNT 0x48u

#define ORACLE_GPU_STATUS_BUSY (1ull << 0)
#define ORACLE_GPU_STATUS_DONE (1ull << 1)
#define ORACLE_GPU_STATUS_ERROR (1ull << 2)

struct OracleGPUInputSegment
{
    uint64_t addr;
    uint64_t bytes;
};

struct OracleGPUCommand
{
    uint32_t magic;
    uint16_t version;
    uint16_t op_type;
    uint32_t num_inputs;
    uint32_t result_policy;
    uint64_t dst_addr;
    uint64_t dst_bytes;
    uint64_t oracle_result_addr;
    uint64_t oracle_result_bytes;
    uint64_t compute_latency_ns;
    uint64_t completion_flag_addr;
    uint64_t user_tag;
    struct OracleGPUInputSegment inputs[ORACLE_GPU_MAX_INPUTS];
};

#ifdef __cplusplus
static_assert(sizeof(OracleGPUInputSegment) == 16,
    "OracleGPUInputSegment layout must remain stable");
static_assert(sizeof(OracleGPUCommand) == 200,
    "OracleGPUCommand layout must remain stable");
#else
_Static_assert(sizeof(struct OracleGPUInputSegment) == 16,
    "OracleGPUInputSegment layout must remain stable");
_Static_assert(sizeof(struct OracleGPUCommand) == 200,
    "OracleGPUCommand layout must remain stable");
#endif

#endif // __DEV_ORACLE_GPU_PROTOCOL_H__
