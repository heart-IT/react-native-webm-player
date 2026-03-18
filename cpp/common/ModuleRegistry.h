#pragma once

#include <mutex>
#include <memory>
#include <unordered_map>
#include <jsi/jsi.h>

namespace media {

/**
 * Thread-safe registry for JSI modules across multiple JS runtimes.
 *
 * React Native can have multiple JS engines (e.g., during hot reload).
 * This registry tracks module instances per runtime to prevent leaks
 * and ensure proper cleanup.
 *
 * Uses std::mutex (not lock-free) because all access is from the JS thread
 * during install/uninstall. Zero contention in practice — the simplicity
 * outweighs the cost of a lock-free map.
 */
template<typename ModuleType>
class ModuleRegistry {
public:
    static ModuleRegistry& instance() noexcept {
        static ModuleRegistry registry;
        return registry;
    }

    void registerModule(uintptr_t runtimeId, std::shared_ptr<ModuleType> module) {
        std::lock_guard<std::mutex> lk(lock_);
        modules_[runtimeId] = std::move(module);
    }

    void unregisterModule(uintptr_t runtimeId) {
        std::lock_guard<std::mutex> lk(lock_);
        modules_.erase(runtimeId);
    }

    std::shared_ptr<ModuleType> getModule(uintptr_t runtimeId) {
        std::lock_guard<std::mutex> lk(lock_);
        auto it = modules_.find(runtimeId);
        return it != modules_.end() ? it->second : nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(lock_);
        modules_.clear();
    }

    static uintptr_t getRuntimeId(facebook::jsi::Runtime& rt) {
        return reinterpret_cast<uintptr_t>(&rt);
    }

private:
    ModuleRegistry() = default;
    std::mutex lock_;
    std::unordered_map<uintptr_t, std::shared_ptr<ModuleType>> modules_;
};

}  // namespace media
