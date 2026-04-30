#import "MediaPipelineModuleV2.h"
#import "PlaybackEngineV2.h"

#import <Foundation/Foundation.h>

namespace jsi = facebook::jsi;

namespace mediamodule_v2 {

// One engine per runtime. Most apps have a single RN runtime; map keeps it tidy.
static std::unordered_map<uintptr_t, PlaybackEngineV2*> g_engines;
static std::mutex g_enginesMutex;
static PlaybackEngineV2* g_currentEngine = nil;  // Most recently installed; used by VideoViewV2.

static PlaybackEngineV2* engineFor(jsi::Runtime& rt) {
    std::lock_guard<std::mutex> lock(g_enginesMutex);
    auto it = g_engines.find(reinterpret_cast<uintptr_t>(&rt));
    return (it != g_engines.end()) ? it->second : nil;
}

#pragma mark - Property setters

static jsi::Value boolReturn(jsi::Runtime& rt, BOOL ok) {
    return jsi::Value(ok ? true : false);
}

#pragma mark - Method bindings

static jsi::Function makeFn(jsi::Runtime& rt, const char* name, unsigned argc,
                             jsi::HostFunctionType fn) {
    return jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forUtf8(rt, name), argc, std::move(fn));
}

static jsi::Object createApiObject(jsi::Runtime& rt) {
    jsi::Object api(rt);

    api.setProperty(rt, "start", makeFn(rt, "start", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e start] : NO);
        }));
    api.setProperty(rt, "stop", makeFn(rt, "stop", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e stop] : NO);
        }));
    api.setProperty(rt, "pause", makeFn(rt, "pause", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e pause] : NO);
        }));
    api.setProperty(rt, "resume", makeFn(rt, "resume", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e resume] : NO);
        }));
    api.setProperty(rt, "isRunning", makeFn(rt, "isRunning", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return jsi::Value(e ? [e isRunning] == YES : false);
        }));
    api.setProperty(rt, "isPaused", makeFn(rt, "isPaused", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return jsi::Value(e ? [e isPaused] == YES : false);
        }));

    api.setProperty(rt, "feedData", makeFn(rt, "feedData", 1,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
            if (count < 1 || !args[0].isObject()) return jsi::Value(false);
            jsi::Object obj = args[0].asObject(rt);
            if (!obj.isArrayBuffer(rt)) return jsi::Value(false);
            jsi::ArrayBuffer ab = obj.getArrayBuffer(rt);
            const uint8_t* data = ab.data(rt);
            size_t size = ab.size(rt);
            PlaybackEngineV2* e = engineFor(rt);
            if (!e || !data || size == 0) return jsi::Value(false);
            size_t wrote = [e feedData:data length:size];
            return jsi::Value(static_cast<int>(wrote == size));
        }));

    api.setProperty(rt, "setMuted", makeFn(rt, "setMuted", 1,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
            if (count < 1) return jsi::Value(false);
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e setMuted:args[0].getBool() ? YES : NO] : NO);
        }));
    api.setProperty(rt, "setGain", makeFn(rt, "setGain", 1,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
            if (count < 1) return jsi::Value(false);
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e setGain:static_cast<float>(args[0].getNumber())] : NO);
        }));
    api.setProperty(rt, "setPlaybackRate", makeFn(rt, "setPlaybackRate", 1,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
            if (count < 1) return jsi::Value(false);
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e setPlaybackRate:static_cast<float>(args[0].getNumber())] : NO);
        }));
    api.setProperty(rt, "setEndOfStream", makeFn(rt, "setEndOfStream", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e setEndOfStream] : NO);
        }));
    api.setProperty(rt, "resetStream", makeFn(rt, "resetStream", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return boolReturn(rt, e ? [e resetStream] : NO);
        }));

    api.setProperty(rt, "getPlaybackState", makeFn(rt, "getPlaybackState", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            return jsi::Value(e ? static_cast<int>([e playbackState]) : 0);
        }));

    api.setProperty(rt, "getMetrics", makeFn(rt, "getMetrics", 0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            PlaybackEngineV2* e = engineFor(rt);
            jsi::Object out(rt);
            if (!e) return jsi::Value(std::move(out));
            PlaybackEngineV2Metrics m = [e metrics];
            out.setProperty(rt, "bytesFedTotal", jsi::Value(static_cast<double>(m.bytesFedTotal)));
            out.setProperty(rt, "audioPacketsDecoded", jsi::Value(static_cast<double>(m.audioPacketsDecoded)));
            out.setProperty(rt, "videoPacketsDecoded", jsi::Value(static_cast<double>(m.videoPacketsDecoded)));
            out.setProperty(rt, "audioUnderruns", jsi::Value(static_cast<double>(m.audioUnderruns)));
            out.setProperty(rt, "videoFramesDropped", jsi::Value(static_cast<double>(m.videoFramesDropped)));
            out.setProperty(rt, "videoWidth", jsi::Value(m.videoWidth));
            out.setProperty(rt, "videoHeight", jsi::Value(m.videoHeight));
            out.setProperty(rt, "currentTimeSeconds", jsi::Value(m.currentTimeSeconds));
            out.setProperty(rt, "playbackRate", jsi::Value(m.playbackRate));
            out.setProperty(rt, "muted", jsi::Value(m.muted));
            out.setProperty(rt, "gain", jsi::Value(m.gain));
            return jsi::Value(std::move(out));
        }));

    return api;
}

#pragma mark - Install / uninstall

bool installV2JSI(jsi::Runtime& rt,
                   std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    (void)callInvoker;  // not used yet; reserved for async dispatch
    {
        std::lock_guard<std::mutex> lock(g_enginesMutex);
        uintptr_t rtId = reinterpret_cast<uintptr_t>(&rt);
        if (g_engines.count(rtId)) {
            g_currentEngine = g_engines[rtId];
            return true;
        }
        PlaybackEngineV2* engine = [[PlaybackEngineV2 alloc] init];
        g_engines[rtId] = engine;
        g_currentEngine = engine;
    }
    rt.global().setProperty(rt, "__MediaPipelineV2", createApiObject(rt));
    return true;
}

void uninstallV2JSI(jsi::Runtime& rt) {
    rt.global().setProperty(rt, "__MediaPipelineV2", jsi::Value::undefined());
    std::lock_guard<std::mutex> lock(g_enginesMutex);
    uintptr_t rtId = reinterpret_cast<uintptr_t>(&rt);
    auto it = g_engines.find(rtId);
    if (it != g_engines.end()) {
        if (g_currentEngine == it->second) g_currentEngine = nil;
        [it->second stop];
        g_engines.erase(it);
    }
    if (!g_currentEngine && !g_engines.empty()) {
        g_currentEngine = g_engines.begin()->second;
    }
}

PlaybackEngineV2* currentEngine() {
    std::lock_guard<std::mutex> lock(g_enginesMutex);
    return g_currentEngine;
}

}  // namespace mediamodule_v2
