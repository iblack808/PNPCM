#ifndef ORACLE_GPU_USER_ABI_H
#define ORACLE_GPU_USER_ABI_H

/*
 * Guest-side OracleGPU runtime ABI shim.
 *
 * The device-owned ABI lives in src/dev/oracle_gpu_protocol.h.
 * This wrapper intentionally includes that header, or an installed copy of the
 * same file, so the guest runtime and the gem5 device model share one source
 * of truth for the command layout and MMIO register definitions.
 */

#if defined(__has_include)
#if __has_include("oracle_gpu_protocol.h")
#include "oracle_gpu_protocol.h"
#else
#include "dev/oracle_gpu_protocol.h"
#endif
#else
#include "dev/oracle_gpu_protocol.h"
#endif

#endif
