#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Base = std::uint64_t{1} << 32;
constexpr std::uint64_t Watched = 4 * Block;
constexpr std::chrono::seconds Patience{10};
constexpr std::chrono::milliseconds Settle{200};

struct FakeWatch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    std::vector<std::thread::id> walkers;
    bool holdNextWalk = false;
    bool holding = false;
    bool released = false;
    bool timedOut = false;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}

namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return address >= Base && bytes <= Watched && address - Base <= Watched - bytes;
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        std::unique_lock lock(fake.mutex);
        fake.walkers.push_back(std::this_thread::get_id());
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
        if (fake.holdNextWalk) {
            fake.holdNextWalk = false;
            fake.holding = true;
            fake.changed.notify_all();
            fake.timedOut = !fake.changed.wait_for(lock, Patience, [&] { return fake.released; });
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}

namespace {

using namespace AgcDriver::GuestMemory;

void CheckCollectWaitsForScan() {
    auto& fake = Fake();
    BumpCollectEpoch();
    const auto before = CollectWrites(Base, Block);
    Require(before != 0 && UnchangedSince(Base, Block, before), "the block was not collected");
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Page, Base + 2 * Page);
        fake.holdNextWalk = true;
        fake.holding = false;
        fake.released = false;
    }
    std::thread scanner([] { ScanWritesUnlocked(Base, Block); });
    bool holding = false;
    {
        std::unique_lock lock(fake.mutex);
        holding = fake.changed.wait_for(lock, Patience, [&] { return fake.holding; });
    }
    std::mutex doneMutex;
    std::condition_variable doneChanged;
    bool done = false;
    bool sawWrite = false;
    std::thread reader([&] {
        CollectWritesUncached(Base, Block);
        const bool seen = !UnchangedSince(Base + Page, Page, before);
        const std::lock_guard lock(doneMutex);
        sawWrite = seen;
        done = true;
        doneChanged.notify_all();
    });
    bool finishedEarly = false;
    {
        std::unique_lock lock(doneMutex);
        finishedEarly = doneChanged.wait_for(lock, Settle, [&] { return done; });
    }
    {
        const std::lock_guard lock(fake.mutex);
        fake.released = true;
    }
    fake.changed.notify_all();
    scanner.join();
    reader.join();
    Require(holding, "the scan did not reach the write watch");
    Require(!fake.timedOut, "the held scan was never released");
    Require(!finishedEarly, "a collect over a range a scan was holding did not wait for it");
    Require(sawWrite, "a collect that waited for a scan did not see the write the scan took");
}

void CheckScanServesLaterEpoch() {
    auto& fake = Fake();
    BumpCollectEpoch();
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Block + Page, Base + Block + 2 * Page);
        fake.walkers.clear();
    }
    const auto before = TrackerGeneration();
    Require(ScanWritesUnlocked(Base + Block, Block), "the scan of a watched block failed");
    std::size_t walks = 0;
    {
        const std::lock_guard lock(fake.mutex);
        walks = fake.walkers.size();
    }
    const auto served = CollectWrites(Base + Block, Block);
    {
        const std::lock_guard lock(fake.mutex);
        Require(fake.walkers.size() == walks, "a collect walked a block a scan in its epoch had walked");
    }
    Require(served != 0 && !UnchangedSince(Base + Block + Page, Page, before), "a collect served by a scan did not see the write the scan took");
    BumpCollectEpoch();
    CollectWrites(Base + Block, Block);
    {
        const std::lock_guard lock(fake.mutex);
        Require(fake.walkers.size() == walks + 1, "a scan from an earlier epoch served a later one");
    }
}

void NextQueueEpoch() {
    const auto handled = WriteCollectorEpochs();
    BumpCollectEpoch();
    NoteQueueEpoch();
    for (int attempt = 0; attempt < 500 && WriteCollectorEpochs() == handled; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(WriteCollectorEpochs() > handled, "the write collector did not take a queue epoch");
}

void CheckCollectorLifecycle() {
    auto& fake = Fake();
    MarkCollectDemandThread();
    NextQueueEpoch();
    Require(WriteCollectorsRunning() == 1, "the write collector did not start");
    CollectWrites(Base + 2 * Block, Block);
    {
        const std::lock_guard lock(fake.mutex);
        fake.walkers.clear();
    }
    for (int epoch = 0; epoch < 3; ++epoch) NextQueueEpoch();
    {
        const std::lock_guard lock(fake.mutex);
        Require(std::any_of(fake.walkers.begin(), fake.walkers.end(), [](const auto& id) { return id != std::this_thread::get_id(); }), "the write collector did not rescan a range collected three epochs earlier");
        fake.walkers.clear();
    }
    CollectWrites(Base + 2 * Block, Block);
    {
        const std::lock_guard lock(fake.mutex);
        Require(fake.walkers.empty(), "a collect walked a range the write collector had scanned in its epoch");
    }
    StopWriteCollector();
    Require(WriteCollectorsRunning() == 0, "the write collector kept running after its stop");
    BumpCollectEpoch();
    NoteQueueEpoch();
    Require(WriteCollectorsRunning() == 0, "the write collector restarted after its stop");
    Require(CollectWrites(Base + 3 * Block, Block) != 0, "collects stopped working with the write collector");
}

}

int main() {
    try {
        setenv("APS5_WRITE_COLLECTOR", "1", 1);
        CheckCollectWaitsForScan();
        CheckScanServesLaterEpoch();
        CheckCollectorLifecycle();
    } catch (const std::exception& error) {
        std::cerr << "collect in flight test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "collect in flight tests passed\n";
    return 0;
}
