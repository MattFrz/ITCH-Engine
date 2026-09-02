# Linux tuning for the low-latency path

None of this is required. The engine builds, runs, passes its tests and
benchmarks on an untuned machine without root, and every knob it does touch
degrades to a printed note rather than a failure. What tuning buys is the
tail: on an untuned desktop the whole-datagram p99.9 is roughly 2.6x the p50
and the maximum is four orders of magnitude above it, and essentially all of
that is the operating system rather than the code.

The tools print what they actually got:

```
platform: linux: cpu affinity yes, numa yes (sysfs), mlockall yes, SCHED_FIFO yes
runtime : pinned to cpu 6
runtime : memory locked (mlockall)
runtime : real-time scheduling FAILED: SCHED_FIFO: errno 1 (needs CAP_SYS_NICE)
```

A run that says `FAILED` did not get that setting, and its numbers should be
read accordingly.

---

## What the software does itself

Flags on `itch_replay`, `itch_live` and both benchmarks:

| flag | what it does | needs |
|---|---|---|
| `--cpu N` | pins the thread to logical CPU N | nothing |
| `--lock-memory` | `mlockall(MCL_CURRENT \| MCL_FUTURE)` | `CAP_IPC_LOCK` or a raised `RLIMIT_MEMLOCK` |
| `--realtime` | `SCHED_FIFO` | `CAP_SYS_NICE` |

`RuntimeTuning` (cpp/include/itch_engine/runtime/affinity.hpp) also supports
`numa_node`, which resolves the node's CPU list from
`/sys/devices/system/node/nodeN/cpulist` and pins inside it - no libnuma
dependency, same source libnuma reads.

Granting the capabilities without running as root:

```bash
sudo setcap cap_sys_nice,cap_ipc_lock+ep ./build/cpp/itch_live
```

---

## What the software does NOT do, on purpose

Steering NIC interrupts, changing the CPU governor and reserving huge pages are
root-level changes to a shared machine. A library that a unit test links should
not be making them, so they are documented here instead of being applied
silently. Everything below is something you do to the host, once, deliberately.

---

## 1. Isolate the cores the engine runs on

Kernel command line:

```
isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3
```

* `isolcpus` keeps the general scheduler off those CPUs.
* `nohz_full` stops the periodic timer tick on them when one task is running,
  which removes a per-tick interruption from the tail.
* `rcu_nocbs` moves RCU callback processing off them.
* `irqaffinity` sends every interrupt that is not explicitly steered to the
  housekeeping cores.

Then pin with `--cpu 4`. Check it took:

```bash
cat /sys/devices/system/cpu/isolated
```

Leave SMT siblings of an isolated core unused, or disable SMT
(`nosmt`) - a sibling running unrelated work shares the core's execution
resources and shows up directly in the tail.

## 2. Fix the CPU frequency

Frequency transitions are visible at p99. Pin the governor:

```bash
sudo cpupower frequency-set -g performance
# and stop the deepest idle states, whose exit latency is the other half
sudo cpupower idle-set -D 0
```

Verify:

```bash
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq
```

Note the consequence for measurement: with a fixed frequency, TSC ticks and
core cycles keep a constant ratio, so `cycles/event` becomes directly
comparable across runs. Without it, only the nanosecond columns are.

## 3. Steer NIC interrupts away from the engine core

Find the queues:

```bash
grep eth0 /proc/interrupts
```

Pin each to a housekeeping core (this is why `irqaffinity=0-3` is set above):

```bash
# for each IRQ number N belonging to the NIC
echo 2 | sudo tee /proc/irq/N/smp_affinity_list
```

Stop `irqbalance` from undoing it:

```bash
sudo systemctl stop irqbalance && sudo systemctl disable irqbalance
```

The goal is that the core running the receive loop takes no NIC interrupt at
all - it is polling, and an interrupt on it is pure latency.

## 4. Socket and stack settings

The receiver asks for `SO_RCVBUF` and reports what the kernel actually granted;
the kernel caps it, so raise the cap first:

```bash
sudo sysctl -w net.core.rmem_max=268435456
sudo sysctl -w net.core.rmem_default=268435456
sudo sysctl -w net.core.netdev_max_backlog=65536
# busy polling, if you pass --busy-poll
sudo sysctl -w net.core.busy_read=50
sudo sysctl -w net.core.busy_poll=50
```

Then check the granted value in the tool's output:

```
SO_RCVBUF requested 33554432, kernel granted 33554432
```

A `kernel_drops` count above zero in `itch_live` means the socket buffer
overflowed - the host was too slow or the buffer too small. It is deliberately
reported separately from a MoldUDP64 sequence gap: drops say "you were too
slow", gaps say "something upstream lost them".

Coalescing is a direct latency/throughput trade. For lowest latency:

```bash
sudo ethtool -C eth0 rx-usecs 0 rx-frames 1
sudo ethtool -G eth0 rx 4096        # deeper ring, more burst tolerance
```

## 5. Huge pages

The book's preallocated arrays are exactly the kind of allocation that wants
2 MiB pages: with the default configuration it holds about 9 MiB, which is
~2,300 4 KiB pages against 5 huge pages, and the TLB pressure of the former is
measurable (see the capacity table in
[low_latency_architecture.md](low_latency_architecture.md)).

Reserve at boot, which is the only reliable way to get contiguous memory:

```
default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

or at runtime, which can fail on a fragmented system:

```bash
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
```

Transparent huge pages are the zero-effort version and are usually enough here,
since the allocations are large and long-lived:

```bash
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo defer  | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
```

`defer` rather than `always` for defrag: synchronous compaction inside a page
fault is a multi-millisecond stall, which is worse than the misses it saves.

## 6. Memory locking

`--lock-memory` calls `mlockall(MCL_CURRENT | MCL_FUTURE)` so nothing the
engine touches can be swapped or reclaimed. Raise the limit first:

```bash
# /etc/security/limits.conf
*  hard  memlock  unlimited
*  soft  memlock  unlimited
```

## 7. NUMA

On a multi-socket machine, the engine core, the memory it uses and the NIC
should all be on one node. Find the NIC's node:

```bash
cat /sys/class/net/eth0/device/numa_node
```

Then pin to that node's CPUs (`RuntimeTuning::numa_node`, or `--cpu` with a CPU
from `/sys/devices/system/node/nodeN/cpulist`). Crossing an interconnect for
every packet costs more than every optimization in this repository put
together.

---

## Verifying it worked

Run the pipeline benchmark before and after. The mean should barely move - the
code is the same - and the tail should tighten considerably:

```bash
./build/cpp/bench_pipeline --capture data/capture/AAPL_2026-07-30.itchcap --cpu 4 --lock-memory
```

Compare p99.9, p99.99 and max. If the mean moved a lot instead, something else
changed and the comparison is not measuring what you think.

`perf` is the tool for going further:

```bash
perf stat -e cycles,instructions,cache-misses,cache-references,\
dTLB-load-misses,branch-misses,LLC-load-misses \
  ./build/cpp/bench_pipeline --capture <capture> --cpu 4

perf record -e cycles:pp -C 4 -- ./build/cpp/bench_pipeline --capture <capture> --cpu 4
perf report --stdio
```

The two counters worth watching for this workload are `dTLB-load-misses` and
`LLC-load-misses`: they are what the capacity sizing is really about, and they
are how the oversized-default regression was diagnosed.
