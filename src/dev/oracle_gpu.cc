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
      ADD_STAT(dmaReadBytes, statistics::units::Byte::get(),
               "Total bytes read through the DMA port"),
      ADD_STAT(dmaWriteBytes, statistics::units::Byte::get(),
               "Total bytes written through the DMA port"),
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
    payloadBuffer.clear();
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
OracleGPU::finishDescRead()
{
    std::memcpy(&activeCmd, descBuffer.data(), sizeof(activeCmd));
    DPRINTF(OracleGPU,
            "Descriptor src=%#llx dst=%#llx bytes=%llu completion=%#llx "
            "compute_latency_ns=%llu\n",
            static_cast<unsigned long long>(activeCmd.src_addr),
            static_cast<unsigned long long>(activeCmd.dst_addr),
            static_cast<unsigned long long>(activeCmd.bytes),
            static_cast<unsigned long long>(activeCmd.completion_flag_addr),
            static_cast<unsigned long long>(activeCmd.compute_latency_ns));

    if (activeCmd.bytes == 0) {
        setError("descriptor requested zero-byte transfer");
        return;
    }
    if (activeCmd.bytes > maxTransferBytes) {
        setError(csprintf("descriptor bytes %llu exceed max_transfer_bytes %u",
                 static_cast<unsigned long long>(activeCmd.bytes),
                 maxTransferBytes));
        return;
    }
    if (activeCmd.src_addr == 0 || activeCmd.dst_addr == 0 ||
        activeCmd.completion_flag_addr == 0) {
        setError("descriptor contains null address");
        return;
    }

    payloadBuffer.resize(activeCmd.bytes, 0);
    dmaRead(activeCmd.src_addr, activeCmd.bytes, &payloadReadDoneEvent,
            payloadBuffer.data(), 0);
    stats.dmaReadBytes += activeCmd.bytes;
}

void
OracleGPU::finishPayloadRead()
{
    DPRINTF(OracleGPU, "Payload DMA read complete for %llu bytes\n",
            static_cast<unsigned long long>(activeCmd.bytes));

    const Tick delay = sim_clock::as_int::ns * activeCmd.compute_latency_ns;
    schedule(computeDoneEvent, curTick() + delay);
}

void
OracleGPU::finishCompute()
{
    DPRINTF(OracleGPU, "Compute delay complete, writing payload to %#llx\n",
            static_cast<unsigned long long>(activeCmd.dst_addr));

    dmaWrite(activeCmd.dst_addr, activeCmd.bytes, &payloadWriteDoneEvent,
             payloadBuffer.data(), 0);
    stats.dmaWriteBytes += activeCmd.bytes;
}

void
OracleGPU::finishPayloadWrite()
{
    DPRINTF(OracleGPU, "Payload DMA write complete, setting completion flag\n");

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

    DPRINTF(OracleGPU, "Command complete, total completed=%llu\n",
            static_cast<unsigned long long>(stats.completedCount.value()));

    clearCommandState();
}

} // namespace gem5
