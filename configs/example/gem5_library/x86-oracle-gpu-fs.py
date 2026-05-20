import base64
import os
from pathlib import Path

import m5
import gem5.components.boards as gem5_boards_pkg
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
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

REPO_ROOT = Path(__file__).resolve().parents[3]
BOARDS_SRC_PATH = REPO_ROOT / "src/python/gem5/components/boards"
if str(BOARDS_SRC_PATH) not in gem5_boards_pkg.__path__:
    gem5_boards_pkg.__path__.append(str(BOARDS_SRC_PATH))

from gem5.components.boards.x86_board_oracle_gpu import X86BoardOracleGPU

KERNEL_PATH = "/data/tyb/gem5/vmlinux"
DISK_IMAGE_PATH = "/data/tyb/gem5/parsec.img"
GUEST_BIN_PATH = os.path.join(
    os.path.dirname(os.path.realpath(__file__)),
    "../../../tests/test-progs/oracle-gpu/bin/x86/linux/oracle_gpu_fs_test",
)
DETAILED_CPU_TYPE = CPUTypes.TIMING
BOOT_CPU_TYPE = CPUTypes.KVM
NUM_CPUS = 1


def build_guest_command(guest_bin_path: str) -> str:
    with open(guest_bin_path, "rb") as fh:
        guest_bin_b64 = base64.b64encode(fh.read()).decode("ascii")

    return f"""
set -e
if [ -x /sbin/m5 ]; then
    /sbin/m5 exit
fi
cat >/root/oracle_gpu_fs_test.b64 <<'GEM5_EOF'
{guest_bin_b64}
GEM5_EOF
base64 -d /root/oracle_gpu_fs_test.b64 >/root/oracle_gpu_fs_test
chmod +x /root/oracle_gpu_fs_test
/root/oracle_gpu_fs_test
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
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_IMAGE_PATH),
    readfile_contents=build_guest_command(GUEST_BIN_PATH),
    kernel_args=board.get_default_kernel_args() + ["init=/root/gem5_init.sh"],
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [processor.switch]),
    },
)

print("Booting x86 FS system with OracleGPU")
m5.stats.reset()
simulator.run()
