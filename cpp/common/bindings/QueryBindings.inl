// JSI bindings for read-only queries (metrics, track info, time, state, keyframe).
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerQueryMethods() {
    auto& t = dispatch_;

    t.method("getMetrics", 0, [this](jsi::Runtime& rt, const jsi::Value*, size_t) {
        return collectMetrics(rt, metricsContext());
    }, DeadReturn::Undefined);
    t.method("getTrackInfo", 0, [this](jsi::Runtime& rt, const jsi::Value*, size_t) {
        if (demuxer_.parseState() != media::demux::ParseState::Streaming) return jsi::Value::null();
        auto info = demuxer_.trackInfoSnapshot();
        jsi::Object obj(rt);
        // Codec IDs are ASCII registered identifiers (e.g. "A_OPUS", "V_VP9"); skip
        // the UTF-8 validation pass.
        obj.setProperty(rt, "audioCodecId",
            jsi::String::createFromAscii(rt, info.audioCodecId.data(), info.audioCodecId.size()));
        obj.setProperty(rt, "videoCodecId",
            jsi::String::createFromAscii(rt, info.videoCodecId.data(), info.videoCodecId.size()));
        obj.setProperty(rt, "videoWidth", info.videoWidth);
        obj.setProperty(rt, "videoHeight", info.videoHeight);
        return jsi::Value(std::move(obj));
    }, DeadReturn::Null);
    t.method("getCurrentTimeUs", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        return jsi::Value(static_cast<double>(session_.currentTimeUs()));
    }, DeadReturn::Zero);
    t.method("getPlaybackState", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        using PB = media::PlaybackState;
        auto pb = [&] {
            if (!session_.isRunning()) return PB::Idle;
            if (session_.isPaused()) return PB::Paused;
            auto stallState = stallRecovery_.state();
            if (stallState == media::StallState::Failed) return PB::Failed;
            if (stallState == media::StallState::Stalled || stallState == media::StallState::Recovering) return PB::Stalled;
            auto s = session_.audioState();
            if (s == media::StreamState::Buffering || s == media::StreamState::Underrun) return PB::Buffering;
            return PB::Playing;
        }();
        return jsi::Value(static_cast<int>(pb));
    }, DeadReturn::Zero);
    t.method("requestKeyFrame", 0, [this](jsi::Runtime&, const jsi::Value*, size_t) {
        if (videoQueue_) { videoQueue_->requestKeyFrame(); return jsi::Value(true); }
        return jsi::Value(false);
    });
}

}  // namespace mediamodule
