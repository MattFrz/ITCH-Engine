#pragma once

// Optional low-latency runtime tuning.
//
// Everything here is opt-in and every part of it degrades to a note rather
// than a failure. Unit tests run unprivileged and must stay that way, so
// nothing in this header is required for the engine to work - it only removes
// jitter when the operating system lets it.
//
// What it covers, and what it deliberately does not:
//
//   CPU affinity      done here (sched_setaffinity / SetThreadAffinityMask)
//   NUMA affinity     done here, by resolving a node's CPU list from sysfs and
//                     pinning inside it - no libnuma dependency
//   memory locking    done here (mlockall), needs CAP_IPC_LOCK or a raised
//                     RLIMIT_MEMLOCK
//   real-time sched   done here (SCHED_FIFO), needs CAP_SYS_NICE
//   huge pages        done here for the process heap hint; the reliable form
//                     is a boot-time reservation, documented not coded
//   IRQ affinity      NOT done here. Steering NIC interrupts is a root-level
//                     system change that belongs in a runbook, not in a
//                     library that a unit test links. See docs/linux_tuning.md
//   CPU governor      likewise: documented, not silently changed
//   isolcpus/nohz     boot parameters, documented

#include <string>
#include <vector>

namespace itch {
namespace runtime {

struct RuntimeTuning {
    int cpu = -1;         // pin to this logical CPU; -1 leaves scheduling alone
    int numa_node = -1;   // pin within this NUMA node (ignored if cpu >= 0)
    bool lock_memory = false;   // mlockall(MCL_CURRENT | MCL_FUTURE)
    bool realtime = false;      // SCHED_FIFO
    int rt_priority = 50;       // 1..99 on Linux
    bool prefault_heap = false; // touch and hold a slab so the first events do
                                // not pay for page faults
    std::size_t prefault_bytes = 64u * 1024u * 1024u;
};

struct TuningReport {
    bool pinned = false;
    int pinned_cpu = -1;
    bool memory_locked = false;
    bool realtime = false;
    bool prefaulted = false;
    // Human-readable outcome for every step attempted, successful or not. The
    // tools print this so a benchmark run always states the conditions it ran
    // under instead of implying tuning that did not happen.
    std::vector<std::string> notes;
};

TuningReport apply_tuning(const RuntimeTuning& tuning);

bool pin_current_thread(int cpu, std::string* error);
bool pin_current_thread_to_numa_node(int node, std::string* error, int* chosen_cpu);
bool lock_all_memory(std::string* error);
bool set_realtime_priority(int priority, std::string* error);
bool prefault(std::size_t bytes, std::string* error);

int current_cpu();
int hardware_concurrency();
std::vector<int> cpus_on_numa_node(int node);

// One line naming the platform and which knobs this build can even attempt.
std::string describe_runtime_support();

}  // namespace runtime
}  // namespace itch
