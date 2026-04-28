#include <jni.h>
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <algorithm>
#include <string>
#include "JniGlobals.h"
#include "playback/AudioSessionManager.h"
#include "common/MediaLog.h"

namespace {
    // JNI references, written once in nativeInit() and read from any thread.
    // std::atomic ensures visibility across threads without explicit barriers.
    std::atomic<jclass> g_bridgeClass{nullptr};
    std::atomic<jmethodID> g_setAudioRouteMethod{nullptr};
    std::atomic<jmethodID> g_getAvailableAudioRoutesMethod{nullptr};
    std::atomic<jmethodID> g_getAvailableAudioDevicesMethod{nullptr};
    std::atomic<jmethodID> g_getCurrentRouteMethod{nullptr};

    // Guards JNI global ref lifetime: call sites hold this while using
    // g_bridgeClass, nativeDestroy holds it during DeleteGlobalRef.
    // Prevents use-after-free when teardown races with in-flight JNI calls.
    std::mutex g_jniMutex;

    // RAII guard for JNI thread attach/detach. If the current thread was not
    // already attached to the JVM, AttachCurrentThread is called on construction
    // and DetachCurrentThread on destruction.
    struct JniEnvGuard {
        JNIEnv* env = nullptr;
        bool attached = false;

        bool acquire() {
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
                return true;
            }
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
                return true;
            }
            return false;
        }

        ~JniEnvGuard() {
            if (attached) g_jvm->DetachCurrentThread();
        }

        JniEnvGuard() = default;
        JniEnvGuard(const JniEnvGuard&) = delete;
        JniEnvGuard& operator=(const JniEnvGuard&) = delete;
    };

    // Invoke a callback with a valid JNIEnv and the bridge class/method,
    // handling mutex, thread attach/detach, and null checks.
    // Returns the callback's return value, or `fallback` if JNI is unavailable.
    template<typename R, typename Fn>
    R withJni(const std::atomic<jmethodID>& methodAtom, R fallback,
              const char* tag, Fn&& fn) {
        std::lock_guard<std::mutex> jniLock(g_jniMutex);

        jclass cls = g_bridgeClass.load(std::memory_order_acquire);
        jmethodID method = methodAtom.load(std::memory_order_acquire);
        if (!g_jvm || !cls || !method) {
            if (tag) MEDIA_LOG_E("%s: JNI not initialized", tag);
            return fallback;
        }

        JniEnvGuard guard;
        if (!guard.acquire()) {
            MEDIA_LOG_E("%s: failed to attach thread", tag);
            return fallback;
        }

        R result = fn(guard.env, cls, method);

        if (guard.env->ExceptionCheck()) {
            guard.env->ExceptionClear();
            MEDIA_LOG_E("%s: JNI exception", tag);
            return fallback;
        }

        return result;
    }

    // Void overload — no return value, no fallback.
    template<typename Fn>
    void withJniVoid(const std::atomic<jmethodID>& methodAtom,
                     const char* tag, Fn&& fn) {
        std::lock_guard<std::mutex> jniLock(g_jniMutex);

        jclass cls = g_bridgeClass.load(std::memory_order_acquire);
        jmethodID method = methodAtom.load(std::memory_order_acquire);
        if (!g_jvm || !cls || !method) return;

        JniEnvGuard guard;
        if (!guard.acquire()) {
            MEDIA_LOG_E("%s: failed to attach thread", tag);
            return;
        }

        fn(guard.env, cls, method);

        if (guard.env->ExceptionCheck()) {
            guard.env->ExceptionClear();
            MEDIA_LOG_E("%s: JNI exception", tag);
        }
    }
}

// Called from JSI to set audio route, optionally targeting a specific device
bool jniSetAudioRoute(int route, const std::string& deviceId) {
    return withJni(g_setAudioRouteMethod, false, "jniSetAudioRoute",
        [&](JNIEnv* env, jclass cls, jmethodID method) -> bool {
            jstring jDeviceId = env->NewStringUTF(deviceId.c_str());
            if (!jDeviceId) {
                MEDIA_LOG_E("jniSetAudioRoute: NewStringUTF failed");
                return false;
            }
            jboolean result = env->CallStaticBooleanMethod(
                cls, method, static_cast<jint>(route), jDeviceId);
            env->DeleteLocalRef(jDeviceId);
            // Clear exception inside callback so withJni's check sees clean state
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                MEDIA_LOG_E("jniSetAudioRoute: JNI exception");
                return false;
            }
            return result == JNI_TRUE;
        });
}

// Called from JSI to get available audio routes
std::vector<media::AudioRoute> jniGetAvailableAudioRoutes() {
    return withJni(g_getAvailableAudioRoutesMethod,
        std::vector<media::AudioRoute>{}, "jniGetAvailableAudioRoutes",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            std::vector<media::AudioRoute> routes;
            auto result = static_cast<jintArray>(
                env->CallStaticObjectMethod(cls, method));

            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                MEDIA_LOG_E("jniGetAvailableAudioRoutes: JNI exception");
                return routes;
            }
            if (result) {
                jsize length = env->GetArrayLength(result);
                jint* elements = env->GetIntArrayElements(result, nullptr);
                if (elements) {
                    for (jsize i = 0; i < length; i++) {
                        routes.push_back(static_cast<media::AudioRoute>(elements[i]));
                    }
                    env->ReleaseIntArrayElements(result, elements, JNI_ABORT);
                }
                env->DeleteLocalRef(result);
            }
            return routes;
        });
}

// Called from JSI to get available audio devices with names and IDs
std::vector<media::AudioDeviceInfo> jniGetAvailableAudioDevices() {
    return withJni(g_getAvailableAudioDevicesMethod,
        std::vector<media::AudioDeviceInfo>{}, "jniGetAvailableAudioDevices",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            std::vector<media::AudioDeviceInfo> devices;
            auto result = static_cast<jobjectArray>(
                env->CallStaticObjectMethod(cls, method));

            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                if (result) env->DeleteLocalRef(result);
                MEDIA_LOG_E("jniGetAvailableAudioDevices: JNI exception");
                return devices;
            }
            if (result) {
                jsize length = env->GetArrayLength(result);
                // Array is packed as triplets: [route, deviceName, deviceId, route, ...]
                for (jsize i = 0; i + 2 < length; i += 3) {
                    auto jRouteStr = static_cast<jstring>(env->GetObjectArrayElement(result, i));
                    auto jName = static_cast<jstring>(env->GetObjectArrayElement(result, i + 1));
                    auto jId = static_cast<jstring>(env->GetObjectArrayElement(result, i + 2));

                    const char* routeChars = jRouteStr ? env->GetStringUTFChars(jRouteStr, nullptr) : nullptr;
                    const char* nameChars = jName ? env->GetStringUTFChars(jName, nullptr) : nullptr;
                    const char* idChars = jId ? env->GetStringUTFChars(jId, nullptr) : nullptr;

                    media::AudioDeviceInfo info;
                    info.route = static_cast<media::AudioRoute>(routeChars ? std::atoi(routeChars) : 0);
                    info.deviceName = nameChars ? nameChars : "";
                    info.deviceId = idChars ? idChars : "";

                    if (routeChars) env->ReleaseStringUTFChars(jRouteStr, routeChars);
                    if (nameChars) env->ReleaseStringUTFChars(jName, nameChars);
                    if (idChars) env->ReleaseStringUTFChars(jId, idChars);
                    if (jRouteStr) env->DeleteLocalRef(jRouteStr);
                    if (jName) env->DeleteLocalRef(jName);
                    if (jId) env->DeleteLocalRef(jId);

                    devices.push_back(std::move(info));
                }
                env->DeleteLocalRef(result);
            }
            return devices;
        });
}

// Called from AudioSessionManager::ensureRouteDetected() to query current route before session start
int jniGetCurrentRoute() {
    return withJni(g_getCurrentRouteMethod, 0, "jniGetCurrentRoute",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            return static_cast<int>(env->CallStaticIntMethod(cls, method));
        });
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_audio_AudioSessionBridge_nativeInit(JNIEnv* env, jclass) {
    // Find the class explicitly by name - the jclass parameter from Kotlin object
    // may not be usable directly for GetStaticMethodID
    jclass localClass = env->FindClass("com/heartit/webmplayer/audio/AudioSessionBridge");
    if (!localClass) {
        MEDIA_LOG_E("nativeInit: failed to find AudioSessionBridge class");
        return;
    }

    jclass globalClass = static_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);

    // Store method IDs before the class ref so readers that see g_bridgeClass
    // (via acquire) are guaranteed to also see the method IDs.
    g_setAudioRouteMethod.store(
        env->GetStaticMethodID(globalClass, "setAudioRoute", "(ILjava/lang/String;)Z"), std::memory_order_relaxed);
    g_getAvailableAudioRoutesMethod.store(
        env->GetStaticMethodID(globalClass, "getAvailableAudioRoutes", "()[I"), std::memory_order_relaxed);
    g_getAvailableAudioDevicesMethod.store(
        env->GetStaticMethodID(globalClass, "getAvailableAudioDevices", "()[Ljava/lang/String;"), std::memory_order_relaxed);
    g_getCurrentRouteMethod.store(
        env->GetStaticMethodID(globalClass, "getCurrentRoute", "()I"), std::memory_order_relaxed);
    // Release fence: all method ID stores above are visible before class ref becomes non-null
    g_bridgeClass.store(globalClass, std::memory_order_release);

    media::AudioSessionManager::instance().initialize();
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_audio_AudioSessionBridge_nativeOnAudioRouteChanged(
        JNIEnv* env, jclass, jint route, jstring jDeviceId) {
    int routeVal = std::clamp(static_cast<int>(route), 0,
                              static_cast<int>(media::AudioRoute::UsbDevice));
    std::string deviceId;
    if (jDeviceId) {
        const char* chars = env->GetStringUTFChars(jDeviceId, nullptr);
        if (chars) {
            deviceId = chars;
            env->ReleaseStringUTFChars(jDeviceId, chars);
        }
    }
    media::AudioSessionManager::instance().onAudioRouteChanged(
        static_cast<media::AudioRoute>(routeVal),
        deviceId);
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_audio_AudioSessionBridge_nativeDestroy(JNIEnv* env, jclass) {
    media::AudioSessionManager::instance().shutdown(env);

    // Hold g_jniMutex while deleting the global ref to prevent use-after-free
    // in concurrent jniSetAudioRoute calls.
    std::lock_guard<std::mutex> jniLock(g_jniMutex);
    jclass cls = g_bridgeClass.exchange(nullptr, std::memory_order_acq_rel);
    if (cls) {
        env->DeleteGlobalRef(cls);
    }
    g_setAudioRouteMethod.store(nullptr, std::memory_order_relaxed);
    g_getAvailableAudioRoutesMethod.store(nullptr, std::memory_order_relaxed);
    g_getAvailableAudioDevicesMethod.store(nullptr, std::memory_order_relaxed);
    g_getCurrentRouteMethod.store(nullptr, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_audio_AudioSessionBridge_nativeOnAudioFocusChanged(
        JNIEnv*, jclass, jint focusChange) {
    media::AudioSessionManager::instance().onAudioFocusChanged(static_cast<int>(focusChange));
}

}  // extern "C"

// Implementation of AudioSessionManager methods
namespace media {

AudioSessionManager& AudioSessionManager::instance() noexcept {
    static AudioSessionManager mgr;
    return mgr;
}

AudioSessionManager::~AudioSessionManager() = default;

bool AudioSessionManager::initialize() noexcept {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++refCount_;
        if (initialized_) return true;

        // Wire RouteHandler callbacks to existing infrastructure
        routeHandler_.setCallbacks({
            .restartStreams = [this]() { this->requestStreamRestart(); },
            .fireJsCallback = [this](AudioRoute route) {
                AudioRouteCallback cb;
                {
                    std::lock_guard<std::mutex> lk2(this->mutex_);
                    cb = this->routeCallback_;
                }
                if (cb) cb(route);
            },
            .resetDrift = [this]() {
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lk(this->mutex_);
                    cb = this->driftResetCallback_;
                }
                if (cb) cb();
            },
        });

        initialized_ = true;
        MEDIA_LOG_I("AudioSessionManager initialized (refCount=%d)", refCount_);
    }

    // Initialize: detect current route, notify JS
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        RouteDetectionResult result{
            currentRoute_.load(std::memory_order_acquire),
            currentDeviceId_
        };
        routeHandler_.onInitialRoute(result);
    }

    return true;
}

void AudioSessionManager::onAudioRouteChanged(AudioRoute newRoute,
                                               const std::string& deviceId) noexcept {
    currentRoute_.exchange(newRoute, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        currentDeviceId_ = deviceId;
    }

    MEDIA_LOG_I("AudioRoute: -> %s (deviceId=%s)",
                audioRouteToString(newRoute), deviceId.c_str());

    // Let RouteHandler handle dedup, restart decisions, and JS callback dispatch.
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        routeHandler_.onRouteDetected({newRoute, deviceId});
    }
}

void AudioSessionManager::shutdown([[maybe_unused]] JNIEnv* env) noexcept {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!initialized_ || refCount_ <= 0) return;
        --refCount_;
        if (refCount_ > 0) {
            MEDIA_LOG_I("AudioSessionManager: shutdown deferred (refCount=%d)", refCount_);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        routeHandler_.reset();
    }

    std::lock_guard<std::mutex> lk(mutex_);
    routeCallback_ = nullptr;
    restartCallback_ = nullptr;
    driftResetCallback_ = nullptr;
    audioFocusCallback_ = nullptr;
    initialized_ = false;

    // Reset all routing state to defaults for clean re-initialization (parity with iOS)
    currentRoute_.store(AudioRoute::Unknown, std::memory_order_relaxed);
    currentDeviceId_.clear();

    MEDIA_LOG_I("AudioSessionManager shutdown (refCount=0)");
}

bool AudioSessionManager::setAudioRoute(AudioRoute route, const std::string& deviceId) noexcept {
    return executeRouteChangePlatform(route, deviceId);
}

bool AudioSessionManager::executeRouteChangePlatform(AudioRoute route, const std::string& deviceId) noexcept {
    return jniSetAudioRoute(static_cast<int>(route), deviceId);
}

std::vector<AudioRoute> AudioSessionManager::getAvailableAudioRoutes() noexcept {
    return jniGetAvailableAudioRoutes();
}

std::vector<AudioDeviceInfo> AudioSessionManager::getAvailableAudioDevices() noexcept {
    return jniGetAvailableAudioDevices();
}

void AudioSessionManager::ensureRouteDetected() noexcept {
    if (currentRoute_.load(std::memory_order_acquire) != AudioRoute::Unknown) return;

    int route = jniGetCurrentRoute();
    int routeVal = std::clamp(route, 0, static_cast<int>(AudioRoute::UsbDevice));
    auto detected = static_cast<AudioRoute>(routeVal);
    if (detected != AudioRoute::Unknown) {
        AudioRoute expected = AudioRoute::Unknown;
        currentRoute_.compare_exchange_strong(expected, detected, std::memory_order_acq_rel);
    }
}

void AudioSessionManager::onAudioFocusChanged(int focusChange) noexcept {
    AudioFocusState state;
    switch (focusChange) {
        case 1:  // AUDIOFOCUS_GAIN
            state = AudioFocusState::Gained;
            break;
        case -1: // AUDIOFOCUS_LOSS
            state = AudioFocusState::Lost;
            break;
        case -2: // AUDIOFOCUS_LOSS_TRANSIENT
            state = AudioFocusState::LostTransient;
            break;
        case -3: // AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK
            state = AudioFocusState::LostTransientCanDuck;
            break;
        default:
            return;
    }

    // Update interrupted flag for RT-safe muting in AAudio data callback.
    // Lost and LostTransient silence immediately; LostTransientCanDuck does not
    // (ducking is handled by gain reduction in JS). Mirrors iOS interruptedFlag_.
    bool wasInterrupted = interrupted_.load(std::memory_order_acquire);
    bool nowInterrupted = (state == AudioFocusState::Lost ||
                           state == AudioFocusState::LostTransient);
    interrupted_.store(nowInterrupted, std::memory_order_release);

    MEDIA_LOG_I("AudioFocus: %d → state=%d interrupted=%d", focusChange,
                static_cast<int>(state), nowInterrupted);

    // On focus regain after interruption, restart the audio stream.
    // Mirrors iOS behaviour in AudioSessionManager.mm which calls restartCallback_
    // on interruption end with shouldResume.
    if (wasInterrupted && !nowInterrupted) {
        requestStreamRestart();
    }

    AudioFocusCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = audioFocusCallback_;
    }
    if (cb) cb(state);
}

}  // namespace media
