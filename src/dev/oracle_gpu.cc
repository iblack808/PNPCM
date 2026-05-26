#include "dev/oracle_gpu.hh"

#include <algorithm>
#include <cstring>

#include "base/bitfield.hh"
#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/OracleGPU.hh"
#include "mem/packet_access.hh"
#include "sim/core.hh"

namespace gem5
{

OracleGPU::OracleGPUStats::OracleGPUStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(commandCount, statistics::units::Count::get(),
               "Number of submitted OracleGPU commands"),
      ADD_STAT(genericCommandCount, statistics::units::Count::get(),
               "Number of submitted generic OracleGPU commands"),
      ADD_STAT(dmaReadBytes, statistics::units::Byte::get(),
               "Total bytes read through the DMA port"),
      ADD_STAT(dmaWriteBytes, statistics::units::Byte::get(),
               "Total bytes written through the DMA port"),
      ADD_STAT(completedCount, statistics::units::Count::get(),
               "Number of completed OracleGPU commands"),
      ADD_STAT(invalidCommandCount, statistics::units::Count::get(),
               "Number of invalid OracleGPU commands"),
      ADD_STAT(lastComputeLatencyTicks, statistics::units::Tick::get(),
               "Requested compute delay in ticks for the last command"),
      ADD_STAT(lastComputeStartTick, statistics::units::Tick::get(),
               "Tick when the last command entered compute delay"),
      ADD_STAT(lastComputeDoneTick, statistics::units::Tick::get(),
               "Tick when the last command finished compute delay"),
      ADD_STAT(lastComputeObservedTicks, statistics::units::Tick::get(),
               "Observed compute delay ticks for the last command")
{
}

OracleGPU::OracleGPU(const Params &p)
    : DmaDevice(p),
      stats(this),
      pioAddr(p.pio_addr),
      pioSize(p.pio_size),
      pioDelay(p.pio_latency),
      maxTransferBytes(p.max_transfer_bytes),
      descAddr(0),
      statusReg(0),
      errorCode(0),
      descBuffer(sizeof(CommandDescriptor), 0),
      completionValue(1),
      currentInputIndex(0),
      currentInputOffset(0),
      currentInputChunkBytes(0),
      currentComputeStartTick(0),
      descReadDoneEvent([this] { finishDescRead(); }, name()),
      inputReadDoneEvent([this] { finishInputRead(); }, name()),
      computeDoneEvent([this] { finishCompute(); }, name()),
      oracleResultReadDoneEvent([this] { finishOracleResultRead(); }, name()),
      payloadWriteDoneEvent([this] { finishPayloadWrite(); }, name()),
      completionWriteDoneEvent([this] { finishCompletionWrite(); }, name())
{
    clearCommandState();
}

void
OracleGPU::clearCommandState()
{
    std::memset(&activeCmd, 0, sizeof(activeCmd));
    inputBuffer.clear();
    outputBuffer.clear();
    currentInputIndex = 0;
    currentInputOffset = 0;
    currentInputChunkBytes = 0;
    currentComputeStartTick = 0;
}

void
OracleGPU::setError(const std::string &reason)
{
    statusReg &= ~(STATUS_BUSY | STATUS_DONE);
    statusReg |= STATUS_ERROR;
    errorCode++;
    stats.invalidCommandCount++;
    clearCommandState();
    warn("%s: %s", name().c_str(), reason.c_str());
    DPRINTF(OracleGPU, "Error: %s\n", reason.c_str());
}

AddrRangeList
OracleGPU::getAddrRanges() const
{
    return {RangeSize(pioAddr, pioSize)};
}

Tick
OracleGPU::read(PacketPtr pkt)
{
    const Addr offset = pkt->getAddr() - pioAddr;
    uint64_t value = 0;

    switch (offset) {
      case REG_DESC_ADDR_LO:
        value = bits(descAddr, 31, 0);
        break;
      case REG_DESC_ADDR_HI:
        value = bits(descAddr, 63, 32);
        break;
      case REG_STATUS:
        value = statusReg |
            (errorCode ? static_cast<uint64_t>(STATUS_ERROR) : 0ULL);
        break;
      case REG_COMMAND_COUNT:
        value = stats.commandCount.value();
        break;
      case REG_COMPLETED_COUNT:
        value = stats.completedCount.value();
        break;
      case REG_DMA_READ_BYTES:
        value = stats.dmaReadBytes.value();
        break;
      case REG_DMA_WRITE_BYTES:
        value = stats.dmaWriteBytes.value();
        break;
      case REG_GENERIC_COMMAND_COUNT:
        value = stats.genericCommandCount.value();
        break;
      case REG_INVALID_COMMAND_COUNT:
        value = stats.invalidCommandCount.value();
        break;
      default:
        value = 0;
        break;
    }

    pkt->setUintX(value, byteOrder);
    pkt->makeResponse();
    return pioDelay;
}

Tick
OracleGPU::write(PacketPtr pkt)
{
    const Addr offset = pkt->getAddr() - pioAddr;
    const uint64_t value = pkt->getUintX(byteOrder);

    switch (offset) {
      case REG_DESC_ADDR_LO:
        descAddr = insertBits(descAddr, 31, 0, value);
        break;
      case REG_DESC_ADDR_HI:
        descAddr = insertBits(descAddr, 63, 32, value);
        break;
      case REG_DOORBELL:
        if (value != 0) {
            if (busy()) {
                setError("doorbell received while device is busy");
            } else if (descAddr == 0) {
                setError("doorbell received with null descriptor address");
            } else {
                startCommand();
            }
        }
        break;
      default:
        break;
    }

    pkt->makeResponse();
    return pioDelay;
}

void
OracleGPU::startCommand()
{
    clearCommandState();
    errorCode = 0;
    statusReg = STATUS_BUSY;
    stats.commandCount++;

    DPRINTF(OracleGPU, "Starting command fetch from descriptor %#llx\n",
            static_cast<unsigned long long>(descAddr));

    dmaRead(descAddr, sizeof(CommandDescriptor), &descReadDoneEvent,
            descBuffer.data(), 0);
    stats.dmaReadBytes += sizeof(CommandDescriptor);
}

void
OracleGPU::finishDescRead()
{
    std::memcpy(&activeCmd, descBuffer.data(), sizeof(activeCmd));
    DPRINTF(OracleGPU,
            "Descriptor magic=%#x version=%u op_type=%u num_inputs=%u "
            "result_policy=%u dst=%#llx dst_bytes=%llu completion=%#llx "
            "compute_latency_ns=%llu user_tag=%llu\n",
            activeCmd.magic,
            activeCmd.version,
            activeCmd.op_type,
            activeCmd.num_inputs,
            activeCmd.result_policy,
            static_cast<unsigned long long>(activeCmd.dst_addr),
            static_cast<unsigned long long>(activeCmd.dst_bytes),
            static_cast<unsigned long long>(activeCmd.completion_flag_addr),
            static_cast<unsigned long long>(activeCmd.compute_latency_ns),
            static_cast<unsigned long long>(activeCmd.user_tag));

    if (!validateCommand()) {
        return;
    }

    stats.genericCommandCount++;

    for (uint32_t i = 0; i < activeCmd.num_inputs; ++i) {
        const auto &input = activeCmd.inputs[i];
        DPRINTF(OracleGPU,
                "Input[%u] addr=%#llx bytes=%llu user_tag=%llu\n",
                i,
                static_cast<unsigned long long>(input.addr),
                static_cast<unsigned long long>(input.bytes),
                static_cast<unsigned long long>(activeCmd.user_tag));
    }

    startNextInputRead();
}

void
OracleGPU::startNextInputRead()
{
    if (currentInputIndex >= activeCmd.num_inputs) {
        const Tick delay = sim_clock::as_int::ns * activeCmd.compute_latency_ns;
        currentComputeStartTick = curTick();
        stats.lastComputeLatencyTicks = delay;
        stats.lastComputeStartTick = currentComputeStartTick;
        DPRINTF(OracleGPU,
                "All input DMA reads complete, scheduling compute delay of "
                "%llu ns (%llu ticks) for user_tag=%llu\n",
                static_cast<unsigned long long>(activeCmd.compute_latency_ns),
                static_cast<unsigned long long>(delay),
                static_cast<unsigned long long>(activeCmd.user_tag));
        schedule(computeDoneEvent, currentComputeStartTick + delay);
        return;
    }

    const auto &input = currentInput();
    inputBuffer.resize(input.bytes, 0);
    currentInputOffset = 0;
    currentInputChunkBytes = 0;
    DPRINTF(OracleGPU,
            "Starting DMA read for input[%u] addr=%#llx bytes=%llu "
            "chunk_bytes=%llu user_tag=%llu\n",
            currentInputIndex,
            static_cast<unsigned long long>(input.addr),
            static_cast<unsigned long long>(input.bytes),
            static_cast<unsigned long long>(inputDmaChunkBytes),
            static_cast<unsigned long long>(activeCmd.user_tag));
    startCurrentInputChunkRead();
}

void
OracleGPU::startCurrentInputChunkRead()
{
    const auto &input = currentInput();
    currentInputChunkBytes =
        std::min<uint64_t>(inputDmaChunkBytes, input.bytes - currentInputOffset);
    const Addr chunk_addr = input.addr + currentInputOffset;

    DPRINTF(OracleGPU,
            "Starting DMA read chunk for input[%u] addr=%#llx offset=%llu "
            "bytes=%llu user_tag=%llu\n",
            currentInputIndex,
            static_cast<unsigned long long>(chunk_addr),
            static_cast<unsigned long long>(currentInputOffset),
            static_cast<unsigned long long>(currentInputChunkBytes),
            static_cast<unsigned long long>(activeCmd.user_tag));

    dmaRead(chunk_addr, currentInputChunkBytes, &inputReadDoneEvent,
            inputBuffer.data() + currentInputOffset, 0);
    stats.dmaReadBytes += currentInputChunkBytes;
}

void
OracleGPU::finishInputRead()
{
    DPRINTF(OracleGPU,
            "DMA read chunk complete for input[%u] offset=%llu bytes=%llu "
            "user_tag=%llu\n",
            currentInputIndex,
            static_cast<unsigned long long>(currentInputOffset),
            static_cast<unsigned long long>(currentInputChunkBytes),
            static_cast<unsigned long long>(activeCmd.user_tag));

    currentInputOffset += currentInputChunkBytes;
    if (currentInputOffset < currentInput().bytes) {
        startCurrentInputChunkRead();
        return;
    }

    DPRINTF(OracleGPU,
            "DMA read complete for input[%u] total_bytes=%llu "
            "user_tag=%llu\n",
            currentInputIndex,
            static_cast<unsigned long long>(currentInput().bytes),
            static_cast<unsigned long long>(activeCmd.user_tag));

    currentInputIndex++;
    currentInputOffset = 0;
    currentInputChunkBytes = 0;
    startNextInputRead();
}

void
OracleGPU::finishCompute()
{
    stats.lastComputeDoneTick = curTick();
    stats.lastComputeObservedTicks = curTick() - currentComputeStartTick;

    DPRINTF(OracleGPU,
            "Compute delay complete for user_tag=%llu, preparing result "
            "policy=%u dst=%#llx bytes=%llu observed_compute_ticks=%llu\n",
            static_cast<unsigned long long>(activeCmd.user_tag),
            activeCmd.result_policy,
            static_cast<unsigned long long>(activeCmd.dst_addr),
            static_cast<unsigned long long>(activeCmd.dst_bytes),
            static_cast<unsigned long long>(
                stats.lastComputeObservedTicks.value()));

    outputBuffer.resize(activeCmd.dst_bytes, 0);

    switch (activeCmd.result_policy) {
      case ORACLE_GPU_RESULT_ZERO_FILL:
        std::fill(outputBuffer.begin(), outputBuffer.end(), 0);
        startResultWrite();
        break;
      case ORACLE_GPU_RESULT_PATTERN_FILL:
        std::fill(outputBuffer.begin(), outputBuffer.end(),
                  ORACLE_GPU_PATTERN_BYTE);
        startResultWrite();
        break;
      case ORACLE_GPU_RESULT_COPY_ORACLE:
        dmaRead(activeCmd.oracle_result_addr, activeCmd.oracle_result_bytes,
                &oracleResultReadDoneEvent, outputBuffer.data(), 0);
        stats.dmaReadBytes += activeCmd.oracle_result_bytes;
        break;
      default:
        setError(csprintf("unsupported result policy %u",
                 activeCmd.result_policy));
        break;
    }
}

void
OracleGPU::finishOracleResultRead()
{
    DPRINTF(OracleGPU,
            "Oracle result DMA read complete for user_tag=%llu bytes=%llu\n",
            static_cast<unsigned long long>(activeCmd.user_tag),
            static_cast<unsigned long long>(activeCmd.oracle_result_bytes));
    startResultWrite();
}

void
OracleGPU::startResultWrite()
{
    DPRINTF(OracleGPU,
            "Starting DMA write to %#llx for %llu bytes user_tag=%llu\n",
            static_cast<unsigned long long>(activeCmd.dst_addr),
            static_cast<unsigned long long>(activeCmd.dst_bytes),
            static_cast<unsigned long long>(activeCmd.user_tag));

    dmaWrite(activeCmd.dst_addr, activeCmd.dst_bytes, &payloadWriteDoneEvent,
             outputBuffer.data(), 0);
    stats.dmaWriteBytes += activeCmd.dst_bytes;
}

void
OracleGPU::finishPayloadWrite()
{
    DPRINTF(OracleGPU,
            "Result DMA write complete, setting completion flag for "
            "user_tag=%llu\n",
            static_cast<unsigned long long>(activeCmd.user_tag));

    dmaWrite(activeCmd.completion_flag_addr, sizeof(completionValue),
             &completionWriteDoneEvent,
             reinterpret_cast<uint8_t *>(&completionValue), 0);
    stats.dmaWriteBytes += sizeof(completionValue);
}

void
OracleGPU::finishCompletionWrite()
{
    statusReg &= ~STATUS_BUSY;
    statusReg |= STATUS_DONE;
    stats.completedCount++;

    DPRINTF(OracleGPU,
            "Command complete for user_tag=%llu, total completed=%llu\n",
            static_cast<unsigned long long>(activeCmd.user_tag),
            static_cast<unsigned long long>(stats.completedCount.value()));

    clearCommandState();
}

const OracleGPU::InputSegment &
OracleGPU::currentInput() const
{
    return activeCmd.inputs[currentInputIndex];
}

bool
OracleGPU::validateCommand()
{
    if (activeCmd.magic != ORACLE_GPU_CMD_MAGIC) {
        setError(csprintf("invalid command magic %#x", activeCmd.magic));
        return false;
    }
    if (activeCmd.version != ORACLE_GPU_CMD_VERSION) {
        setError(csprintf("unsupported command version %u", activeCmd.version));
        return false;
    }
    if (activeCmd.op_type != ORACLE_GPU_OP_GENERIC) {
        setError(csprintf("unsupported op_type %u", activeCmd.op_type));
        return false;
    }
    if (activeCmd.num_inputs == 0 ||
        activeCmd.num_inputs > ORACLE_GPU_MAX_INPUTS) {
        setError(csprintf("invalid num_inputs %u", activeCmd.num_inputs));
        return false;
    }
    if (activeCmd.dst_addr == 0 || activeCmd.dst_bytes == 0 ||
        activeCmd.completion_flag_addr == 0) {
        setError("descriptor contains null or zero-sized output/completion");
        return false;
    }
    if (activeCmd.dst_bytes > maxTransferBytes) {
        setError(csprintf("dst_bytes %llu exceed max_transfer_bytes %u",
                 static_cast<unsigned long long>(activeCmd.dst_bytes),
                 maxTransferBytes));
        return false;
    }

    switch (activeCmd.result_policy) {
      case ORACLE_GPU_RESULT_ZERO_FILL:
      case ORACLE_GPU_RESULT_PATTERN_FILL:
        break;
      case ORACLE_GPU_RESULT_COPY_ORACLE:
        if (activeCmd.oracle_result_addr == 0 ||
            activeCmd.oracle_result_bytes == 0) {
            setError("COPY_ORACLE requires a non-null oracle_result buffer");
            return false;
        }
        if (activeCmd.oracle_result_bytes != activeCmd.dst_bytes) {
            setError(csprintf(
                "oracle_result_bytes %llu must match dst_bytes %llu",
                static_cast<unsigned long long>(activeCmd.oracle_result_bytes),
                static_cast<unsigned long long>(activeCmd.dst_bytes)));
            return false;
        }
        if (activeCmd.oracle_result_bytes > maxTransferBytes) {
            setError(csprintf(
                "oracle_result_bytes %llu exceed max_transfer_bytes %u",
                static_cast<unsigned long long>(
                    activeCmd.oracle_result_bytes),
                maxTransferBytes));
            return false;
        }
        break;
      default:
        setError(csprintf("unsupported result_policy %u",
                 activeCmd.result_policy));
        return false;
    }

    for (uint32_t i = 0; i < activeCmd.num_inputs; ++i) {
        const auto &input = activeCmd.inputs[i];
        if (input.addr == 0 || input.bytes == 0) {
            setError(csprintf("input[%u] contains null address or zero bytes",
                     i));
            return false;
        }
        if (input.bytes > maxTransferBytes) {
            setError(csprintf("input[%u] bytes %llu exceed max_transfer_bytes "
                     "%u",
                     i,
                     static_cast<unsigned long long>(input.bytes),
                     maxTransferBytes));
            return false;
        }
    }

    return true;
}

} // namespace gem5
