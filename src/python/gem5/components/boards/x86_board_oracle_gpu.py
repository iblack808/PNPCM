# Copyright (c) 2026

from typing import List, Sequence

from m5.objects import (
    Addr,
    AddrRange,
    BaseXBar,
    Bridge,
    CowDiskImage,
    IdeDisk,
    IOXBar,
    OracleGPU,
    Pc,
    Port,
    RawDiskImage,
    X86E820Entry,
    X86FsLinux,
    X86IntelMPBus,
    X86IntelMPBusHierarchy,
    X86IntelMPIOAPIC,
    X86IntelMPIOIntAssignment,
    X86IntelMPProcessor,
    X86SMBiosBiosInformation,
)
from m5.util.convert import toMemorySize

from ...isas import ISA
from ...resources.resource import AbstractResource
from ...utils.override import overrides
from ..cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy
from ..memory.abstract_memory_system import AbstractMemorySystem
from ..processors.abstract_processor import AbstractProcessor
from .abstract_system_board import AbstractSystemBoard
from .kernel_disk_workload import KernelDiskWorkload


class X86BoardOracleGPU(AbstractSystemBoard, KernelDiskWorkload):
    ORACLE_GPU_SCRATCH_BASE = 0x30000000
    ORACLE_GPU_SCRATCH_SIZE = 0x08000000

    def __init__(
        self,
        clk_freq: str,
        processor: AbstractProcessor,
        memory: AbstractMemorySystem,
        cache_hierarchy: AbstractCacheHierarchy,
        oracle_gpu_mmio_base: int = 0xC1000000,
        oracle_gpu_mmio_size: int = 0x1000,
        oracle_gpu_max_transfer_bytes: int = 4096,
    ) -> None:
        self._oracle_gpu_mmio_base = oracle_gpu_mmio_base
        self._oracle_gpu_mmio_size = oracle_gpu_mmio_size
        self._oracle_gpu_max_transfer_bytes = oracle_gpu_max_transfer_bytes

        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )

        if self.get_processor().get_isa() != ISA.X86:
            raise Exception(
                "The X86BoardOracleGPU requires a processor using the X86 "
                f"ISA. Current processor ISA: '{processor.get_isa().name}'."
            )

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        self.pc = Pc()
        self.oracle_gpu = OracleGPU(
            pio_addr=self._oracle_gpu_mmio_base,
            pio_size=self._oracle_gpu_mmio_size,
            pio_latency="50ns",
            max_transfer_bytes=self._oracle_gpu_max_transfer_bytes,
        )
        self.workload = X86FsLinux()
        self.iobus = IOXBar()
        self._setup_io_devices()
        self.m5ops_base = 0xFFFF0000

    def _setup_io_devices(self):
        io_address_space_base = 0x8000000000000000
        pci_config_address_space_base = 0xC000000000000000
        interrupts_address_space_base = 0xA000000000000000
        apic_range_size = 1 << 12

        if self.get_cache_hierarchy().is_ruby():
            self.pc.attachIO(
                self.get_io_bus(),
                [self.pc.south_bridge.ide.dma, self.oracle_gpu.dma],
            )
        else:
            self.bridge = Bridge(delay="50ns")
            self.bridge.mem_side_port = self.get_io_bus().cpu_side_ports
            self.bridge.cpu_side_port = (
                self.get_cache_hierarchy().get_mem_side_port()
            )
            self.bridge.ranges = [
                AddrRange(0xC0000000, 0xFFFF0000),
                AddrRange(
                    io_address_space_base, interrupts_address_space_base - 1
                ),
                AddrRange(pci_config_address_space_base, Addr.max),
            ]

            self.apicbridge = Bridge(delay="50ns")
            self.apicbridge.cpu_side_port = self.get_io_bus().mem_side_ports
            self.apicbridge.mem_side_port = (
                self.get_cache_hierarchy().get_cpu_side_port()
            )
            self.apicbridge.ranges = [
                AddrRange(
                    interrupts_address_space_base,
                    interrupts_address_space_base
                    + self.get_processor().get_num_cores() * apic_range_size
                    - 1,
                )
            ]
            self.pc.attachIO(self.get_io_bus())

        self.oracle_gpu.pio = self.get_io_bus().mem_side_ports
        self.oracle_gpu.dma = self.get_io_bus().cpu_side_ports

        self.workload.smbios_table.structures = [X86SMBiosBiosInformation()]

        base_entries = []
        ext_entries = []
        for i in range(self.get_processor().get_num_cores()):
            base_entries.append(
                X86IntelMPProcessor(
                    local_apic_id=i,
                    local_apic_version=0x14,
                    enable=True,
                    bootstrap=(i == 0),
                )
            )

        io_apic = X86IntelMPIOAPIC(
            id=self.get_processor().get_num_cores(),
            version=0x11,
            enable=True,
            address=0xFEC00000,
        )

        self.pc.south_bridge.io_apic.apic_id = io_apic.id
        base_entries.append(io_apic)
        base_entries.append(X86IntelMPBus(bus_id=0, bus_type="PCI   "))
        base_entries.append(X86IntelMPBus(bus_id=1, bus_type="ISA   "))
        ext_entries.append(
            X86IntelMPBusHierarchy(
                bus_id=1, subtractive_decode=True, parent_bus=0
            )
        )

        base_entries.append(
            X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=0,
                source_bus_irq=0 + (4 << 2),
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=16,
            )
        )

        def assignISAInt(irq, apicPin):
            base_entries.append(
                X86IntelMPIOIntAssignment(
                    interrupt_type="ExtInt",
                    polarity="ConformPolarity",
                    trigger="ConformTrigger",
                    source_bus_id=1,
                    source_bus_irq=irq,
                    dest_io_apic_id=io_apic.id,
                    dest_io_apic_intin=0,
                )
            )
            base_entries.append(
                X86IntelMPIOIntAssignment(
                    interrupt_type="INT",
                    polarity="ConformPolarity",
                    trigger="ConformTrigger",
                    source_bus_id=1,
                    source_bus_irq=irq,
                    dest_io_apic_id=io_apic.id,
                    dest_io_apic_intin=apicPin,
                )
            )

        assignISAInt(0, 2)
        assignISAInt(1, 1)
        for i in range(3, 15):
            assignISAInt(i, i)

        self.workload.intel_mp_table.base_entries = base_entries
        self.workload.intel_mp_table.ext_entries = ext_entries

        entries = [
            X86E820Entry(addr=0, size="639kB", range_type=1),
            X86E820Entry(addr=0x9FC00, size="385kB", range_type=2),
            X86E820Entry(
                addr=0x100000,
                size=f"{self.ORACLE_GPU_SCRATCH_BASE - 0x100000:d}B",
                range_type=1,
            ),
            X86E820Entry(
                addr=self.ORACLE_GPU_SCRATCH_BASE,
                size=f"{self.ORACLE_GPU_SCRATCH_SIZE:d}B",
                range_type=2,
            ),
            X86E820Entry(
                addr=self.ORACLE_GPU_SCRATCH_BASE + self.ORACLE_GPU_SCRATCH_SIZE,
                size=f"{self.mem_ranges[0].size() - (self.ORACLE_GPU_SCRATCH_BASE + self.ORACLE_GPU_SCRATCH_SIZE):d}B",
                range_type=1,
            ),
            X86E820Entry(addr=0xFFFF0000, size="64kB", range_type=2),
        ]
        self.workload.e820_table.entries = entries

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self) -> BaseXBar:
        return self.iobus

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> Sequence[Port]:
        return [
            self.pc.south_bridge.ide.dma,
            self.iobus.mem_side_ports,
            self.oracle_gpu.dma,
        ]

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        return self.iobus.mem_side_ports

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self):
        memory = self.get_memory()

        if memory.get_size() > toMemorySize("3GB"):
            raise Exception(
                "X86BoardOracleGPU currently only supports memory sizes up "
                "to 3GB because of the I/O hole."
            )

        data_range = AddrRange(memory.get_size())
        memory.set_memory_range([data_range])
        cpu_abstract_mems = []
        for mc in memory.get_memory_controllers():
            cpu_abstract_mems.append(mc.dram)
        self.memories = cpu_abstract_mems

        self.mem_ranges = [
            data_range,
            AddrRange(0xC0000000, size=0x100000),
        ]

    @overrides(KernelDiskWorkload)
    def get_disk_device(self):
        return "/dev/sda1"

    @overrides(KernelDiskWorkload)
    def get_default_kernel_args(self) -> List[str]:
        return [
            "earlyprintk=ttyS0",
            "console=ttyS0",
            "lpj=7999923",
            "root={root_value}",
            "disk_device={disk_device}",
            "idle=nomwait",
            "intel_idle.max_cstate=0",
            "memmap=16M$0x30000000",
        ]

    @overrides(KernelDiskWorkload)
    def _add_disk_to_board(self, disk_image: AbstractResource):
        ide_disk = IdeDisk()
        ide_disk.driveID = "device0"
        ide_disk.image = CowDiskImage(
            child=RawDiskImage(
                read_only=True, image_file=disk_image.get_local_path()
            ),
            read_only=False,
        )

        self.pc.south_bridge.ide.disks = [ide_disk]
