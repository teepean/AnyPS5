#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

// Pool of threads that prepare queue 0's draws in parallel (APS5_PREP_THREADS=<n>, 0 or unset: off;
// needs the draw pipeline). Jobs start in submission order; AwaitIdle waits until every submitted
// job has returned.
class DrawPreparation {
public:
    static std::size_t Threads();
    static void Submit(std::function<void()> job);
    static void AwaitIdle();
};

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static std::size_t QueuedItems();
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    static void StopReclaim();
    static void FollowEpoch(std::uint64_t token);

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {});
    // Parallel draw preparation (APS5_PREP_THREADS): the queue worker reserves a draw's place with
    // Reserve, and the thread that prepares the draw (Driver::draw) fills it through Enqueue while
    // that thread's PreparingTicket is the reservation's ticket, or Cancels it. The committer runs
    // places strictly in order and waits at an unfilled one, so work the worker enqueues later (labels,
    // DMA) never overtakes a draw still being prepared. A preparation's Drain waits only for the
    // places before its own (it holds the next turn), the worker's for every place.
    std::uint64_t Reserve();
    void Cancel(std::uint64_t ticket);
    void Fail(std::exception_ptr error);
    // Waits until no reservation is unfilled (the worker, before a submission's packets go away).
    void AwaitPrepared();
    static std::uint64_t& PreparingTicket();
    // The worker's epoch token when the preparing draw's place was reserved (the commit follows it).
    static std::uint64_t& PreparingEpoch();
    // The first place not yet filled when the preparation started: every earlier one was visible to
    // its capture checks (Overlaps).
    static std::uint64_t& PreparingVisibleFrom();
    std::uint64_t FirstUnfilled();
    // At a preparation's turn to enqueue: waits until every earlier place is filled, then reports
    // whether a place filled after the preparation started (its writes unknown to the capture
    // checks) writes memory the preparation read. The preparation then redoes its draw in its turn.
    bool EarlierWritesOverlap(std::uint64_t ticket, std::uint64_t visibleFrom, std::span<const Range> reads);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    bool Busy() const { return outstanding.load(std::memory_order_acquire) != 0; }
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    // Overlaps with the writes known now: places still being prepared count as not overlapping.
    // For checks that cannot change what the packet reads (an indirect register list is read at
    // submit, Driver::readRegisterLists).
    bool OverlapsKnown(std::uint64_t address, std::size_t bytes);
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes, bool* unfilled = nullptr);

private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        // A reservation's ticket (0 for work the worker enqueued directly) and whether it is filled.
        std::uint64_t ticket = 0;
        bool ready = true;
    };
    std::deque<Item>::iterator reservation(std::uint64_t ticket);
    DrawPipeline() = default;
    void run();
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    // The worker waiting for room (Reserve, Enqueue): woken one per finished place, apart from the
    // waiters on `idle` (drains, turns, fills).
    std::condition_variable room;
    std::uint32_t roomWaiters = 0;
    // Waiters for a fill (EarlierWritesOverlap, AwaitPrepared), woken by fills and cancels only.
    std::condition_variable filled;
    std::uint32_t fillWaiters = 0;
    std::deque<Item> items;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::uint64_t nextTicket = 1;
    // Writes of reserved places the committer already ran (ticket, writes), newest last, for
    // EarlierWritesOverlap; bounded.
    std::deque<std::pair<std::uint64_t, std::vector<Range>>> history;
    std::uint64_t historyFrom = 1;
    std::uint64_t redone = 0;
    std::size_t unready = 0;
    std::uint64_t reserved = 0;
    std::uint64_t turnWaitNs = 0;
    std::uint64_t committerStallNs = 0;
    std::uint64_t preparedWaits = 0;
    std::uint64_t preparedWaitNs = 0;
    std::uint32_t idleWaiters = 0;
    bool committerWaiting = false;
    bool stopped = false;
    std::thread thread;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::uint64_t depthSum = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

}

#endif
