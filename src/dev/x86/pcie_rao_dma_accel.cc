#include "dev/x86/pcie_rao_dma_accel.hh"

#include <cstring>

#include "debug/PCIeRAODMAAccel.hh"

namespace gem5
{

PCIeRAODMAAccel::RAOStats::RAOStats(PCIeRAODMAAccel &device)
    : statistics::Group(&device),
      ADD_STAT(numFetch, statistics::units::Count::get(),
               "Number of FETCH operations"),
      ADD_STAT(numFetchAdd, statistics::units::Count::get(),
               "Number of FETCH_ADD operations"),
      ADD_STAT(numAdd, statistics::units::Count::get(),
               "Number of ADD operations"),
      ADD_STAT(numReads, statistics::units::Count::get(),
               "Number of DMA read requests issued by RAO"),
      ADD_STAT(numWrites, statistics::units::Count::get(),
               "Number of DMA write requests issued by RAO"),
      ADD_STAT(numTotalOps, statistics::units::Count::get(),
               "Number of completed RAO operations"),
      ADD_STAT(totalExecTicks, statistics::units::Tick::get(),
               "Total RAO DMA execution ticks"),
      ADD_STAT(totalReadLatency, statistics::units::Tick::get(),
               "Accumulated read request service latency"),
      ADD_STAT(totalWriteLatency, statistics::units::Tick::get(),
               "Accumulated write request service latency"),
      ADD_STAT(totalComputeTicks, statistics::units::Tick::get(),
               "Accumulated RAO compute ticks between read and write"),
      ADD_STAT(totalOpLatency, statistics::units::Tick::get(),
               "Accumulated per-operation service latency"),
      ADD_STAT(avgReadLatency, statistics::units::Rate<
                  statistics::units::Tick,
                  statistics::units::Count>::get(),
               "Average read request service latency"),
      ADD_STAT(avgWriteLatency, statistics::units::Rate<
                  statistics::units::Tick,
                  statistics::units::Count>::get(),
               "Average write request service latency"),
      ADD_STAT(avgComputeTicks, statistics::units::Rate<
                  statistics::units::Tick,
                  statistics::units::Count>::get(),
               "Average RAO compute ticks per write operation"),
      ADD_STAT(avgOpLatency, statistics::units::Rate<
                  statistics::units::Tick,
                  statistics::units::Count>::get(),
               "Average per-operation service latency")
{
    avgReadLatency = totalReadLatency / numReads;
    avgWriteLatency = totalWriteLatency / numWrites;
    avgComputeTicks = totalComputeTicks / numWrites;
    avgOpLatency = totalOpLatency / numTotalOps;
}

PCIeRAODMAAccel::PCIeRAODMAAccel(const Params &p)
    : PciDevice(p),
      cacheLineSize(p.cacheline_size),
      maxOps(p.max_ops),
      fifoSize(p.fifo_size),
      maxReqSize(p.max_req_size),
      maxPending(p.max_pending),
      computeLatency(p.compute_latency),
      dcachePort(this),
      icachePort(this),
      ctrlReg(0),
      statusReg(0),
      completedOps(0),
      errorCode(0),
      firstIssueTick(0),
      finishTick(0),
      issueNextEvent([this] { issueNext(); }, name()),
      finishComputeEvent([this] { finishCompute(); }, name()),
      stats(*this)
{
    traceEntries.reserve(maxOps);
    dmaReadEngine = std::make_unique<DmaReadEngine>(
        this, fifoSize, maxReqSize, maxPending);
    dmaWriteEngine = std::make_unique<DmaWriteEngine>(
        this, fifoSize, maxReqSize, maxPending);
    std::memset(dmaBuffer, 0, sizeof(dmaBuffer));
    resetState();
}

void
PCIeRAODMAAccel::resetState()
{
    ctrlReg = 0;
    statusReg = 0;
    completedOps = 0;
    errorCode = 0;
    firstIssueTick = 0;
    finishTick = 0;
    std::memset(dmaBuffer, 0, sizeof(dmaBuffer));
    traceEntries.clear();
    resultTable.clear();
    stagingEntry = StagingEntry();
    activeContext = ActiveContext();
}

Addr
PCIeRAODMAAccel::barOffset(Addr addr) const
{
    assert(BARs[0]);
    return addr - BARs[0]->addr();
}

Port &
PCIeRAODMAAccel::getPort(const std::string &if_name, PortID idx)
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
PCIeRAODMAAccel::getAddrRanges() const
{
    return PciDevice::getAddrRanges();
}

Tick
PCIeRAODMAAccel::read(PacketPtr pkt)
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
PCIeRAODMAAccel::write(PacketPtr pkt)
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
PCIeRAODMAAccel::traceReady() const
{
    return !traceEntries.empty() && errorCode == 0;
}

void
PCIeRAODMAAccel::startExecution()
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
PCIeRAODMAAccel::finishExecution()
{
    statusReg &= ~STATUS_BUSY;
    statusReg |= STATUS_DONE;
    finishTick = clockEdge();
    if (firstIssueTick != 0 && finishTick >= firstIssueTick) {
        stats.totalExecTicks = finishTick - firstIssueTick;
    }
}

void
PCIeRAODMAAccel::issueNext()
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
    activeContext.op_start_tick = clockEdge();
    if (firstIssueTick == 0) {
        firstIssueTick = clockEdge();
    }
    issueRead(activeContext.trace_index);
}

void
PCIeRAODMAAccel::issueRead(size_t trace_index)
{
    const TraceEntry &entry = traceEntries.at(trace_index);
    activeContext.read_issue_tick = clockEdge();
    std::memset(dmaBuffer, 0, sizeof(dmaBuffer));
    dmaReadEngine->startFill(entry.paddr, sizeof(uint64_t));
    stats.numReads++;
}

void
PCIeRAODMAAccel::issueWrite(size_t trace_index, uint64_t value)
{
    activeContext.write_issue_tick = clockEdge();
    traceEntries.at(trace_index);
    std::memcpy(dmaBuffer, &value, sizeof(uint64_t));
    const TraceEntry &entry = traceEntries.at(trace_index);
    dmaWriteEngine->put(dmaBuffer, sizeof(uint64_t));
    dmaWriteEngine->startWrite(entry.paddr, sizeof(uint64_t));
    stats.numWrites++;
}

uint64_t
PCIeRAODMAAccel::resolveOperand(const TraceEntry &entry) const
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
PCIeRAODMAAccel::handleReadResponse(size_t trace_index)
{
    assert(trace_index < traceEntries.size());
    const TraceEntry &entry = traceEntries.at(trace_index);

    uint64_t old_value = 0;
    std::memcpy(&old_value, dmaBuffer, sizeof(uint64_t));
    activeContext.old_value = old_value;
    activeContext.read_done_tick = clockEdge();
    stats.totalReadLatency +=
        activeContext.read_done_tick - activeContext.read_issue_tick;

    switch (entry.opcode) {
      case OpcodeFetch:
        resultTable[entry.seq_id] = old_value;
        activeContext.state = OpDone;
        completedOps++;
        stats.numFetch++;
        stats.numTotalOps++;
        stats.totalOpLatency += clockEdge() - activeContext.op_start_tick;
        activeContext = ActiveContext();
        schedule(issueNextEvent, nextCycle());
        break;
      case OpcodeFetchAdd:
        activeContext.new_value = old_value + 1;
        activeContext.state = OpComputing;
        stats.numFetchAdd++;
        schedule(finishComputeEvent, clockEdge() + computeLatency);
        break;
      case OpcodeAdd:
        activeContext.new_value = old_value + resolveOperand(entry);
        activeContext.state = OpComputing;
        stats.numAdd++;
        schedule(finishComputeEvent, clockEdge() + computeLatency);
        break;
      default:
        panic("Unsupported opcode %u", entry.opcode);
    }
}

void
PCIeRAODMAAccel::finishCompute()
{
    assert(activeContext.state == OpComputing);
    stats.totalComputeTicks += clockEdge() - activeContext.read_done_tick;
    activeContext.state = OpWriting;
    issueWrite(activeContext.trace_index, activeContext.new_value);
}

void
PCIeRAODMAAccel::handleWriteResponse(size_t trace_index)
{
    const TraceEntry &entry = traceEntries.at(trace_index);
    resultTable[entry.seq_id] = activeContext.old_value;
    completedOps++;
    stats.numTotalOps++;
    stats.totalWriteLatency += clockEdge() - activeContext.write_issue_tick;
    stats.totalOpLatency += clockEdge() - activeContext.op_start_tick;
    activeContext = ActiveContext();
    schedule(issueNextEvent, nextCycle());
}

void
PCIeRAODMAAccel::completeDmaRead()
{
    assert(activeContext.state == OpReading);
    dmaReadEngine->get(dmaBuffer, sizeof(uint64_t));
    handleReadResponse(activeContext.trace_index);
}

void
PCIeRAODMAAccel::completeDmaWrite()
{
    assert(activeContext.state == OpWriting);
    handleWriteResponse(activeContext.trace_index);
}

void
PCIeRAODMAAccel::DmaReadEngine::onIdle()
{
    device->completeDmaRead();
}

void
PCIeRAODMAAccel::DmaWriteEngine::onIdle()
{
    device->completeDmaWrite();
}

void
PCIeRAODMAAccel::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(PCIeRAODMAAccel,
            "%s received snoop for addr=%#x cmd=%s\n",
            __func__, pkt->getAddr(), pkt->cmdString());
}

bool
PCIeRAODMAAccel::DcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(PCIeRAODMAAccel,
            "Unexpected cached response addr=%#x cmd=%s\n",
            pkt->getAddr(), pkt->cmdString());
    delete pkt;
    return true;
}

void
PCIeRAODMAAccel::DcachePort::recvReqRetry()
{
    DPRINTF(PCIeRAODMAAccel, "Unexpected cached retry\n");
}

bool
PCIeRAODMAAccel::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(PCIeRAODMAAccel, "Received icache response %#x\n",
            pkt->getAddr());
    delete pkt;
    return true;
}

void
PCIeRAODMAAccel::IcachePort::recvReqRetry()
{
    DPRINTF(PCIeRAODMAAccel, "Received icache retry\n");
}

} // namespace gem5
