/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "mem/cxl_pcm_memory.hh"

#include "base/random.hh"
#include "base/trace.hh"
#include "debug/CXLPCM.hh"
#include "debug/Drain.hh"

namespace gem5
{

namespace memory
{

CXLPCMMemory::PCMStats::PCMStats(CXLPCMMemory &mem)
    : statistics::Group(&mem),
      ADD_STAT(pcmReadBytes, statistics::units::Byte::get(),
               "Total bytes read from this CXL-PCM memory"),
      ADD_STAT(pcmWriteBytes, statistics::units::Byte::get(),
               "Total bytes written to this CXL-PCM memory"),
      ADD_STAT(pcmReadRequests, statistics::units::Count::get(),
               "Total read requests served by this CXL-PCM memory"),
      ADD_STAT(pcmWriteRequests, statistics::units::Count::get(),
               "Total write requests served by this CXL-PCM memory"),
      ADD_STAT(totalPcmWrites, statistics::units::Byte::get(),
               "Total PCM write traffic for endurance analysis")
{
}

CXLPCMMemory::CXLPCMMemory(const CXLPCMMemoryParams &p)
    : AbstractMemory(p),
      pcmStats(*this),
      port(name() + ".port", *this),
      readLatency(p.read_latency),
      writeLatency(p.write_latency),
      latencyVar(p.latency_var),
      readBandwidth(p.read_bandwidth),
      writeBandwidth(p.write_bandwidth),
      isBusy(false),
      retryReq(false),
      retryResp(false),
      releaseEvent([this]{ release(); }, name()),
      dequeueEvent([this]{ dequeue(); }, name())
{
}

void
CXLPCMMemory::init()
{
    AbstractMemory::init();

    if (port.isConnected()) {
        port.sendRangeChange();
    }
}

Tick
CXLPCMMemory::recvAtomic(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    const Tick latency = getLatency(pkt);
    recordPCMStats(pkt);
    access(pkt);
    return latency;
}

Tick
CXLPCMMemory::recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor)
{
    const Tick latency = recvAtomic(pkt);
    getBackdoor(_backdoor);
    return latency;
}

void
CXLPCMMemory::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());

    functionalAccess(pkt);

    bool done = false;
    auto p = packetQueue.begin();
    while (!done && p != packetQueue.end()) {
        done = pkt->trySatisfyFunctional(p->pkt);
        ++p;
    }

    pkt->popLabel();
}

void
CXLPCMMemory::recvMemBackdoorReq(const MemBackdoorReq &req,
        MemBackdoorPtr &_backdoor)
{
    getBackdoor(_backdoor);
}

bool
CXLPCMMemory::recvTimingReq(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "CXL-PCM only supports read and write requests, saw %s to "
             "%#llx\n", pkt->cmdString(), pkt->getAddr());

    if (retryReq) {
        return false;
    }

    if (isBusy) {
        retryReq = true;
        return false;
    }

    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    const Tick duration = pkt->getSize() * getBandwidth(pkt);
    if (duration != 0) {
        schedule(releaseEvent, curTick() + duration);
        isBusy = true;
    }

    const bool needs_response = pkt->needsResponse();
    const Tick access_latency = getLatency(pkt);
    recvAtomic(pkt);

    if (needs_response) {
        assert(pkt->isResponse());

        const Tick when_to_send = curTick() + receive_delay + access_latency;

        auto i = packetQueue.begin();
        while (i != packetQueue.end() && i->tick <= when_to_send) {
            if (i->pkt->matchAddr(pkt)) {
                break;
            }
            ++i;
        }
        packetQueue.emplace(i, pkt, when_to_send);

        if (!retryResp && !dequeueEvent.scheduled()) {
            schedule(dequeueEvent, packetQueue.front().tick);
        }
    } else {
        pendingDelete.reset(pkt);
    }

    return true;
}

void
CXLPCMMemory::release()
{
    assert(isBusy);
    isBusy = false;
    if (retryReq) {
        retryReq = false;
        port.sendRetryReq();
    }
}

void
CXLPCMMemory::dequeue()
{
    assert(!packetQueue.empty());
    DeferredPacket deferred_pkt = packetQueue.front();

    retryResp = !port.sendTimingResp(deferred_pkt.pkt);

    if (!retryResp) {
        packetQueue.pop_front();

        if (!packetQueue.empty()) {
            reschedule(dequeueEvent,
                       std::max(packetQueue.front().tick, curTick()), true);
        } else if (drainState() == DrainState::Draining) {
            DPRINTF(Drain, "Draining of CXLPCMMemory complete\n");
            signalDrainDone();
        }
    }
}

Tick
CXLPCMMemory::getLatency(PacketPtr pkt) const
{
    Tick base_latency = readLatency;
    if (pkt->isWrite()) {
        base_latency = writeLatency;
    }

    return base_latency +
        (latencyVar ? random_mt.random<Tick>(0, latencyVar) : 0);
}

double
CXLPCMMemory::getBandwidth(PacketPtr pkt) const
{
    return pkt->isWrite() ? writeBandwidth : readBandwidth;
}

void
CXLPCMMemory::recordPCMStats(PacketPtr pkt)
{
    if (pkt->isRead()) {
        pcmStats.pcmReadRequests++;
        pcmStats.pcmReadBytes += pkt->getSize();
        DPRINTF(CXLPCM, "read addr=%#llx bytes=%u\n",
                pkt->getAddr(), pkt->getSize());
    } else if (pkt->isWrite()) {
        pcmStats.pcmWriteRequests++;
        pcmStats.pcmWriteBytes += pkt->getSize();
        pcmStats.totalPcmWrites += pkt->getSize();
        DPRINTF(CXLPCM, "write addr=%#llx bytes=%u\n",
                pkt->getAddr(), pkt->getSize());
    }
}

void
CXLPCMMemory::recvRespRetry()
{
    assert(retryResp);

    dequeue();
}

Port &
CXLPCMMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port") {
        return AbstractMemory::getPort(if_name, idx);
    } else {
        return port;
    }
}

DrainState
CXLPCMMemory::drain()
{
    if (!packetQueue.empty()) {
        DPRINTF(Drain, "CXLPCMMemory queue has requests, waiting to drain\n");
        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

CXLPCMMemory::MemoryPort::MemoryPort(const std::string& _name,
                                     CXLPCMMemory& _memory)
    : ResponsePort(_name), mem(_memory)
{ }

AddrRangeList
CXLPCMMemory::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(mem.getAddrRange());
    return ranges;
}

Tick
CXLPCMMemory::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return mem.recvAtomic(pkt);
}

Tick
CXLPCMMemory::MemoryPort::recvAtomicBackdoor(
        PacketPtr pkt, MemBackdoorPtr &_backdoor)
{
    return mem.recvAtomicBackdoor(pkt, _backdoor);
}

void
CXLPCMMemory::MemoryPort::recvFunctional(PacketPtr pkt)
{
    mem.recvFunctional(pkt);
}

void
CXLPCMMemory::MemoryPort::recvMemBackdoorReq(const MemBackdoorReq &req,
        MemBackdoorPtr &backdoor)
{
    mem.recvMemBackdoorReq(req, backdoor);
}

bool
CXLPCMMemory::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return mem.recvTimingReq(pkt);
}

void
CXLPCMMemory::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry();
}

} // namespace memory
} // namespace gem5
