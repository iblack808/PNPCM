#include "dev/oracle_gpu.hh"

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
      ADD_STAT(copyCommandCount, statistics::units::Count::get(),
               "Number of submitted OracleGPU copy commands"),
      ADD_STAT(decodeAttentionCommandCount, statistics::units::Count::get(),
               "Number of submitted OracleGPU decode-attention commands"),
      ADD_STAT(dmaReadBytes, statistics::units::Byte::get(),
               "Total bytes read through the DMA port"),
      ADD_STAT(dmaWriteBytes, statistics::units::Byte::get(),
               "Total bytes written through the DMA port"),
      ADD_STAT(qReadBytes, statistics::units::Byte::get(),
               "Total Q bytes read by decode-attention commands"),
      ADD_STAT(kCacheReadBytes, statistics::units::Byte::get(),
               "Total K-cache bytes read by decode-attention commands"),
      ADD_STAT(vCacheReadBytes, statistics::units::Byte::get(),
               "Total V-cache bytes read by decode-attention commands"),
      ADD_STAT(outputWriteBytes, statistics::units::Byte::get(),
               "Total output bytes written by OracleGPU commands"),
      ADD_STAT(completionWriteBytes, statistics::units::Byte::get(),
               "Total completion-flag bytes written by OracleGPU commands"),
      ADD_STAT(completedCount, statistics::units::Count::get(),
               "Number of completed OracleGPU commands")
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
      descReadDoneEvent([this] { finishDescRead(); }, name()),
      payloadReadDoneEvent([this] { finishPayloadRead(); }, name()),
      qReadDoneEvent([this] { finishQRead(); }, name()),
      kCacheReadDoneEvent([this] { finishKCacheRead(); }, name()),
      vCacheReadDoneEvent([this] { finishVCacheRead(); }, name()),
      computeDoneEvent([this] { finishCompute(); }, name()),
      payloadWriteDoneEvent([this] { finishPayloadWrite(); }, name()),
      completionWriteDoneEvent([this] { finishCompletionWrite(); }, name())
{
    clearCommandState();
}

void
OracleGPU::clearCommandState()
{
    std::memset(&activeCmd, 0, sizeof(activeCmd));
    dmaBuffer.clear();
    outputBuffer.clear();
}

void
OracleGPU::setError(const std::string &reason)
{
    statusReg &= ~(STATUS_BUSY | STATUS_DONE);
    statusReg |= STATUS_ERROR;
    errorCode++;
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
OracleGPU::startCopyPayloadRead()
{
    const auto &copy = activeCmd.copy;

    stats.copyCommandCount++;
    dmaBuffer.resize(copy.bytes, 0);
    dmaRead(copy.src_addr, copy.bytes, &payloadReadDoneEvent, dmaBuffer.data(), 0);
    stats.dmaReadBytes += copy.bytes;

    DPRINTF(OracleGPU, "Issuing copy DMA read src=%#llx bytes=%llu\n",
            static_cast<unsigned long long>(copy.src_addr),
            static_cast<unsigned long long>(copy.bytes));
}

void
OracleGPU::startDecodeAttentionQRead()
{
    const auto &decode = activeCmd.decode_attention;

    stats.decodeAttentionCommandCount++;
    dmaBuffer.resize(decode.q_bytes, 0);
    dmaRead(decode.q_addr, decode.q_bytes, &qReadDoneEvent, dmaBuffer.data(), 0);
    stats.dmaReadBytes += decode.q_bytes;
    stats.qReadBytes += decode.q_bytes;

    DPRINTF(OracleGPU,
            "Issuing decode-attention Q DMA read addr=%#llx bytes=%llu\n",
            static_cast<unsigned long long>(decode.q_addr),
            static_cast<unsigned long long>(decode.q_bytes));
}

bool
OracleGPU::validateCopyCommand() const
{
    const auto &copy = activeCmd.copy;

    if (copy.bytes == 0) {
        return false;
    }
    if (copy.bytes > maxTransferBytes) {
        return false;
    }
    if (copy.src_addr == 0 || copy.dst_addr == 0 ||
        activeCmd.completion_flag_addr == 0) {
        return false;
    }

    return true;
}

bool
OracleGPU::validateDecodeAttentionCommand() const
{
    const auto &decode = activeCmd.decode_attention;

    if (decode.q_addr == 0 || decode.k_cache_addr == 0 ||
        decode.v_cache_addr == 0 || decode.out_addr == 0 ||
        activeCmd.completion_flag_addr == 0) {
        return false;
    }
    if (decode.q_bytes == 0 || decode.k_cache_bytes == 0 ||
        decode.v_cache_bytes == 0 || decode.out_bytes == 0) {
        return false;
    }
    if (decode.q_bytes > maxTransferBytes ||
        decode.k_cache_bytes > maxTransferBytes ||
        decode.v_cache_bytes > maxTransferBytes ||
        decode.out_bytes > maxTransferBytes) {
        return false;
    }
    if (decode.batch_size == 0 || decode.seq_len == 0 ||
        decode.num_q_heads == 0 || decode.num_kv_heads == 0 ||
        decode.head_dim == 0 || decode.dtype_bytes == 0) {
        return false;
    }

    return true;
}

OracleGPU::Opcode
OracleGPU::activeOpcode() const
{
    return static_cast<Opcode>(activeCmd.op);
}

void
OracleGPU::finishDescRead()
{
    std::memcpy(&activeCmd, descBuffer.data(), sizeof(activeCmd));
    switch (activeOpcode()) {
      case Opcode::Copy:
      {
        const auto &copy = activeCmd.copy;
        DPRINTF(OracleGPU,
                "Copy descriptor src=%#llx dst=%#llx bytes=%llu "
                "completion=%#llx compute_latency_ns=%llu\n",
                static_cast<unsigned long long>(copy.src_addr),
                static_cast<unsigned long long>(copy.dst_addr),
                static_cast<unsigned long long>(copy.bytes),
                static_cast<unsigned long long>(activeCmd.completion_flag_addr),
                static_cast<unsigned long long>(activeCmd.compute_latency_ns));

        if (!validateCopyCommand()) {
            if (copy.bytes > maxTransferBytes) {
                setError(csprintf(
                    "copy bytes %llu exceed max_transfer_bytes %u",
                    static_cast<unsigned long long>(copy.bytes),
                    maxTransferBytes));
            } else {
                setError("copy descriptor is invalid");
            }
            return;
        }

        startCopyPayloadRead();
        break;
      }
      case Opcode::DecodeAttention:
      {
        const auto &decode = activeCmd.decode_attention;
        DPRINTF(OracleGPU,
                "Decode-attention descriptor q=(%#llx,%llu) "
                "k=(%#llx,%llu) v=(%#llx,%llu) out=(%#llx,%llu) "
                "batch=%u seq=%u q_heads=%u kv_heads=%u head_dim=%u "
                "dtype=%u layer=%u request=%u completion=%#llx "
                "compute_latency_ns=%llu\n",
                static_cast<unsigned long long>(decode.q_addr),
                static_cast<unsigned long long>(decode.q_bytes),
                static_cast<unsigned long long>(decode.k_cache_addr),
                static_cast<unsigned long long>(decode.k_cache_bytes),
                static_cast<unsigned long long>(decode.v_cache_addr),
                static_cast<unsigned long long>(decode.v_cache_bytes),
                static_cast<unsigned long long>(decode.out_addr),
                static_cast<unsigned long long>(decode.out_bytes),
                decode.batch_size, decode.seq_len, decode.num_q_heads,
                decode.num_kv_heads, decode.head_dim, decode.dtype_bytes,
                decode.layer_id, decode.request_id,
                static_cast<unsigned long long>(activeCmd.completion_flag_addr),
                static_cast<unsigned long long>(activeCmd.compute_latency_ns));

        if (!validateDecodeAttentionCommand()) {
            const bool oversize =
                decode.q_bytes > maxTransferBytes ||
                decode.k_cache_bytes > maxTransferBytes ||
                decode.v_cache_bytes > maxTransferBytes ||
                decode.out_bytes > maxTransferBytes;
            if (oversize) {
                setError(csprintf(
                    "decode-attention transfer exceeds max_transfer_bytes %u",
                    maxTransferBytes));
            } else {
                setError("decode-attention descriptor is invalid");
            }
            return;
        }

        startDecodeAttentionQRead();
        break;
      }
      default:
        setError(csprintf("unsupported opcode %u", activeCmd.op));
        break;
    }
}

void
OracleGPU::finishPayloadRead()
{
    DPRINTF(OracleGPU, "Copy payload DMA read complete for %llu bytes\n",
            static_cast<unsigned long long>(activeCmd.copy.bytes));

    const Tick delay = sim_clock::as_int::ns * activeCmd.compute_latency_ns;
    schedule(computeDoneEvent, curTick() + delay);
}

void
OracleGPU::finishQRead()
{
    const auto &decode = activeCmd.decode_attention;

    DPRINTF(OracleGPU,
            "Decode-attention Q DMA read complete, reading K cache "
            "addr=%#llx bytes=%llu\n",
            static_cast<unsigned long long>(decode.k_cache_addr),
            static_cast<unsigned long long>(decode.k_cache_bytes));

    dmaBuffer.resize(decode.k_cache_bytes, 0);
    dmaRead(decode.k_cache_addr, decode.k_cache_bytes, &kCacheReadDoneEvent,
            dmaBuffer.data(), 0);
    stats.dmaReadBytes += decode.k_cache_bytes;
    stats.kCacheReadBytes += decode.k_cache_bytes;
}

void
OracleGPU::finishKCacheRead()
{
    const auto &decode = activeCmd.decode_attention;

    DPRINTF(OracleGPU,
            "Decode-attention K-cache DMA read complete, reading V cache "
            "addr=%#llx bytes=%llu\n",
            static_cast<unsigned long long>(decode.v_cache_addr),
            static_cast<unsigned long long>(decode.v_cache_bytes));

    dmaBuffer.resize(decode.v_cache_bytes, 0);
    dmaRead(decode.v_cache_addr, decode.v_cache_bytes, &vCacheReadDoneEvent,
            dmaBuffer.data(), 0);
    stats.dmaReadBytes += decode.v_cache_bytes;
    stats.vCacheReadBytes += decode.v_cache_bytes;
}

void
OracleGPU::finishVCacheRead()
{
    const auto &decode = activeCmd.decode_attention;

    DPRINTF(OracleGPU,
            "Decode-attention V-cache DMA read complete for %llu bytes\n",
            static_cast<unsigned long long>(decode.v_cache_bytes));

    const Tick delay = sim_clock::as_int::ns * activeCmd.compute_latency_ns;
    schedule(computeDoneEvent, curTick() + delay);
}

void
OracleGPU::fillDecodeAttentionOutput()
{
    const auto &decode = activeCmd.decode_attention;

    outputBuffer.resize(decode.out_bytes, 0);
    for (uint64_t i = 0; i < decode.out_bytes; ++i) {
        outputBuffer[i] = static_cast<uint8_t>(
            (decode.layer_id + decode.request_id + i) & 0xff);
    }
}

void
OracleGPU::finishCompute()
{
    switch (activeOpcode()) {
      case Opcode::Copy:
        DPRINTF(OracleGPU, "Compute delay complete, writing copy payload to "
                "%#llx\n",
                static_cast<unsigned long long>(activeCmd.copy.dst_addr));

        dmaWrite(activeCmd.copy.dst_addr, activeCmd.copy.bytes,
                 &payloadWriteDoneEvent, dmaBuffer.data(), 0);
        stats.dmaWriteBytes += activeCmd.copy.bytes;
        stats.outputWriteBytes += activeCmd.copy.bytes;
        break;
      case Opcode::DecodeAttention:
        fillDecodeAttentionOutput();
        DPRINTF(OracleGPU,
                "Compute delay complete, writing decode-attention output to "
                "%#llx (%llu bytes)\n",
                static_cast<unsigned long long>(
                    activeCmd.decode_attention.out_addr),
                static_cast<unsigned long long>(
                    activeCmd.decode_attention.out_bytes));

        dmaWrite(activeCmd.decode_attention.out_addr,
                 activeCmd.decode_attention.out_bytes,
                 &payloadWriteDoneEvent, outputBuffer.data(), 0);
        stats.dmaWriteBytes += activeCmd.decode_attention.out_bytes;
        stats.outputWriteBytes += activeCmd.decode_attention.out_bytes;
        break;
      default:
        setError("compute completed with unsupported opcode");
        break;
    }
}

void
OracleGPU::finishPayloadWrite()
{
    DPRINTF(OracleGPU, "Payload DMA write complete, setting completion flag\n");

    dmaWrite(activeCmd.completion_flag_addr, sizeof(completionValue),
             &completionWriteDoneEvent,
             reinterpret_cast<uint8_t *>(&completionValue), 0);
    stats.dmaWriteBytes += sizeof(completionValue);
    stats.completionWriteBytes += sizeof(completionValue);
}

void
OracleGPU::finishCompletionWrite()
{
    statusReg &= ~STATUS_BUSY;
    statusReg |= STATUS_DONE;
    stats.completedCount++;

    DPRINTF(OracleGPU, "Command complete, total completed=%llu\n",
            static_cast<unsigned long long>(stats.completedCount.value()));

    clearCommandState();
}

} // namespace gem5
