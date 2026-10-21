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
#include <thread>
#include <utility>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

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
    static void FollowEpoch(std::uint64_t token);
    static unsigned Committers();

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {});
    bool AttachSampleDump(std::uint64_t address, std::uint32_t queue, Range write);
    bool AttachLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint32_t opcode, std::uint32_t queue);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    void Stop();
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    bool Busy() const { return outstanding.load(std::memory_order_acquire) != 0; }
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    static std::uint64_t HeldReadQueries();
    void ReleaseRunningReads();

private:
    struct Follower {
        std::uint64_t address = 0;
        std::uint64_t end = 0;
        std::array<std::byte, 8> bytes{};
        std::uint8_t size = 0;
        std::uint8_t opcode = 0;
    };
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        std::vector<Follower> followers;
        std::uint32_t followerQueue = 0;
    };
    bool attach(const Follower& follower, std::uint32_t queue);
    DrawPipeline() = default;
    void run();
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::vector<Item> finished;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::uint32_t roomWaiters = 0;
    std::uint32_t drainWaiters = 0;
    bool committerWaiting = false;
    bool stopped = false;
    bool closing = false;
    std::thread thread;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t attachedDumps = 0;
    std::uint64_t attachedLabels = 0;
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
