#include "itch_engine/runtime/affinity.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace itch {
namespace runtime {

namespace {
std::string err_text(int e) { return "errno " + std::to_string(e); }
}  // namespace

int hardware_concurrency() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<int>(n);
}

int current_cpu() {
#if defined(__linux__)
    return ::sched_getcpu();
#elif defined(_WIN32)
    return static_cast<int>(GetCurrentProcessorNumber());
#else
    return -1;
#endif
}

std::vector<int> cpus_on_numa_node(int node) {
    std::vector<int> cpus;
#if defined(__linux__)
    // Read the node's cpulist from sysfs rather than linking libnuma: one file,
    // no dependency, and it is the same source libnuma reads.
    std::ostringstream path;
    path << "/sys/devices/system/node/node" << node << "/cpulist";
    std::ifstream f(path.str());
    if (!f) return cpus;
    std::string list;
    std::getline(f, list);
    std::size_t pos = 0;
    while (pos < list.size()) {
        std::size_t comma = list.find(',', pos);
        std::string part = list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        const std::size_t dash = part.find('-');
        if (dash == std::string::npos) {
            if (!part.empty()) cpus.push_back(std::atoi(part.c_str()));
        } else {
            const int lo = std::atoi(part.substr(0, dash).c_str());
            const int hi = std::atoi(part.substr(dash + 1).c_str());
            for (int c = lo; c <= hi; ++c) cpus.push_back(c);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
#else
    (void)node;
#endif
    return cpus;
}

bool pin_current_thread(int cpu, std::string* error) {
    if (cpu < 0) {
        if (error) *error = "no cpu requested";
        return false;
    }
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        if (error) *error = "pthread_setaffinity_np: " + err_text(rc);
        return false;
    }
    return true;
#elif defined(_WIN32)
    // Windows affinity masks are per processor group; 64 CPUs per group is the
    // limit of this simple form, which covers every machine this is used on.
    if (cpu >= 64) {
        if (error) *error = "cpu >= 64 needs a processor group API call";
        return false;
    }
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        if (error) *error = "SetThreadAffinityMask failed";
        return false;
    }
    return true;
#else
    if (error) *error = "cpu pinning unsupported on this platform";
    return false;
#endif
}

bool pin_current_thread_to_numa_node(int node, std::string* error, int* chosen_cpu) {
    const std::vector<int> cpus = cpus_on_numa_node(node);
    if (cpus.empty()) {
        if (error) *error = "no cpu list for numa node " + std::to_string(node);
        return false;
    }
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        if (error) *error = "pthread_setaffinity_np: " + err_text(rc);
        return false;
    }
    if (chosen_cpu) *chosen_cpu = cpus.front();
    return true;
#else
    (void)error;
    (void)chosen_cpu;
    return false;
#endif
}

bool lock_all_memory(std::string* error) {
#if defined(__linux__)
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        if (error) {
            *error = "mlockall: " + err_text(errno) +
                     " (needs CAP_IPC_LOCK or a raised RLIMIT_MEMLOCK)";
        }
        return false;
    }
    return true;
#elif defined(_WIN32)
    // SetProcessWorkingSetSize plus VirtualLock is the Windows analogue and
    // needs SeLockMemoryPrivilege; not attempted because Windows is the
    // development platform here, not the deployment one.
    if (error) *error = "memory locking not implemented on Windows (development platform)";
    return false;
#else
    if (error) *error = "memory locking unsupported on this platform";
    return false;
#endif
}

bool set_realtime_priority(int priority, std::string* error) {
#if defined(__linux__)
    sched_param param{};
    param.sched_priority = priority;
    const int rc = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        if (error) *error = "SCHED_FIFO: " + err_text(rc) + " (needs CAP_SYS_NICE)";
        return false;
    }
    return true;
#elif defined(_WIN32)
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) == 0) {
        if (error) *error = "SetThreadPriority failed";
        return false;
    }
    (void)priority;
    return true;
#else
    (void)priority;
    if (error) *error = "real-time scheduling unsupported on this platform";
    return false;
#endif
}

bool prefault(std::size_t bytes, std::string* error) {
    // Touch every page of a slab and hold it, so the first market events do not
    // pay for demand paging. Kept alive in a function-local static for the
    // lifetime of the process.
    static std::vector<unsigned char> slab;
    try {
        slab.assign(bytes, 0);
    } catch (...) {
        if (error) *error = "prefault allocation failed";
        return false;
    }
    const std::size_t page = 4096;
    volatile unsigned char sink = 0;
    for (std::size_t i = 0; i < slab.size(); i += page) {
        slab[i] = 1;
        sink = static_cast<unsigned char>(sink + slab[i]);
    }
    (void)sink;
    return true;
}

TuningReport apply_tuning(const RuntimeTuning& tuning) {
    TuningReport report;
    std::string err;

    if (tuning.cpu >= 0) {
        if (pin_current_thread(tuning.cpu, &err)) {
            report.pinned = true;
            report.pinned_cpu = tuning.cpu;
            report.notes.push_back("pinned to cpu " + std::to_string(tuning.cpu));
        } else {
            report.notes.push_back("cpu pinning FAILED: " + err);
        }
    } else if (tuning.numa_node >= 0) {
        int chosen = -1;
        if (pin_current_thread_to_numa_node(tuning.numa_node, &err, &chosen)) {
            report.pinned = true;
            report.pinned_cpu = chosen;
            report.notes.push_back("pinned to numa node " + std::to_string(tuning.numa_node));
        } else {
            report.notes.push_back("numa pinning FAILED: " + err);
        }
    } else {
        report.notes.push_back("no cpu pinning requested (latency tails will be wider)");
    }

    if (tuning.lock_memory) {
        if (lock_all_memory(&err)) {
            report.memory_locked = true;
            report.notes.push_back("memory locked (mlockall)");
        } else {
            report.notes.push_back("memory locking FAILED: " + err);
        }
    }

    if (tuning.realtime) {
        if (set_realtime_priority(tuning.rt_priority, &err)) {
            report.realtime = true;
            report.notes.push_back("real-time scheduling enabled");
        } else {
            report.notes.push_back("real-time scheduling FAILED: " + err);
        }
    }

    if (tuning.prefault_heap) {
        if (prefault(tuning.prefault_bytes, &err)) {
            report.prefaulted = true;
            report.notes.push_back("prefaulted " + std::to_string(tuning.prefault_bytes / 1048576) +
                                   " MiB");
        } else {
            report.notes.push_back("prefault FAILED: " + err);
        }
    }

    return report;
}

std::string describe_runtime_support() {
#if defined(__linux__)
    return "linux: cpu affinity yes, numa yes (sysfs), mlockall yes, SCHED_FIFO yes";
#elif defined(_WIN32)
    return "windows: cpu affinity yes, numa no, memory locking no, priority boost only";
#else
    return "unknown platform: no runtime tuning available";
#endif
}

}  // namespace runtime
}  // namespace itch
