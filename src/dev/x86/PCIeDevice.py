from m5.params import *
from m5.objects.PciDevice import *


class PCIeRAODMAAccel(PciDevice):
    type = 'PCIeRAODMAAccel'
    cxx_header = "dev/x86/pcie_rao_dma_accel.hh"
    cxx_class = 'gem5::PCIeRAODMAAccel'
    dcache_port = RequestPort("Unused cached data port")
    icache_port = RequestPort("Unused instr port")
    cacheline_size = Param.Int(64, "Device cache line size")
    max_ops = Param.Int(4096, "Maximum number of trace entries")
    fifo_size = Param.Int(4096, "DMA FIFO size in bytes")
    max_req_size = Param.Int(64, "Maximum DMA request size in bytes")
    max_pending = Param.Int(1, "Maximum number of pending DMA requests")
    compute_latency = Param.Latency(
        "15ns", "Compute/control latency before a mutating RAO DMA write"
    )

    VendorID = 0x8086
    DeviceID = 0x7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x1f
    InterruptPin = 0x01

    BAR0 = PciMemBar(size='4MiB')
    BAR1 = PciMemUpperBar()

    def connectCachedPorts(self, in_ports):
        self.dcache_port = in_ports
