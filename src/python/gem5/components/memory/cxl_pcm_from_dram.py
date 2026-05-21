from typing import List, Sequence, Tuple

from m5.objects import AddrRange, CXLPCMFromDRAMMemory, Port
from m5.util.convert import toMemorySize

from ...utils.override import overrides
from ..boards.abstract_board import AbstractBoard
from .abstract_memory_system import AbstractMemorySystem


class SingleChannelCXLPCMFromDRAM(AbstractMemorySystem):
    def __init__(
        self,
        size: str = "1GiB",
        read_latency: str = "150ns",
        write_latency: str = "500ns",
        read_bandwidth: str = "8GiB/s",
        write_bandwidth: str = "2GiB/s",
        latency_var: str = "0ns",
    ) -> None:
        super().__init__()

        self._size_str = size
        self._size = toMemorySize(size)
        self.module = CXLPCMFromDRAMMemory(
            read_latency=read_latency,
            write_latency=write_latency,
            latency_var=latency_var,
            read_bandwidth=read_bandwidth,
            write_bandwidth=write_bandwidth,
        )

    @overrides(AbstractMemorySystem)
    def incorporate_memory(self, board: AbstractBoard) -> None:
        self.module.clk_domain = board.get_clock_domain()

    @overrides(AbstractMemorySystem)
    def get_mem_ports(self) -> Sequence[Tuple[AddrRange, Port]]:
        return [(self.module.range, self.module.port)]

    @overrides(AbstractMemorySystem)
    def get_memory_controllers(self) -> List[CXLPCMFromDRAMMemory]:
        return [self.module]

    @overrides(AbstractMemorySystem)
    def get_size(self) -> int:
        return self._size

    def get_size_str(self) -> str:
        return self._size_str

    @overrides(AbstractMemorySystem)
    def set_memory_range(self, ranges: List[AddrRange]) -> None:
        if len(ranges) != 1 or ranges[0].size() != self._size:
            raise Exception(
                "CXL-PCM memory requires a single range matching its size."
            )
        self.module.range = ranges[0]
