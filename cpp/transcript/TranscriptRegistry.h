// Singleton registry for transcript text delivery to native views and JS callbacks.
// Transcript thread pushes text; registered consumers receive updates.
// Supports multiple concurrent callbacks (native view + JS callback).
//
// Modeled on VideoSurfaceRegistry — mutex-protected register/unregister.
//
// Callback lifetime contract: callbacks MUST self-guard via weak_ptr (or equivalent)
// before touching any captured state — a callback snapshot taken during pushSegment()
// can fire after clearCallback() returns. Upstream callers in PipelineOrchestrator
// satisfy this via weakOwner_.lock() checks.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
        // Wrap in shared_ptr so the pushSegment snapshot is a pointer bump, not
        // a std::function copy (which could heap-allocate on captures past SBO).
        auto shared = callback ? std::make_shared<TranscriptCallback>(std::move(callback)) : nullptr;
        std::lock_guard<std::mutex> lk(mtx_);
        callbacks_[static_cast<size_t>(slot)] = std::move(shared);
    }

    void clearCallback(CallbackSlot slot) {
        std::lock_guard<std::mutex> lk(mtx_);
        callbacks_[static_cast<size_t>(slot)] = nullptr;
    }

    // Called from transcript thread with new text
    void pushSegment(const TranscriptSegment& segment) {
        std::shared_ptr<TranscriptCallback> cbs[static_cast<size_t>(CallbackSlot::Count)];
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // O(1) pop-front via deque — prior std::vector::erase(begin()) was O(N).
            if (history_.size() >= kMaxHistory) {
                history_.pop_front();
            }
            history_.push_back(segment);
            for (size_t i = 0; i < static_cast<size_t>(CallbackSlot::Count); ++i) {
                cbs[i] = callbacks_[i];
            }
        }
        // Fire callbacks outside lock
        for (auto& cb : cbs) {
            if (cb && *cb) (*cb)(segment);
        }
    }

    std::vector<TranscriptSegment> getHistory() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::vector<TranscriptSegment>(history_.begin(), history_.end());
    }

    void clearHistory() {
        std::lock_guard<std::mutex> lk(mtx_);
        history_.clear();
    }

private:
    TranscriptRegistry() = default;

    static constexpr size_t kMaxHistory = 200;
    mutable std::mutex mtx_;
    std::shared_ptr<TranscriptCallback> callbacks_[static_cast<size_t>(CallbackSlot::Count)]{};
    std::deque<TranscriptSegment> history_;
};

}  // namespace media::transcript
