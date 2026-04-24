#ifndef __DEV_X86_CXL_TYPE1_RAO_ACCEL_HH__
#define __DEV_X86_CXL_TYPE1_RAO_ACCEL_HH__

#include <deque>
#include <unordered_map>
#include <vector>

#include "base/addr_range.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "dev/pci/device.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CXLType1RAOAccel.hh"

namespace gem5
{

class CXLType1RAOAccel : public PciDevice
{
  private:
    enum RegisterOffset : Addr
    {
        REG_CTRL = 0x00,
        REG_STATUS = 0x08,
        REG_MAX_OPS = 0x10,
        REG_TRACE_LEN = 0x18,
        REG_COMPLETED_OPS = 0x20,
        REG_STAGE_SEQ_ID = 0x28,
        REG_STAGE_GROUP_ID = 0x30,
        REG_STAGE_STEP_ID = 0x38,
        REG_STAGE_PADDR = 0x40,
        REG_STAGE_OPCODE = 0x48,
        REG_STAGE_OPERAND_MODE = 0x50,
        REG_STAGE_OPERAND_IMM = 0x58,
        REG_STAGE_RESULT_ID = 0x60,
        REG_PUSH_TRACE = 0x68,
    };

    enum CtrlBits : uint64_t
    {
        CTRL_START = 1ULL << 0,
        CTRL_RESET = 1ULL << 1,
    };

    enum StatusBits : uint64_t
    {
        STATUS_BUSY = 1ULL << 0,
        STATUS_DONE = 1ULL << 1,
        STATUS_ERROR = 1ULL << 2,
    };

    enum Opcode : uint32_t
    {
        OpcodeFetch = 1,
        OpcodeFetchAdd = 2,
        OpcodeAdd = 3,
    };

    enum OperandMode : uint32_t
    {
        OperandNone = 0,
        OperandImm = 1,
        OperandResultRef = 2,
    };

    enum OpState : uint32_t
    {
        OpIdle = 0,
        OpReading = 1,
        OpWriting = 2,
        OpDone = 3,
    };

    struct TraceEntry
    {
        uint64_t seq_id = 0;
        uint64_t group_id = 0;
        uint64_t step_id = 0;
        Addr paddr = 0;
        uint32_t opcode = 0;
        uint32_t operand_mode = 0;
        uint64_t operand_imm = 0;
        int64_t result_id = -1;
    };

    struct StagingEntry
    {
        uint64_t seq_id = 0;
        uint64_t group_id = 0;
        uint64_t step_id = 0;
        Addr paddr = 0;
        uint32_t opcode = 0;
        uint32_t operand_mode = 0;
        uint64_t operand_imm = 0;
        int64_t result_id = -1;
    };

    struct ActiveContext
    {
        size_t trace_index = 0;
        uint64_t old_value = 0;
        uint64_t new_value = 0;
        uint32_t state = OpIdle;
    };

    class TimingDevicePort : public RequestPort
    {
      public:
        TimingDevicePort(const std::string &name, CXLType1RAOAccel *device)
            : RequestPort(name), device(device),
              retryRespEvent([this] { sendRetryResp(); }, name)
        {}

      protected:
        CXLType1RAOAccel *device;
        EventFunctionWrapper retryRespEvent;
    };

    class DcachePort : public TimingDevicePort
    {
      public:
        explicit DcachePort(CXLType1RAOAccel *device)
            : TimingDevicePort(device->name() + ".dcache_port", device)
        {}

      protected:
        void recvTimingSnoopReq(PacketPtr pkt) override;
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        bool isSnooping() const override { return true; }
    };

    class IcachePort : public TimingDevicePort
    {
      public:
        explicit IcachePort(CXLType1RAOAccel *device)
            : TimingDevicePort(device->name() + ".icache_port", device)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    class TraceSenderState : public Packet::SenderState
    {
      public:
        explicit TraceSenderState(size_t trace_index, bool read_pkt)
            : index(trace_index), is_read(read_pkt)
        {}

        size_t index;
        bool is_read;
    };

    const unsigned int cacheLineSize;
    const unsigned int maxOps;

    DcachePort dcachePort;
    IcachePort icachePort;
    PacketPtr dcache_pkt;

    std::vector<TraceEntry> traceEntries;
    std::unordered_map<uint64_t, uint64_t> resultTable;
    StagingEntry stagingEntry;
    ActiveContext activeContext;

    uint64_t ctrlReg;
    uint64_t statusReg;
    uint64_t completedOps;
    uint64_t errorCode;
    Tick firstIssueTick;
    Tick finishTick;

    EventFunctionWrapper issueNextEvent;

    void resetState();
    void startExecution();
    void issueNext();
    void issueRead(size_t trace_index);
    void issueWrite(size_t trace_index, uint64_t value);
    void recvData(PacketPtr pkt);
    void handleReadResponse(size_t trace_index, PacketPtr pkt);
    void handleWriteResponse(size_t trace_index);
    uint64_t resolveOperand(const TraceEntry &entry) const;
    bool traceReady() const;
    void finishExecution();
    Addr barOffset(Addr addr) const;

    PacketPtr buildPacket(const RequestPtr &req, bool read);
    void sendData(const RequestPtr &req, uint8_t *data, bool read,
                  size_t trace_index);
    bool handleReadPacket(PacketPtr pkt);
    bool handleWritePacket();

    struct RAOStats : public statistics::Group
    {
        explicit RAOStats(CXLType1RAOAccel &device);

        statistics::Scalar numFetch;
        statistics::Scalar numFetchAdd;
        statistics::Scalar numAdd;
        statistics::Scalar numReads;
        statistics::Scalar numWrites;
        statistics::Scalar numTotalOps;
        statistics::Scalar totalExecTicks;
        statistics::Scalar totalOpLatency;
        statistics::Formula avgOpLatency;
    };

    RAOStats stats;

  public:
    using Params = CXLType1RAOAccelParams;
    explicit CXLType1RAOAccel(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
};

} // namespace gem5

#endif // __DEV_X86_CXL_TYPE1_RAO_ACCEL_HH__
