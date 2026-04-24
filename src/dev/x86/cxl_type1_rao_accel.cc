#include "dev/x86/cxl_type1_rao_accel.hh"

#include <cstring>

#include "debug/CXLType1RAOAccel.hh"

namespace gem5
{

CXLType1RAOAccel::RAOStats::RAOStats(CXLType1RAOAccel &device)
    : statistics::Group(&device),
      ADD_STAT(numFetch, statistics::units::Count::get(),
               "Number of FETCH operations"),
      ADD_STAT(numFetchAdd, statistics::units::Count::get(),
               "Number of FETCH_ADD operations"),
      ADD_STAT(numAdd, statistics::units::Count::get(),
               "Number of ADD operations"),
      ADD_STAT(numReads, statistics::units::Count::get(),
               "Number of read requests issued by RAO"),
      ADD_STAT(numWrites, statistics::units::Count::get(),
               "Number of write requests issued by RAO"),
      ADD_STAT(numTotalOps, statistics::units::Count::get(),
               "Number of completed RAO operations"),
      ADD_STAT(totalExecTicks, statistics::units::Tick::get(),
               "Total RAO execution ticks"),
      ADD_STAT(totalOpLatency, statistics::units::Tick::get(),
               "Accumulated latency across completed operations"),
      ADD_STAT(avgOpLatency, statistics::units::Rate<
                  statistics::units::Tick,
                  statistics::units::Count>::get(),
               "Average operation latency")
{
    avgOpLatency = totalOpLatency / numTotalOps;
}

CXLType1RAOAccel::CXLType1RAOAccel(const Params &p)
    : PciDevice(p),
      cacheLineSize(p.cacheline_size),
      maxOps(p.max_ops),
      dcachePort(this),
      icachePort(this),
      dcache_pkt(nullptr),
      ctrlReg(0),
      statusReg(0),
      completedOps(0),
      errorCode(0),
      firstIssueTick(0),
      finishTick(0),
      issueNextEvent([this] { issueNext(); }, name()),
      stats(*this)
{
    traceEntries.reserve(maxOps);
    resetState();
}

void
CXLType1RAOAccel::resetState()
{
    ctrlReg = 0;
    statusReg = 0;
    completedOps = 0;
    errorCode = 0;
    firstIssueTick = 0;
    finishTick = 0;
    dcache_pkt = nullptr;
    traceEntries.clear();
    resultTable.clear();
    stagingEntry = StagingEntry();
    activeContext = ActiveContext();
}

Addr
CXLType1RAOAccel::barOffset(Addr addr) const
{
    assert(BARs[0]);
    return addr - BARs[0]->addr();
}

Port &
CXLType1RAOAccel::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma") {
        return dmaPort;
    } else if (if_name == "dcache_port") {
        return dcachePort;
    } else if (if_name == "icache_port") {
        return icachePort;
    }
    return PioDevice::getPort(if_name, idx);
}

AddrRangeList
CXLType1RAOAccel::getAddrRanges() const
{
    return PciDevice::getAddrRanges();
}

Tick
CXLType1RAOAccel::read(PacketPtr pkt)
{
    const Addr offset = barOffset(pkt->getAddr());
    uint64_t value = 0;

    switch (offset) {
      case REG_CTRL:
        value = ctrlReg;
        break;
      case REG_STATUS:
        value = statusReg |
            (errorCode ? static_cast<uint64_t>(STATUS_ERROR) : 0ULL);
        break;
      case REG_MAX_OPS:
        value = maxOps;
        break;
      case REG_TRACE_LEN:
        value = traceEntries.size();
        break;
      case REG_COMPLETED_OPS:
        value = completedOps;
        break;
      case REG_STAGE_SEQ_ID:
        value = stagingEntry.seq_id;
        break;
      case REG_STAGE_GROUP_ID:
        value = stagingEntry.group_id;
        break;
      case REG_STAGE_STEP_ID:
        value = stagingEntry.step_id;
        break;
      case REG_STAGE_PADDR:
        value = stagingEntry.paddr;
        break;
      case REG_STAGE_OPCODE:
        value = stagingEntry.opcode;
        break;
      case REG_STAGE_OPERAND_MODE:
        value = stagingEntry.operand_mode;
        break;
      case REG_STAGE_OPERAND_IMM:
        value = stagingEntry.operand_imm;
        break;
      case REG_STAGE_RESULT_ID:
        value = static_cast<uint64_t>(stagingEntry.result_id);
        break;
      default:
        value = 0;
        break;
    }

    pkt->setUintX(value, ByteOrder::little);
    pkt->makeResponse();
    return pioDelay;
}

Tick
CXLType1RAOAccel::write(PacketPtr pkt)
{
    const Addr offset = barOffset(pkt->getAddr());
    const uint64_t value = pkt->getUintX(ByteOrder::little);

    switch (offset) {
      case REG_CTRL:
        ctrlReg = value;
        if (value & CTRL_RESET) {
            resetState();
        }
        if (value & CTRL_START) {
            startExecution();
        }
        break;
      case REG_STAGE_SEQ_ID:
        stagingEntry.seq_id = value;
        break;
      case REG_STAGE_GROUP_ID:
        stagingEntry.group_id = value;
        break;
      case REG_STAGE_STEP_ID:
        stagingEntry.step_id = value;
        break;
      case REG_STAGE_PADDR:
        stagingEntry.paddr = value;
        break;
      case REG_STAGE_OPCODE:
        stagingEntry.opcode = value;
        break;
      case REG_STAGE_OPERAND_MODE:
        stagingEntry.operand_mode = value;
        break;
      case REG_STAGE_OPERAND_IMM:
        stagingEntry.operand_imm = value;
        break;
      case REG_STAGE_RESULT_ID:
        stagingEntry.result_id = static_cast<int64_t>(value);
        break;
      case REG_PUSH_TRACE:
        if (traceEntries.size() >= maxOps || statusReg & STATUS_BUSY) {
            errorCode = 1;
        } else {
            traceEntries.push_back({
                stagingEntry.seq_id,
                stagingEntry.group_id,
                stagingEntry.step_id,
                stagingEntry.paddr,
                stagingEntry.opcode,
                stagingEntry.operand_mode,
                stagingEntry.operand_imm,
                stagingEntry.result_id,
            });
            stagingEntry = StagingEntry();
        }
        break;
      default:
        break;
    }

    pkt->makeResponse();
    return pioDelay;
}

bool
CXLType1RAOAccel::traceReady() const
{
    return !traceEntries.empty() && errorCode == 0;
}

void
CXLType1RAOAccel::startExecution()
{
    if (!traceReady() || (statusReg & STATUS_BUSY)) {
        if (!traceReady()) {
            errorCode = 2;
        }
        return;
    }

    completedOps = 0;
    resultTable.clear();
    activeContext = ActiveContext();
    statusReg = STATUS_BUSY;
    firstIssueTick = 0;
    finishTick = 0;

    if (!issueNextEvent.scheduled()) {
        schedule(issueNextEvent, clockEdge(Cycles(1)));
    }
}

void
CXLType1RAOAccel::finishExecution()
{
    statusReg &= ~STATUS_BUSY;
    statusReg |= STATUS_DONE;
    finishTick = clockEdge();
    if (firstIssueTick != 0 && finishTick >= firstIssueTick) {
        stats.totalExecTicks = finishTick - firstIssueTick;
    }
}

void
CXLType1RAOAccel::issueNext()
{
    if (!(statusReg & STATUS_BUSY)) {
        return;
    }

    if (activeContext.state != OpIdle) {
        return;
    }

    if (completedOps >= traceEntries.size()) {
        finishExecution();
        return;
    }

    activeContext.trace_index = completedOps;
    activeContext.old_value = 0;
    activeContext.new_value = 0;
    activeContext.state = OpReading;
    if (firstIssueTick == 0) {
        firstIssueTick = clockEdge();
    }
    issueRead(activeContext.trace_index);
}

void
CXLType1RAOAccel::issueRead(size_t trace_index)
{
    const TraceEntry &entry = traceEntries.at(trace_index);
    auto req = std::make_shared<Request>(entry.paddr, sizeof(uint64_t), 0, 0);
    auto *data = new uint8_t[sizeof(uint64_t)];
    std::memset(data, 0, sizeof(uint64_t));
    sendData(req, data, true, trace_index);
    stats.numReads++;
}

void
CXLType1RAOAccel::issueWrite(size_t trace_index, uint64_t value)
{
    const TraceEntry &entry = traceEntries.at(trace_index);
    auto req = std::make_shared<Request>(entry.paddr, sizeof(uint64_t), 0, 0);
    auto *data = new uint8_t[sizeof(uint64_t)];
    std::memcpy(data, &value, sizeof(uint64_t));
    sendData(req, data, false, trace_index);
    stats.numWrites++;
}

uint64_t
CXLType1RAOAccel::resolveOperand(const TraceEntry &entry) const
{
    switch (entry.operand_mode) {
      case OperandNone:
        return 0;
      case OperandImm:
        return entry.operand_imm;
      case OperandResultRef: {
        auto it = resultTable.find(entry.result_id);
        if (it == resultTable.end()) {
            panic("Missing operand dependency for result_id=%d",
                  entry.result_id);
        }
        return it->second;
      }
      default:
        panic("Unsupported operand mode %u", entry.operand_mode);
    }
}

void
CXLType1RAOAccel::handleReadResponse(size_t trace_index, PacketPtr pkt)
{
    assert(trace_index < traceEntries.size());
    const TraceEntry &entry = traceEntries.at(trace_index);

    uint64_t old_value = 0;
    pkt->writeData(reinterpret_cast<uint8_t *>(&old_value));
    activeContext.old_value = old_value;

    switch (entry.opcode) {
      case OpcodeFetch:
        resultTable[entry.seq_id] = old_value;
        activeContext.state = OpDone;
        completedOps++;
        stats.numFetch++;
        stats.numTotalOps++;
        stats.totalOpLatency += clockEdge() - firstIssueTick;
        activeContext = ActiveContext();
        schedule(issueNextEvent, nextCycle());
        break;
      case OpcodeFetchAdd:
        activeContext.new_value = old_value + 1;
        activeContext.state = OpWriting;
        stats.numFetchAdd++;
        issueWrite(trace_index, activeContext.new_value);
        break;
      case OpcodeAdd:
        activeContext.new_value = old_value + resolveOperand(entry);
        activeContext.state = OpWriting;
        stats.numAdd++;
        issueWrite(trace_index, activeContext.new_value);
        break;
      default:
        panic("Unsupported opcode %u", entry.opcode);
    }
}

void
CXLType1RAOAccel::handleWriteResponse(size_t trace_index)
{
    const TraceEntry &entry = traceEntries.at(trace_index);
    resultTable[entry.seq_id] = activeContext.old_value;
    completedOps++;
    stats.numTotalOps++;
    stats.totalOpLatency += clockEdge() - firstIssueTick;
    activeContext = ActiveContext();
    schedule(issueNextEvent, nextCycle());
}

void
CXLType1RAOAccel::recvData(PacketPtr pkt)
{
    auto *state = dynamic_cast<TraceSenderState *>(pkt->senderState);
    assert(state);

    if (state->is_read) {
        handleReadResponse(state->index, pkt);
    } else {
        handleWriteResponse(state->index);
    }

    delete state;
    pkt->senderState = nullptr;
}

PacketPtr
CXLType1RAOAccel::buildPacket(const RequestPtr &req, bool read)
{
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}

void
CXLType1RAOAccel::sendData(const RequestPtr &req, uint8_t *data, bool read,
                           size_t trace_index)
{
    PacketPtr pkt = buildPacket(req, read);
    pkt->dataDynamic<uint8_t>(data);
    pkt->senderState = new TraceSenderState(trace_index, read);

    if (read) {
        handleReadPacket(pkt);
    } else {
        dcache_pkt = pkt;
        handleWritePacket();
    }
}

bool
CXLType1RAOAccel::handleReadPacket(PacketPtr pkt)
{
    if (!dcachePort.sendTimingReq(pkt)) {
        dcache_pkt = pkt;
        return false;
    }
    dcache_pkt = nullptr;
    return true;
}

bool
CXLType1RAOAccel::handleWritePacket()
{
    if (!dcachePort.sendTimingReq(dcache_pkt)) {
        return false;
    }
    dcache_pkt = nullptr;
    return true;
}

void
CXLType1RAOAccel::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CXLType1RAOAccel,
            "%s received snoop for addr=%#x cmd=%s\n",
            __func__, pkt->getAddr(), pkt->cmdString());
}

bool
CXLType1RAOAccel::DcachePort::recvTimingResp(PacketPtr pkt)
{
    device->recvData(pkt);
    delete pkt;
    return true;
}

void
CXLType1RAOAccel::DcachePort::recvReqRetry()
{
    assert(device->dcache_pkt != nullptr);
    PacketPtr pkt = device->dcache_pkt;
    if (sendTimingReq(pkt)) {
        device->dcache_pkt = nullptr;
    }
}

bool
CXLType1RAOAccel::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1RAOAccel, "Received icache response %#x\n",
            pkt->getAddr());
    return true;
}

void
CXLType1RAOAccel::IcachePort::recvReqRetry()
{
    DPRINTF(CXLType1RAOAccel, "Received icache retry\n");
}

} // namespace gem5
