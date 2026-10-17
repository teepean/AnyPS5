#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}

namespace {

class PreparationPool {
public:
    void Submit(std::function<void()> job) {
        std::lock_guard lock(mutex);
        if (threads.empty()) {
            for (std::size_t index = 0; index < DrawPreparation::Threads(); ++index) threads.emplace_back([this] { run(); });
        }
        jobs.push_back(std::move(job));
        ++pending;
        ready.notify_one();
    }
    void AwaitIdle() {
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return pending == 0; });
    }

private:
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            ready.wait(lock, [&] { return !jobs.empty(); });
            auto job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
            if (--pending == 0) done.notify_all();
        }
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable done;
    std::deque<std::function<void()>> jobs;
    std::size_t pending = 0;
    std::vector<std::thread> threads;
};

PreparationPool& preparationPool() {
    static auto* pool = new PreparationPool();
    return *pool;
}

}

std::size_t DrawPreparation::Threads() {
    static const std::size_t threads = [] {
        const char* text = std::getenv("APS5_PREP_THREADS");
        return text != nullptr ? static_cast<std::size_t>(std::min<unsigned long long>(std::strtoull(text, nullptr, 10), 16)) : std::size_t{0};
    }();
    return DrawPipeline::Depth() != 0 ? threads : 0;
}

void DrawPreparation::Submit(std::function<void()> job) {
    preparationPool().Submit(std::move(job));
}

void DrawPreparation::AwaitIdle() {
    if (Threads() != 0) preparationPool().AwaitIdle();
}

namespace {

// Destroys finished commits (their captured preparation state) off the committer, which only moves
// them here. Never stopped (process lifetime, like the committer).
class CommitReclaim {
public:
    void Release(DrawPipeline::Commit commit) {
        if (!commit) return;
        {
            std::lock_guard lock(mutex);
            if (!stopped) {
                if (!thread.joinable()) thread = std::thread([this] { run(); });
                released.push_back(std::move(commit));
                if (released.size() == 32) ready.notify_one();
                return;
            }
        }
        commit = nullptr;
    }

    void Stop() {
        std::deque<DrawPipeline::Commit> taken;
        {
            std::unique_lock lock(mutex);
            stopped = true;
            idle.wait(lock, [&] { return !clearing; });
            taken.swap(released);
        }
        taken.clear();
    }

private:
    void run() {
        std::deque<DrawPipeline::Commit> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                ready.wait_for(lock, std::chrono::milliseconds(2), [&] { return released.size() >= 32; });
                batch.swap(released);
                clearing = !batch.empty();
            }
            if (batch.empty()) continue;
            batch.clear();
            {
                std::lock_guard lock(mutex);
                clearing = false;
            }
            idle.notify_all();
        }
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable idle;
    std::deque<DrawPipeline::Commit> released;
    bool clearing = false;
    bool stopped = false;
    std::thread thread;
};

CommitReclaim& commitReclaim() {
    static auto* reclaim = new CommitReclaim();
    return *reclaim;
}

bool ReclaimCommits() {
    static const bool enabled = std::getenv("APS5_NO_COMMIT_RECLAIM") == nullptr;
    return enabled;
}

}

void DrawPipeline::StopReclaim() {
    commitReclaim().Stop();
}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{8};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{8} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

std::uint64_t& DrawPipeline::PreparingTicket() {
    static thread_local std::uint64_t ticket = 0;
    return ticket;
}

std::uint64_t& DrawPipeline::PreparingEpoch() {
    static thread_local std::uint64_t epoch = 0;
    return epoch;
}

std::uint64_t& DrawPipeline::PreparingVisibleFrom() {
    static thread_local std::uint64_t from = 0;
    return from;
}

std::uint64_t DrawPipeline::FirstUnfilled() {
    std::lock_guard lock(mutex);
    for (const auto& item : items) {
        if (!item.ready) return item.ticket;
    }
    return nextTicket;
}

bool DrawPipeline::EarlierWritesOverlap(std::uint64_t ticket, std::uint64_t visibleFrom, std::span<const Range> reads) {
    if (visibleFrom >= ticket) return false;
    std::unique_lock lock(mutex);
    const auto earlierUnfilled = [&] {
        for (const auto& item : items) {
            if (item.ticket == ticket) return false;
            if (!item.ready) return true;
        }
        return false;
    };
    if (earlierUnfilled()) {
        const auto start = std::chrono::steady_clock::now();
        ++fillWaiters;
        filled.wait(lock, [&] { return !earlierUnfilled(); });
        --fillWaiters;
        turnWaitNs += elapsedNs(start);
    }
    const auto overlaps = [&](const std::vector<Range>& writes) {
        for (const auto& [begin, end] : writes) {
            for (const auto& [readBegin, readEnd] : reads) {
                if (begin < readEnd && readBegin < end) return true;
            }
        }
        return false;
    };
    // The history no longer reaches back to the preparation's start: assume an overlap.
    bool hit = visibleFrom < historyFrom;
    for (const auto& [done, writes] : history) {
        if (hit) break;
        if (done >= visibleFrom && done < ticket) hit = overlaps(writes);
    }
    for (const auto& item : items) {
        if (hit || item.ticket == ticket) break;
        if (item.ticket >= visibleFrom && item.ticket < ticket) hit = overlaps(item.writes);
    }
    if (hit) ++redone;
    return hit;
}

std::deque<DrawPipeline::Item>::iterator DrawPipeline::reservation(std::uint64_t ticket) {
    return std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.ticket == ticket; });
}

std::uint64_t DrawPipeline::Reserve() {
    std::unique_lock lock(mutex);
    rethrowFailure();
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the reserving thread holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        ++roomWaiters;
        room.wait(lock, [&] { return items.size() < Depth(); });
        --roomWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    const auto ticket = nextTicket++;
    Item item;
    item.ticket = ticket;
    item.ready = false;
    depthSum += items.size();
    items.push_back(std::move(item));
    ++unready;
    ++reserved;
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) thread = std::thread([this] { run(); });
    return ticket;
}

void DrawPipeline::Cancel(std::uint64_t ticket) {
    std::lock_guard lock(mutex);
    const auto found = reservation(ticket);
    if (found == items.end() || found->ready) return;
    found->ready = true;
    --unready;
    if (found == items.begin() && committerWaiting) wake.notify_one();
    if (fillWaiters != 0) filled.notify_all();
}

void DrawPipeline::Fail(std::exception_ptr error) {
    std::lock_guard lock(mutex);
    if (failure == nullptr) failure = error;
}

void DrawPipeline::AwaitPrepared() {
    std::unique_lock lock(mutex);
    if (unready == 0) return;
    const auto start = std::chrono::steady_clock::now();
    ++fillWaiters;
    filled.wait(lock, [&] { return unready == 0; });
    --fillWaiters;
    ++preparedWaits;
    preparedWaitNs += elapsedNs(start);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::unique_lock lock(mutex);
    rethrowFailure();
    if (const auto ticket = PreparingTicket(); ticket != 0) {
        // A preparation fills its own place; anything more it enqueues follows that place.
        auto found = reservation(ticket);
        if (found == items.end()) throw std::runtime_error("draw preparation enqueued without its reservation");
        ++enqueued;
        if (!found->ready) {
            found->commit = std::move(commit);
            found->writes = std::move(writes);
            found->labelAddress = labelAddress;
            found->labelBytes = std::move(labelBytes);
            found->ready = true;
            --unready;
            if (found == items.begin() && committerWaiting) wake.notify_one();
            if (fillWaiters != 0) filled.notify_all();
            return;
        }
        auto last = found;
        while (std::next(last) != items.end() && std::next(last)->ticket == ticket) ++last;
        Item item{std::move(commit), std::move(writes), labelAddress, std::move(labelBytes)};
        item.ticket = ticket;
        items.insert(std::next(last), std::move(item));
        outstanding.fetch_add(1, std::memory_order_release);
        return;
    }
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the enqueuing thread holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        ++roomWaiters;
        room.wait(lock, [&] { return items.size() < Depth(); });
        --roomWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes)});
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) thread = std::thread([this] { run(); });
    if (committerWaiting) wake.notify_one();
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    std::fprintf(stderr, "[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room; %llu prepared in parallel (%llu redone after an earlier one wrote their reads), committer waited %.0f ms for preparations, preparations waited %.0f ms for their turn, the worker waited %llu times (%.0f ms) for outstanding preparations; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, static_cast<unsigned long long>(reserved), static_cast<unsigned long long>(redone), static_cast<double>(committerStallNs) / 1e6, static_cast<double>(turnWaitNs) / 1e6, static_cast<unsigned long long>(preparedWaits), static_cast<double>(preparedWaitNs) / 1e6, drainText.c_str());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = reserved = committerStallNs = turnWaitNs = redone = preparedWaits = preparedWaitNs = 0;
    drains = {};
    drainWaitNs = {};
}

void DrawPipeline::run() {
    for (;;) {
        Commit* commit = nullptr;
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            if (!items.empty() && !items.front().ready) {
                const auto stall = std::chrono::steady_clock::now();
                wake.wait(lock, [&] { return !items.empty() && items.front().ready; });
                committerStallNs += elapsedNs(stall);
            }
            wake.wait(lock, [&] { return !items.empty() && items.front().ready; });
            committerWaiting = false;
            commit = stopped || !items.front().commit ? nullptr : &items.front().commit;
        }
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        try {
            if (commit != nullptr) (*commit)();
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto spent = elapsedNs(start);
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        Commit finished;
        {
            std::lock_guard lock(mutex);
            if (const auto ticket = items.front().ticket; ticket != 0) {
                constexpr std::size_t HistoryEntries = 4096;
                if (!items.front().writes.empty()) history.emplace_back(ticket, std::move(items.front().writes));
                while (history.size() > HistoryEntries) {
                    historyFrom = history.front().first + 1;
                    history.pop_front();
                }
            }
            // The commit's captures are destroyed outside the lock (on the reclaim thread by default).
            finished = std::move(items.front().commit);
            items.pop_front();
            ++commits;
            commitNs += spent;
            if (failed) ++commitErrors;
            if (fatal != nullptr && failure == nullptr) failure = fatal;
            outstanding.fetch_sub(1, std::memory_order_release);
            // The worker waiting for room wakes at half depth and refills in a burst.
            if (roomWaiters != 0 && items.size() <= Depth() / 2) room.notify_one();
            if (idleWaiters != 0) idle.notify_all();
        }
        if (ReclaimCommits()) commitReclaim().Release(std::move(finished));
    }
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    if (!Busy()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    if (const auto ticket = PreparingTicket(); ticket != 0) {
        // A preparation waits for its turn: every place before its own has run.
        idle.wait(lock, [&] { return items.empty() || items.front().ticket == ticket || reservation(ticket) == items.end(); });
        turnWaitNs += elapsedNs(start);
    } else {
        idle.wait(lock, [&] { return items.empty(); });
    }
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

bool DrawPipeline::OverlapsKnown(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (const auto& item : items) {
        if (!item.ready) continue;
        for (const auto& [begin, limit] : item.writes) {
            if (address < limit && begin < end) return true;
        }
    }
    return false;
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes, bool* unfilled) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        if (item->ticket != 0 && item->ticket == PreparingTicket()) continue;
        if (!item->ready) {
            if (unfilled != nullptr) *unfilled = true;
            return std::nullopt;
        }
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    const auto ticket = PreparingTicket();
    if (ticket != 0) {
        // A preparation: only the filled places before its own; earlier places still being prepared
        // are checked at its turn (EarlierWritesOverlap).
        for (const auto& item : items) {
            if (item.ticket == ticket) return false;
            if (!item.ready) continue;
            for (const auto& [begin, limit] : item.writes) {
                if (address < limit && begin < end) return true;
            }
        }
        return false;
    }
    // The worker: the writes of a draw still being prepared are not known yet.
    if (unready != 0) return true;
    for (const auto& item : items) {
        for (const auto& [begin, limit] : item.writes) {
            if (address < limit && begin < end) return true;
        }
    }
    return false;
}

}
