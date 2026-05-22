from pathlib import Path
import argparse

import m5
import gem5.components.boards as gem5_boards_pkg
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.components.memory.cxl_pcm_from_dram import (
    SingleChannelCXLPCMFromDRAM,
)
from gem5.components.memory.cxl_pcm_from_nvm import (
    SingleChannelCXLPCMFromNVM,
)
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import DiskImageResource, KernelResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.X86)

KERNEL_PATH = "/data/tyb/gem5/vmlinux"
DISK_IMAGE_PATH = "/data/tyb/gem5/parsec.img"
CXL_PCM_SIZE = "1GiB"
CXL_PCM_BASE = 0x100000000
CXL_PCM_READ_LATENCY = "150ns"
CXL_PCM_WRITE_LATENCY = "500ns"
CXL_PCM_READ_BANDWIDTH = "8GiB/s"
CXL_PCM_WRITE_BANDWIDTH = "2GiB/s"
CXL_PCM_TEST_BYTES = 64 * 1024

REPO_ROOT = Path(__file__).resolve().parents[3]
BOARDS_SRC_PATH = REPO_ROOT / "src/python/gem5/components/boards"
if str(BOARDS_SRC_PATH) not in gem5_boards_pkg.__path__:
    gem5_boards_pkg.__path__.append(str(BOARDS_SRC_PATH))

from gem5.components.boards.x86_board_oracle_gpu import X86BoardOracleGPU


parser = argparse.ArgumentParser(
    description="Run x86 FS with DDR, OracleGPU, and CXL-PCM memory."
)
parser.add_argument("--cxl-pcm-backend", choices=["from_dram", "from_nvm"],
                    default="from_dram")
args = parser.parse_args()

DETAILED_CPU_TYPE = CPUTypes.TIMING
BOOT_CPU_TYPE = CPUTypes.KVM
NUM_CPUS = 1


def build_guest_command() -> str:
    return f"""
set -e
if [ -x /sbin/m5 ]; then
    /sbin/m5 exit
fi
/usr/local/bin/cxl_pcm_mem_test {CXL_PCM_BASE:#x} {CXL_PCM_TEST_BYTES}
/usr/local/bin/oracle_gpu_cxl_pcm_test
/sbin/m5 exit
exec /bin/sh
"""


cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="32kB",
    l1d_assoc=8,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="256kB",
    l2_assoc=8,
    l3_size="8MB",
    l3_assoc=16,
)

memory = SingleChannelDDR4_2400(size="3GB")
cxl_pcm_cls = {
    "from_dram": SingleChannelCXLPCMFromDRAM,
    "from_nvm": SingleChannelCXLPCMFromNVM,
}[args.cxl_pcm_backend]
cxl_pcm = cxl_pcm_cls(
    size=CXL_PCM_SIZE,
    read_latency=CXL_PCM_READ_LATENCY,
    write_latency=CXL_PCM_WRITE_LATENCY,
    read_bandwidth=CXL_PCM_READ_BANDWIDTH,
    write_bandwidth=CXL_PCM_WRITE_BANDWIDTH,
)

processor = SimpleSwitchableProcessor(
    starting_core_type=BOOT_CPU_TYPE,
    switch_core_type=DETAILED_CPU_TYPE,
    isa=ISA.X86,
    num_cores=NUM_CPUS,
)

if BOOT_CPU_TYPE == CPUTypes.KVM:
    for proc in processor.start:
        proc.core.usePerf = False

board = X86BoardOracleGPU(
    clk_freq="2GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_pcm_memory=cxl_pcm,
    cxl_pcm_base=CXL_PCM_BASE,
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_IMAGE_PATH),
    readfile_contents=build_guest_command(),
    kernel_args=board.get_default_kernel_args() + ["init=/root/gem5_init.sh"],
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [processor.switch]),
    },
)

print(
    "Booting x86 FS system with OracleGPU and "
    f"CXL-PCM_{args.cxl_pcm_backend}"
)
m5.stats.reset()
simulator.run()
