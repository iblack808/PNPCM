from typing import List, Sequence, Tuple

from m5.objects import AddrRange, CXLPCMFromNVM_2400_1x64, MemCtrl, Port
from m5.util.convert import toMemoryBandwidth, toMemorySize

from ...utils.override import overrides
from ..boards.abstract_board import AbstractBoard
from .abstract_memory_system import AbstractMemorySystem


class SingleChannelCXLPCMFromNVM(AbstractMemorySystem):
    def __init__(
        self,
        size: str = "1GiB",
        read_latency: str = "150ns",
        write_latency: str = "500ns",
        read_bandwidth: str = "8GiB/s",
        write_bandwidth: str = "2GiB/s",
        static_frontend_latency: str = "10ns",
        static_backend_latency: str = "10ns",
    ) -> None:
        super().__init__()

        self._size_str = size
        self._size = toMemorySize(size)
        self.nvm = CXLPCMFromNVM_2400_1x64(
            tREAD=read_latency,
            tWRITE=write_latency,
            tBURST=self._bandwidth_to_tburst(
                max(
                    toMemoryBandwidth(read_bandwidth),
                    toMemoryBandwidth(write_bandwidth),
                )
            ),
            tREAD_BURST=self._bandwidth_to_tburst(
                toMemoryBandwidth(read_bandwidth)
            ),
            tWRITE_BURST=self._bandwidth_to_tburst(
                toMemoryBandwidth(write_bandwidth)
            ),
        )
        self.mem_ctrl = MemCtrl(
            dram=self.nvm,
            static_frontend_latency=static_frontend_latency,
            static_backend_latency=static_backend_latency,
        )

    @staticmethod
    def _bandwidth_to_tburst(bandwidth: float) -> str:
        device_bus_width = getattr(
            CXLPCMFromNVM_2400_1x64.device_bus_width, "value",
            CXLPCMFromNVM_2400_1x64.device_bus_width,
        )
        burst_length = getattr(
            CXLPCMFromNVM_2400_1x64.burst_length, "value",
            CXLPCMFromNVM_2400_1x64.burst_length,
        )
        burst_bytes = (
            int(device_bus_width) * int(burst_length) // 8
        )
        return f"{burst_bytes / bandwidth * 1e9}ns"

    @overrides(AbstractMemorySystem)
    def incorporate_memory(self, board: AbstractBoard) -> None:
        self.mem_ctrl.clk_domain = board.get_clock_domain()

    @overrides(AbstractMemorySystem)
    def get_mem_ports(self) -> Sequence[Tuple[AddrRange, Port]]:
        return [(self.nvm.range, self.mem_ctrl.port)]

    @overrides(AbstractMemorySystem)
    def get_memory_controllers(self) -> List[MemCtrl]:
        return [self.mem_ctrl]

    @overrides(AbstractMemorySystem)
    def get_size(self) -> int:
        return self._size

    def get_size_str(self) -> str:
        return self._size_str

    @overrides(AbstractMemorySystem)
    def set_memory_range(self, ranges: List[AddrRange]) -> None:
        if len(ranges) != 1 or ranges[0].size() != self._size:
            raise Exception(
                "CXL-PCM_from_nvm memory requires a single range matching "
                "its size."
            )
        self.nvm.range = ranges[0]
