from m5.objects.AbstractMemory import *
from m5.params import *


class CXLPCMMemory(AbstractMemory):
    type = "CXLPCMMemory"
    cxx_header = "mem/cxl_pcm_memory.hh"
    cxx_class = "gem5::memory::CXLPCMMemory"

    port = ResponsePort("This port sends responses and receives requests")

    read_latency = Param.Latency("150ns", "CXL-PCM read latency")
    write_latency = Param.Latency("500ns", "CXL-PCM write latency")
    latency_var = Param.Latency("0ns", "CXL-PCM access latency variance")

    read_bandwidth = Param.MemoryBandwidth(
        "8GiB/s", "CXL-PCM read bandwidth"
    )
    write_bandwidth = Param.MemoryBandwidth(
        "2GiB/s", "CXL-PCM write bandwidth"
    )

    def controller(self):
        return self
