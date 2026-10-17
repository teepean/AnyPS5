#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <sched.h>
#endif

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void PinWorkerThread(const char* role) {
    KeepThreadsInCacheDomain(true);
    TracyZone::NameThread(role);
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(role);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", role, static_cast<unsigned long>(nameResult));
#endif
    const auto mask = WorkerAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

void KeepThreadsInCacheDomain(bool now) {
#ifdef __linux__
    const auto& placement = CpuTopology::ProcessCacheDomain();
    if (placement.cpus == 0) return;
    static std::mutex mutex;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    static std::chrono::steady_clock::time_point checked{};
    static std::vector<long> placed;
    const auto at = std::chrono::steady_clock::now();
    if (!now && at - checked < std::chrono::seconds(1)) return;
    const bool first = checked.time_since_epoch().count() == 0;
    checked = at;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if ((placement.cpus >> cpu) & 1u) CPU_SET(cpu, &set);
    std::vector<long> seen;
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) return;
    while (const dirent* entry = readdir(tasks)) {
        const long tid = std::strtol(entry->d_name, nullptr, 10);
        if (tid <= 0) continue;
        seen.push_back(tid);
        if (std::find(placed.begin(), placed.end(), tid) != placed.end()) continue;
        const bool applied = sched_setaffinity(static_cast<pid_t>(tid), sizeof set, &set) == 0;
        if (CpuTopology::Trace()) std::fprintf(stderr, "[affinity] thread %ld -> cache domain 0x%llx%s\n", tid, static_cast<unsigned long long>(placement.cpus), applied ? "" : " (not applied)");
    }
    closedir(tasks);
    placed.swap(seen);
    if (first) std::fprintf(stderr, "[affinity] process kept in cache domain 0x%llx (%u cores, %u domains)\n", static_cast<unsigned long long>(placement.cpus), placement.cores, placement.domains);
#else
    (void)now;
#endif
}

}
