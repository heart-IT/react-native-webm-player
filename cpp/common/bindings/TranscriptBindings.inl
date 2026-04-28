// JSI bindings for on-device transcription. Both HAVE_WHISPER and the no-op
// fallback live here so callers see identical method shapes either way.
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerTranscriptMethods() {
    auto& t = dispatch_;

#if HAVE_WHISPER
    t.method("setTranscriptionEnabled", 2, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isBool())
            throw jsi::JSError(rt, "setTranscriptionEnabled requires (enabled: boolean, modelPath?: string)");
        bool enabled = args[0].getBool();
        if (enabled) {
            if (transcriptThread_ && transcriptThread_->isRunning()) return jsi::Value(true);
            std::string modelPath;
            if (count >= 2 && args[1].isString()) modelPath = args[1].getString(rt).utf8(rt);
            if (modelPath.empty()) {
                MEDIA_LOG_E("setTranscriptionEnabled: model path required");
                return jsi::Value(false);
            }
            transcriptThread_ = std::make_unique<media::transcript::TranscriptThread>();
            if (!transcriptThread_->start(modelPath)) {
                transcriptThread_.reset();
                return jsi::Value(false);
            }
            session_.setTranscriptBuffer(transcriptThread_->ringBuffer());
        } else {
            session_.setTranscriptBuffer(nullptr);
            session_.waitForTranscriptDrain();
            transcriptThread_.reset();
            media::transcript::TranscriptRegistry::instance().clearHistory();
        }
        return jsi::Value(true);
    });
    t.method("setTranscriptCallback", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 1 || args[0].isNull() || args[0].isUndefined()) {
            transcriptJsCallback_.reset();
            media::transcript::TranscriptRegistry::instance().clearCallback(
                media::transcript::CallbackSlot::JsCallback);
            return jsi::Value(true);
        }
        if (!args[0].isObject() || !args[0].asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setTranscriptCallback requires a function or null");
        auto cb = std::make_shared<jsi::Function>(args[0].asObject(rt).asFunction(rt));
        auto weakCb = weakOwner_;
        transcriptJsCallback_ = cb;
        auto* orch = this;
        auto callInvoker = callInvoker_;
        media::transcript::TranscriptRegistry::instance().setCallback(
            media::transcript::CallbackSlot::JsCallback,
            [cb, callInvoker, weakCb, orch](const media::transcript::TranscriptSegment& seg) {
                auto owner = weakCb.lock();
                if (!owner || !orch->runtime_.load(std::memory_order_acquire)) return;
                auto segCopy = std::make_shared<media::transcript::TranscriptSegment>(seg);
                callInvoker->invokeAsync([cb, segCopy, weakCb, orch]() {
                    auto owner2 = weakCb.lock();
                    if (!owner2) return;
                    auto* rtPtr = orch->runtime_.load(std::memory_order_acquire);
                    if (!rtPtr) return;
                    auto& rt = *rtPtr;
                    jsi::Object obj(rt);
                    obj.setProperty(rt, "text", jsi::String::createFromUtf8(rt, segCopy->text));
                    obj.setProperty(rt, "startUs", static_cast<double>(segCopy->startUs));
                    obj.setProperty(rt, "endUs", static_cast<double>(segCopy->endUs));
                    obj.setProperty(rt, "isFinal", segCopy->isFinal);
                    double durSec = static_cast<double>(segCopy->endUs - segCopy->startUs) / 1'000'000.0;
                    double agoSec = static_cast<double>(orch->session_.currentTimeUs() - segCopy->startUs) / 1'000'000.0;
                    obj.setProperty(rt, "durationSeconds", durSec);
                    obj.setProperty(rt, "agoSeconds", agoSec);
                    cb->call(rt, std::move(obj));
                });
            });
        return jsi::Value(true);
    });
    t.method("getTranscriptHistory", 0, [this](jsi::Runtime& rt, const jsi::Value*, size_t) -> jsi::Value {
        auto history = media::transcript::TranscriptRegistry::instance().getHistory();
        int64_t currentTimeUs = session_.currentTimeUs();
        auto arr = jsi::Array(rt, history.size());
        for (size_t i = 0; i < history.size(); ++i) {
            const auto& h = history[i];
            double durationSec = static_cast<double>(h.endUs - h.startUs) / 1'000'000.0;
            double agoSec = static_cast<double>(currentTimeUs - h.startUs) / 1'000'000.0;
            jsi::Object seg(rt);
            seg.setProperty(rt, "text", jsi::String::createFromUtf8(rt, h.text));
            seg.setProperty(rt, "startUs", static_cast<double>(h.startUs));
            seg.setProperty(rt, "endUs", static_cast<double>(h.endUs));
            seg.setProperty(rt, "isFinal", h.isFinal);
            seg.setProperty(rt, "durationSeconds", durationSec);
            seg.setProperty(rt, "agoSeconds", agoSec);
            arr.setValueAtIndex(rt, i, std::move(seg));
        }
        return std::move(arr);
    }, DeadReturn::Undefined);
    t.method("setTranslationEnabled", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isBool())
            throw jsi::JSError(rt, "setTranslationEnabled requires (enabled: boolean)");
        bool enabled = args[0].getBool();
        if (transcriptThread_) transcriptThread_->setTranslateToEnglish(enabled);
        return jsi::Value(true);
    });
#else
    t.method("setTranscriptionEnabled", 2, [](jsi::Runtime&, const jsi::Value*, size_t) {
        MEDIA_LOG_W("Transcription unavailable: built without HAVE_WHISPER");
        return jsi::Value(false);
    });
    t.method("setTranscriptCallback", 1, [](jsi::Runtime&, const jsi::Value*, size_t) { return jsi::Value(false); });
    t.method("getTranscriptHistory", 0, [](jsi::Runtime& rt, const jsi::Value*, size_t) -> jsi::Value {
        return jsi::Array(rt, 0);
    }, DeadReturn::Undefined);
    t.method("setTranslationEnabled", 1, [](jsi::Runtime&, const jsi::Value*, size_t) { return jsi::Value(false); });
#endif
}

}  // namespace mediamodule
