// JSI bindings for stream-lifecycle methods (data flow + start/stop/pause/...).
//
// Include-only-from: PipelineOrchestrator.h. Defined as out-of-class member
// functions; `inline` is required to satisfy ODR when the parent header is
// included in multiple translation units (iOS + Android platform modules).
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerLifecycleMethods() {
    auto& t = dispatch_;

    // Data flow
    t.method("feedData", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1) throw jsi::JSError(rt, "feedData requires (buffer)");
        return feedData(rt, args[0]);
    });

    // Lifecycle
    t.method("start", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        return jsi::Value(ensureRunning());
    });
    t.method("stop", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        bool joined = stopIngestThread();
        session_.stop();
        destroyRingBuffer();
        if (joined) demuxer_.reset();
        return jsi::Value(true);
    });
    t.method("isRunning", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        return jsi::Value(session_.isRunning());
    });
    t.method("warmUp", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        return jsi::Value(session_.warmUp());
    });
    t.method("pause", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        return jsi::Value(session_.pause());
    });
    t.method("resume", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        bool resumed = session_.resume();
        // Drop the render-clock anchor so the paused interval is not
        // treated as elapsed wall-time (fixes P1-6 frame burst on resume).
        if (resumed && videoQueue_) videoQueue_->signalClockReset();
        return jsi::Value(resumed);
    });
    t.method("isPaused", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        return jsi::Value(session_.isPaused());
    });
    t.method("resetStream", 0, [this](jsi::Runtime& rt, const jsi::Value*, size_t) {
        return resetStream(rt);
    });
}

}  // namespace mediamodule
