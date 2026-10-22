#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"

namespace AgcDriver::DriverDetail {

std::shared_ptr<VulkanDevice> DevicePointer::Load() const { return pointer.load(std::memory_order_acquire); }

DevicePointer::operator std::shared_ptr<VulkanDevice>() const { return Load(); }

std::shared_ptr<VulkanDevice> DevicePointer::LoadCached() const {
    struct Slot {
        const DevicePointer* owner = nullptr;
        VulkanDevice* device = nullptr;
        std::weak_ptr<VulkanDevice> held;
    };
    thread_local Slot slot;
    if (auto* const current = installed.load(std::memory_order_acquire); current != nullptr && slot.owner == this && slot.device == current) {
        if (auto pinned = slot.held.lock()) return pinned;
    }
    auto loaded = Load();
    slot.owner = this;
    slot.device = loaded.get();
    slot.held = loaded;
    return loaded;
}

VulkanDevice* DevicePointer::UnderGpuLock() const {
    GuestMemory::AssertGpuLockHeld("DevicePointer::UnderGpuLock");
    return installed.load(std::memory_order_acquire);
}

DevicePointer& DevicePointer::operator=(std::shared_ptr<VulkanDevice> value) {
    auto* const next = value.get();
    const auto previous = pointer.exchange(std::move(value), std::memory_order_acq_rel);
    installed.store(next, std::memory_order_release);
    return *this;
}

void DevicePointer::Reset() { *this = nullptr; }

VulkanDevice* DevicePointer::operator->() const { return Load().get(); }

DevicePointer::operator bool() const { return Load() != nullptr; }

bool DevicePointer::operator==(std::nullptr_t) const { return Load() == nullptr; }

void DeviceUseGate::lock_shared() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    ++users;
}

void DeviceUseGate::unlock_shared() {
    std::lock_guard lock(mutex);
    if (--users == 0) changed.notify_all();
}

void DeviceUseGate::lock() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    replacing = true;
    changed.wait(lock, [&] { return users == 0; });
}

void DeviceUseGate::unlock() {
    std::lock_guard lock(mutex);
    replacing = false;
    changed.notify_all();
}

}
