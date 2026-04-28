// JSI bindings for clip capture + DVR (setClipBufferDuration, captureClip,
// seekTo, getBufferRangeSeconds).
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerClipMethods() {
    auto& t = dispatch_;

    t.method("setClipBufferDuration", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setClipBufferDuration requires (seconds: number)");
        float seconds = jsiutil::safeFloatArg(rt, args[0], "seconds");
        if (seconds < 0 || seconds > media::config::clip::kMaxDurationSeconds)
            throw jsi::JSError(rt, "clip duration must be 0-120 seconds");
        // Lock against the health watchdog (decode thread) reading
        // ingestRingBuffer_ via tryGetIngestRingBuffer() — without this,
        // the watchdog could observe the shared_ptr mid-mutation. Matches
        // resetStream() and seekTo() which already acquire this mutex.
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        bool wasRunning = ingestThread_ && ingestThread_->isRunning();
        bool enabling = seconds > 0;
        bool needsRestart = wasRunning && (enabling != clipIndex_.isEnabled());
        if (needsRestart) {
            stopIngestThread();
            session_.waitForDecodeIdle();
            destroyRingBuffer();
            demuxer_.reset();
        }
        clipIndex_.setEnabled(enabling);
        if (needsRestart) startIngestThread();
        return jsi::Value(true);
    });
    t.method("captureClip", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "captureClip requires (seconds: number)");
        float seconds = jsiutil::safeFloatArg(rt, args[0], "seconds");
        if (seconds <= 0) throw jsi::JSError(rt, "captureClip: seconds must be > 0");
        auto weakOwner = weakOwner_;
        auto* orch = this;
        auto callInvoker = callInvoker_;
        auto constructor = rt.global().getPropertyAsFunction(rt, "Promise");
        return constructor.callAsConstructor(rt,
            jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, "ex"), 2,
                [weakOwner, orch, seconds, callInvoker](jsi::Runtime& rt, const jsi::Value&,
                                                         const jsi::Value* pArgs, size_t) -> jsi::Value {
                    auto resolve = std::make_shared<jsi::Function>(pArgs[0].getObject(rt).getFunction(rt));
                    auto reject = std::make_shared<jsi::Function>(pArgs[1].getObject(rt).getFunction(rt));
                    auto owner = weakOwner.lock();
                    if (!owner) {
                        reject->call(rt, jsi::String::createFromAscii(rt, "module destroyed"));
                        return jsi::Value::undefined();
                    }
                    auto clipResult = orch->clipIndex_.extractClip(seconds);
                    if (!clipResult.error.empty()) {
                        // ClipIndex error strings are ASCII literals; skip UTF-8 validation.
                        reject->call(rt, jsi::String::createFromAscii(rt,
                            clipResult.error.data(), clipResult.error.size()));
                        return jsi::Value::undefined();
                    }
                    auto clipData = std::make_shared<std::vector<uint8_t>>(std::move(clipResult.data));
                    auto threadOwner = weakOwner;
                    if (orch->clipThread_.joinable()) {
                        if (orch->clipThreadDone_.load(std::memory_order_acquire)) {
                            orch->clipThread_.join();
                        } else {
                            reject->call(rt, jsi::String::createFromAscii(rt, "clip capture already in progress"));
                            return jsi::Value::undefined();
                        }
                    }
                    orch->clipThreadDone_.store(false, std::memory_order_relaxed);
                    orch->clipThread_ = std::thread([clipData, resolve, reject, callInvoker, threadOwner, orch]() {
                        std::string filePath = media::ClipIndex::generateFilePath();
                        std::string error;
                        FILE* f = fopen(filePath.c_str(), "wb");
                        if (!f) { error = "failed to create clip file"; }
                        else {
                            size_t written = fwrite(clipData->data(), 1, clipData->size(), f);
                            fclose(f);
                            if (written != clipData->size()) error = "incomplete write";
                        }
                        // Set done BEFORE queuing the async callback — the worker must not touch
                        // `orch` after invokeAsync so teardown's joinClipThread() has a bounded wait.
                        // threadOwner keeps `orch` alive up to this point (weak_ptr to the owning
                        // module); the async lambda re-locks threadOwner before its own orch access.
                        orch->clipThreadDone_.store(true, std::memory_order_release);
                        callInvoker->invokeAsync([resolve, reject, filePath, error, threadOwner, orch]() {
                            // Bind the shared_ptr for the full lambda scope; the temporary
                            // from `!weak.lock()` is destroyed at the `;`, dropping the lifetime
                            // guard before we access `orch`.
                            auto owner = threadOwner.lock();
                            if (!owner) return;
                            auto* rtPtr = orch->runtime_.load(std::memory_order_acquire);
                            if (!rtPtr) return;
                            auto& rt = *rtPtr;
                            // JSIExceptions can occur if the runtime was invalidated between the
                            // load above and the call below — treat as best-effort silence rather
                            // than letting the exception unwind the JS callInvoker.
                            try {
                                if (error.empty()) resolve->call(rt, jsi::String::createFromUtf8(rt, filePath));
                                else reject->call(rt, jsi::String::createFromUtf8(rt, error));
                            } catch (const jsi::JSIException& e) {
                                MEDIA_LOG_W("captureClip: Promise settle after runtime teardown: %s", e.what());
                            }
                        });
                    });
                    return jsi::Value::undefined();
                }));
    });
    t.method("seekTo", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "seekTo requires (offsetSeconds: number)");
        double offset = args[0].asNumber();
        if (!std::isfinite(offset)) throw jsi::JSError(rt, "seekTo: offset must be finite");
        // Clamp to ±kMaxDurationSeconds before the int64 cast in seekTo() —
        // unbounded finite doubles produce UB at static_cast<int64_t>(offset * 1e6).
        constexpr double kMaxAbs = static_cast<double>(media::config::clip::kMaxDurationSeconds);
        offset = std::clamp(offset, -kMaxAbs, kMaxAbs);
        return seekTo(offset);
    });
    t.method("getBufferRangeSeconds", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        return jsi::Value(static_cast<double>(clipIndex_.availableRangeSeconds()));
    }, DeadReturn::Zero);
}

}  // namespace mediamodule
