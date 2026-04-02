// Singleton registry for transcript text delivery to native views and JS callbacks.
// Transcript thread pushes text; registered consumers receive updates.
// Supports multiple concurrent callbacks (native view + JS callback).
//
// Modeled on VideoSurfaceRegistry — mutex-protected register/unregister.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace media::transcript {

struct TranscriptSegment {
    std::string text;
    int64_t startUs = 0;
    int64_t endUs = 0;
    bool isFinal = false;
};

using TranscriptCallback = std::function<void(const TranscriptSegment& segment)>;

enum class CallbackSlot : uint8_t {
    NativeView = 0,
    JsCallback = 1,
    Count = 2
};

class TranscriptRegistry {
public:
    static TranscriptRegistry& instance() {
        static TranscriptRegistry reg;
        return reg;
    }

    void setCallback(CallbackSlot slot, TranscriptCallback callback) {
        std::lock_guard<std::mutex> lk(mtx_);
        callbacks_[static_cast<size_t>(slot)] = std::move(callback);
    }

    void clearCallback(CallbackSlot slot) {
        std::lock_guard<std::mutex> lk(mtx_);
        callbacks_[static_cast<size_t>(slot)] = nullptr;
    }

    // Called from transcript thread with new text
    void pushSegment(const TranscriptSegment& segment) {
        TranscriptCallback cbs[static_cast<size_t>(CallbackSlot::Count)];
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (history_.size() >= kMaxHistory) {
                history_.erase(history_.begin());
            }
            history_.push_back(segment);
            for (size_t i = 0; i < static_cast<size_t>(CallbackSlot::Count); ++i) {
                cbs[i] = callbacks_[i];
            }
        }
        // Fire callbacks outside lock
        for (auto& cb : cbs) {
            if (cb) cb(segment);
        }
    }

    std::vector<TranscriptSegment> getHistory() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return history_;
    }

    void clearHistory() {
        std::lock_guard<std::mutex> lk(mtx_);
        history_.clear();
    }

private:
    TranscriptRegistry() = default;

    static constexpr size_t kMaxHistory = 200;
    mutable std::mutex mtx_;
    TranscriptCallback callbacks_[static_cast<size_t>(CallbackSlot::Count)]{};
    std::vector<TranscriptSegment> history_;
};

}  // namespace media::transcript
