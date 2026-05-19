from m5.params import *
from m5.proxy import *

from m5.objects.Device import DmaDevice


class OracleGPU(DmaDevice):
    type = "OracleGPU"
    cxx_class = "gem5::OracleGPU"
    cxx_header = "dev/oracle_gpu.hh"

    pio_addr = Param.Addr("Device MMIO base address")
    pio_size = Param.Addr(0x1000, "Device MMIO size")
    pio_latency = Param.Latency("100ns", "PIO access latency")
    max_transfer_bytes = Param.UInt32(
        4096, "Maximum payload bytes accepted in a single command"
    )
