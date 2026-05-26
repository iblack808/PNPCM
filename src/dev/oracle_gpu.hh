#ifndef __DEV_ORACLE_GPU_HH__
#define __DEV_ORACLE_GPU_HH__

#include <vector>

#include "base/statistics.hh"
#include "dev/dma_device.hh"
#include "dev/oracle_gpu_protocol.h"
#include "params/OracleGPU.hh"
#include "sim/eventq.hh"

namespace gem5
{

class OracleGPU : public DmaDevice
{
  private:
    static constexpr ByteOrder byteOrder = ByteOrder::little;

    enum RegisterOffset : Addr
    {
        REG_DESC_ADDR_LO = 0x00,
        REG_DESC_ADDR_HI = 0x08,
        REG_DOORBELL = 0x10,
        REG_STATUS = 0x18,
        REG_COMMAND_COUNT = 0x20,
        REG_COMPLETED_COUNT = 0x28,
        REG_DMA_READ_BYTES = 0x30,
        REG_DMA_WRITE_BYTES = 0x38,
        REG_GENERIC_COMMAND_COUNT = 0x40,
        REG_INVALID_COMMAND_COUNT = 0x48,
    };

    enum StatusBits : uint64_t
    {
        STATUS_BUSY = 1ULL << 0,
        STATUS_DONE = 1ULL << 1,
        STATUS_ERROR = 1ULL << 2,
    };

    using CommandDescriptor = OracleGPUCommand;
    using InputSegment = OracleGPUInputSegment;

    static constexpr uint64_t inputDmaChunkBytes = 64;

    struct OracleGPUStats : public statistics::Group
    {
        explicit OracleGPUStats(statistics::Group *parent);

        statistics::Scalar commandCount;
        statistics::Scalar genericCommandCount;
        statistics::Scalar dmaReadBytes;
        statistics::Scalar dmaWriteBytes;
        statistics::Scalar completedCount;
        statistics::Scalar invalidCommandCount;
        statistics::Scalar lastComputeLatencyTicks;
        statistics::Scalar lastComputeStartTick;
        statistics::Scalar lastComputeDoneTick;
        statistics::Scalar lastComputeObservedTicks;
    } stats;

    const Addr pioAddr;
    const Addr pioSize;
    const Tick pioDelay;
    const uint32_t maxTransferBytes;

    uint64_t descAddr;
    uint64_t statusReg;
    uint64_t errorCode;

    std::vector<uint8_t> descBuffer;
    std::vector<uint8_t> inputBuffer;
    std::vector<uint8_t> outputBuffer;
    CommandDescriptor activeCmd;
    uint32_t completionValue;
    uint32_t currentInputIndex;
    uint64_t currentInputOffset;
    uint64_t currentInputChunkBytes;
    Tick currentComputeStartTick;

    EventFunctionWrapper descReadDoneEvent;
    EventFunctionWrapper inputReadDoneEvent;
    EventFunctionWrapper computeDoneEvent;
    EventFunctionWrapper oracleResultReadDoneEvent;
    EventFunctionWrapper payloadWriteDoneEvent;
    EventFunctionWrapper completionWriteDoneEvent;

    void startCommand();
    void finishDescRead();
    void finishInputRead();
    void finishCompute();
    void finishOracleResultRead();
    void finishPayloadWrite();
    void finishCompletionWrite();
    void startNextInputRead();
    void startCurrentInputChunkRead();
    void startResultWrite();
    bool validateCommand();
    const InputSegment &currentInput() const;

    void setError(const std::string &reason);
    void clearCommandState();
    bool busy() const { return statusReg & STATUS_BUSY; }

  public:
    using Params = OracleGPUParams;
    explicit OracleGPU(const Params &p);

    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
};

} // namespace gem5

#endif // __DEV_ORACLE_GPU_HH__
