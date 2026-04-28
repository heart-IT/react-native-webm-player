#pragma once

#include <mutex>
#include <memory>
#include <jsi/jsi.h>

namespace media {

/**
 * Thread-safe single-slot registry for a JSI module.
 *
 * Previously a per-runtime hash map to handle "multiple JS engines during hot
 * reload". In practice there is only ever one runtime per process; the extra
 * machinery was unused. Reduced to a single shared_ptr with a runtime-ID guard
 * so mismatched uninstalls are a no-op (matches prior behaviour).
 *
 * All access is from the JS thread during install/uninstall — the mutex is
 * defensive rather than contended.
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
        runtimeId_ = runtimeId;
        module_ = std::move(module);
    }

    void unregisterModule(uintptr_t runtimeId) {
        std::lock_guard<std::mutex> lk(lock_);
        if (runtimeId_ == runtimeId) {
            module_.reset();
            runtimeId_ = 0;
        }
    }

    std::shared_ptr<ModuleType> getModule(uintptr_t runtimeId) {
        std::lock_guard<std::mutex> lk(lock_);
        return runtimeId_ == runtimeId ? module_ : nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(lock_);
        module_.reset();
        runtimeId_ = 0;
    }

    static uintptr_t getRuntimeId(facebook::jsi::Runtime& rt) {
        return reinterpret_cast<uintptr_t>(&rt);
    }

private:
    ModuleRegistry() = default;
    std::mutex lock_;
    uintptr_t runtimeId_ = 0;
    std::shared_ptr<ModuleType> module_;
};

}  // namespace media
