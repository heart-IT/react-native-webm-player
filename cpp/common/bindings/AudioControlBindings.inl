// JSI bindings for audio playback controls (mute, gain, rate, buffer, policy).
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerAudioControlMethods() {
    auto& t = dispatch_;

    t.method("setMuted", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isBool()) throw jsi::JSError(rt, "setMuted requires (muted: boolean)");
        session_.setMuted(args[0].getBool());
        return jsi::Value(true);
    });
    t.method("setGain", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setGain requires (gain: number)");
        session_.setGain(jsiutil::safeFloatArg(rt, args[0], "gain"));
        return jsi::Value(true);
    });
    t.method("setPlaybackRate", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setPlaybackRate requires (rate: number)");
        float rate = jsiutil::safeFloatArg(rt, args[0], "rate");
        rate = std::clamp(rate, media::config::playbackrate::kMinRate, media::config::playbackrate::kMaxRate);
        session_.setPlaybackRate(rate);
        if (videoQueue_) videoQueue_->setPlaybackRate(rate);
        return jsi::Value(true);
    });
    t.method("setBufferTarget", 2, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 2 || !args[0].isNumber() || !args[1].isNumber())
            throw jsi::JSError(rt, "setBufferTarget requires (audioMs: number, videoMs: number)");
        double audioMs = args[0].asNumber(), videoMs = args[1].asNumber();
        // Reject non-finite (NaN/Inf) — `static_cast<int64_t>(Inf)` is UB.
        if (!std::isfinite(audioMs) || !std::isfinite(videoMs))
            throw jsi::JSError(rt, "setBufferTarget: audioMs and videoMs must be finite");
        // Clamp to a sane upper bound (60s) before the int64 cast — finite
        // doubles > ~9.22e15 ms would produce UB at static_cast<int64_t>(x*1000).
        constexpr double kMaxBufferMs = 60'000.0;  // 60 seconds in ms
        int64_t audioUs = static_cast<int64_t>(std::clamp(audioMs, 0.0, kMaxBufferMs) * 1000.0);
        int64_t videoUs = static_cast<int64_t>(std::clamp(videoMs, 0.0, kMaxBufferMs) * 1000.0);
        session_.setBufferTargetOverride(audioUs, videoUs);
        if (videoQueue_) videoQueue_->setBufferTargetOverride(videoUs);
        return jsi::Value(true);
    });
    t.method("setCatchupPolicy", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setCatchupPolicy requires (policy: number)");
        int val = jsiutil::safeIntArg(rt, args[0], "policy");
        if (val < 0 || val > 2) throw jsi::JSError(rt, "setCatchupPolicy: policy out of range (0-2)");
        auto policy = static_cast<media::CatchupPolicy>(val);
        session_.setCatchupPolicy(policy);
        if (videoQueue_) videoQueue_->setCatchupPolicy(policy);
        return jsi::Value(true);
    });
    t.method("setStreamStatus", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setStreamStatus requires (status: number)");
        int val = jsiutil::safeIntArg(rt, args[0], "status");
        if (val < 0 || val > 3) throw jsi::JSError(rt, "setStreamStatus: status out of range (0-3)");
        session_.setStreamStatus(static_cast<media::StreamStatus>(val));
        return jsi::Value(true);
    });
}

}  // namespace mediamodule
