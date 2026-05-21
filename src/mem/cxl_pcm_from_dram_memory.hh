/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __MEM_CXL_PCM_FROM_DRAM_MEMORY_HH__
#define __MEM_CXL_PCM_FROM_DRAM_MEMORY_HH__

#include <list>

#include "base/statistics.hh"
#include "mem/abstract_mem.hh"
#include "mem/port.hh"
#include "params/CXLPCMFromDRAMMemory.hh"

namespace gem5
{

namespace memory
{

class CXLPCMFromDRAMMemory : public AbstractMemory
{
  private:
    class DeferredPacket
    {
      public:
        const Tick tick;
        const PacketPtr pkt;

        DeferredPacket(PacketPtr _pkt, Tick _tick) : tick(_tick), pkt(_pkt)
        { }
    };

    class MemoryPort : public ResponsePort
    {
      private:
        CXLPCMFromDRAMMemory& mem;

      public:
        MemoryPort(const std::string& _name,
                   CXLPCMFromDRAMMemory& _memory);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        Tick recvAtomicBackdoor(
                PacketPtr pkt, MemBackdoorPtr &_backdoor) override;
        void recvFunctional(PacketPtr pkt) override;
        void recvMemBackdoorReq(const MemBackdoorReq &req,
                MemBackdoorPtr &backdoor) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    struct PCMStats : public statistics::Group
    {
        explicit PCMStats(CXLPCMFromDRAMMemory &mem);

        statistics::Scalar pcmReadBytes;
        statistics::Scalar pcmWriteBytes;
        statistics::Scalar pcmReadRequests;
        statistics::Scalar pcmWriteRequests;
        statistics::Scalar totalPcmWrites;
    } pcmStats;

    MemoryPort port;

    const Tick readLatency;
    const Tick writeLatency;
    const Tick latencyVar;

    std::list<DeferredPacket> packetQueue;

    const double readBandwidth;
    const double writeBandwidth;

    bool isBusy;
    bool retryReq;
    bool retryResp;

    void release();
    EventFunctionWrapper releaseEvent;

    void dequeue();
    EventFunctionWrapper dequeueEvent;

    Tick getLatency(PacketPtr pkt) const;
    double getBandwidth(PacketPtr pkt) const;
    void recordPCMStats(PacketPtr pkt);

    std::unique_ptr<Packet> pendingDelete;

  public:
    CXLPCMFromDRAMMemory(const CXLPCMFromDRAMMemoryParams &p);

    DrainState drain() override;

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
    void init() override;

  protected:
    Tick recvAtomic(PacketPtr pkt);
    Tick recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor);
    void recvFunctional(PacketPtr pkt);
    void recvMemBackdoorReq(const MemBackdoorReq &req,
            MemBackdoorPtr &backdoor);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
};

} // namespace memory
} // namespace gem5

#endif // __MEM_CXL_PCM_FROM_DRAM_MEMORY_HH__
