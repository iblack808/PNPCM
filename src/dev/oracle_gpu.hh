#ifndef __DEV_ORACLE_GPU_HH__
#define __DEV_ORACLE_GPU_HH__

#include <vector>

#include "base/statistics.hh"
#include "dev/dma_device.hh"
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
    };

    enum StatusBits : uint64_t
    {
        STATUS_BUSY = 1ULL << 0,
        STATUS_DONE = 1ULL << 1,
        STATUS_ERROR = 1ULL << 2,
    };

    struct CommandDescriptor
    {
        uint64_t src_addr;
        uint64_t dst_addr;
        uint64_t bytes;
        uint64_t completion_flag_addr;
        uint64_t compute_latency_ns;
    };

    static_assert(sizeof(CommandDescriptor) == 40,
        "OracleGPU command descriptor size must remain stable");

    struct OracleGPUStats : public statistics::Group
    {
        explicit OracleGPUStats(statistics::Group *parent);

        statistics::Scalar commandCount;
        statistics::Scalar dmaReadBytes;
        statistics::Scalar dmaWriteBytes;
        statistics::Scalar completedCount;
    } stats;

    const Addr pioAddr;
    const Addr pioSize;
    const Tick pioDelay;
    const uint32_t maxTransferBytes;

    uint64_t descAddr;
    uint64_t statusReg;
    uint64_t errorCode;

    std::vector<uint8_t> descBuffer;
    std::vector<uint8_t> payloadBuffer;
    CommandDescriptor activeCmd;
    uint32_t completionValue;

    EventFunctionWrapper descReadDoneEvent;
    EventFunctionWrapper payloadReadDoneEvent;
    EventFunctionWrapper computeDoneEvent;
    EventFunctionWrapper payloadWriteDoneEvent;
    EventFunctionWrapper completionWriteDoneEvent;

    void startCommand();
    void finishDescRead();
    void finishPayloadRead();
    void finishCompute();
    void finishPayloadWrite();
    void finishCompletionWrite();

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
