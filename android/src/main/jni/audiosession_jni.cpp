#include <jni.h>
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <algorithm>
#include <string>
#include "JniGlobals.h"
#include "playback/AudioSessionManager.h"
#include "AudioCoordinator.h"
#include "common/MediaLog.h"

namespace {
    // JNI references, written once in nativeInit() and read from any thread.
    // std::atomic ensures visibility across threads without explicit barriers.
    std::atomic<jclass> g_bridgeClass{nullptr};
    std::atomic<jmethodID> g_startSessionMethod{nullptr};
    std::atomic<jmethodID> g_stopSessionMethod{nullptr};
    std::atomic<jmethodID> g_setAudioRouteMethod{nullptr};
    std::atomic<jmethodID> g_getAvailableAudioRoutesMethod{nullptr};
    std::atomic<jmethodID> g_getAvailableAudioDevicesMethod{nullptr};
    std::atomic<jmethodID> g_getCurrentRouteMethod{nullptr};
    std::atomic<jmethodID> g_routeToBestDeviceNowMethod{nullptr};
    std::atomic<jmethodID> g_validateDeviceStateMethod{nullptr};
    std::atomic<jmethodID> g_reassertVoiceModeMethod{nullptr};

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

// Called by AudioCoordinator when first stream registers
void jniStartSession() {
    withJniVoid(g_startSessionMethod, "jniStartSession",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            env->CallStaticVoidMethod(cls, method);
        });
}

// Called by AudioCoordinator when last stream unregisters
void jniStopSession() {
    withJniVoid(g_stopSessionMethod, "jniStopSession",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            env->CallStaticVoidMethod(cls, method);
        });
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
    return withJni(g_getCurrentRouteMethod, 0, nullptr,
        [](JNIEnv* env, jclass cls, jmethodID method) {
            return static_cast<int>(env->CallStaticIntMethod(cls, method));
        });
}

// Called on device connect/disconnect to trigger Kotlin device selection
void jniRouteToBestDevice() {
    withJniVoid(g_routeToBestDeviceNowMethod, "jniRouteToBestDevice",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            env->CallStaticVoidMethod(cls, method);
        });
}

// Called to verify actual communication device state (Samsung quirk guard)
bool jniValidateDeviceState() {
    return withJni(g_validateDeviceStateMethod, false, "jniValidateDeviceState",
        [](JNIEnv* env, jclass cls, jmethodID method) -> bool {
            return env->CallStaticBooleanMethod(cls, method) == JNI_TRUE;
        });
}

// Called on foreground resume to re-assert MODE_IN_COMMUNICATION (Xiaomi quirk)
void jniReassertVoiceMode() {
    withJniVoid(g_reassertVoiceModeMethod, "jniReassertVoiceMode",
        [](JNIEnv* env, jclass cls, jmethodID method) {
            env->CallStaticVoidMethod(cls, method);
        });
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeInit(JNIEnv* env, jclass) {
    // Find the class explicitly by name - the jclass parameter from Kotlin object
    // may not be usable directly for GetStaticMethodID
    jclass localClass = env->FindClass("com/holepunchto/webmplayer/audio/AudioSessionBridge");
    if (!localClass) {
        MEDIA_LOG_E("nativeInit: failed to find AudioSessionBridge class");
        return;
    }

    jclass globalClass = static_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);

    // Store method IDs before the class ref so readers that see g_bridgeClass
    // (via acquire) are guaranteed to also see the method IDs.
    g_startSessionMethod.store(
        env->GetStaticMethodID(globalClass, "startSession", "()V"), std::memory_order_relaxed);
    g_stopSessionMethod.store(
        env->GetStaticMethodID(globalClass, "stopSession", "()V"), std::memory_order_relaxed);
    g_setAudioRouteMethod.store(
        env->GetStaticMethodID(globalClass, "setAudioRoute", "(ILjava/lang/String;)Z"), std::memory_order_relaxed);
    g_getAvailableAudioRoutesMethod.store(
        env->GetStaticMethodID(globalClass, "getAvailableAudioRoutes", "()[I"), std::memory_order_relaxed);
    g_getAvailableAudioDevicesMethod.store(
        env->GetStaticMethodID(globalClass, "getAvailableAudioDevices", "()[Ljava/lang/String;"), std::memory_order_relaxed);
    g_getCurrentRouteMethod.store(
        env->GetStaticMethodID(globalClass, "getCurrentRoute", "()I"), std::memory_order_relaxed);
    g_routeToBestDeviceNowMethod.store(
        env->GetStaticMethodID(globalClass, "routeToBestDeviceNow", "()V"), std::memory_order_relaxed);
    g_validateDeviceStateMethod.store(
        env->GetStaticMethodID(globalClass, "validateDeviceState", "()Z"), std::memory_order_relaxed);
    g_reassertVoiceModeMethod.store(
        env->GetStaticMethodID(globalClass, "reassertVoiceMode", "()V"), std::memory_order_relaxed);
    // Release fence: all method ID stores above are visible before class ref becomes non-null
    g_bridgeClass.store(globalClass, std::memory_order_release);

    media::AudioSessionManager::instance().initialize();
    media::AudioCoordinator::instance().initialize();
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnSessionIdGenerated(
        JNIEnv*, jclass, jint sessionId) {
    media::AudioSessionManager::instance().onSessionIdGenerated(sessionId);
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeSetVoiceMode(
        JNIEnv*, jclass, jboolean enable) {
    media::AudioSessionManager::instance().setVoiceModeEnabled(enable);
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnAudioRouteChanged(
        JNIEnv* env, jclass, jint route, jint echoMode, jstring jDeviceId) {
    int routeVal = std::clamp(static_cast<int>(route), 0,
                              static_cast<int>(media::AudioRoute::UsbDevice));
    int echoVal = std::clamp(static_cast<int>(echoMode), 0,
                             static_cast<int>(media::EchoMode::None));
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
        static_cast<media::EchoMode>(echoVal),
        deviceId);
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnAudioFocusChanged(
        JNIEnv*, jclass, jint focusState) {
    int focusVal = std::clamp(static_cast<int>(focusState), 0,
                              static_cast<int>(media::AudioFocusState::LossTransientCanDuck));
    media::AudioSessionManager::instance().onAudioFocusChanged(
        static_cast<media::AudioFocusState>(focusVal));
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnLifecycleChanged(
        JNIEnv*, jclass, jboolean foreground) {
    media::AudioSessionManager::instance().onLifecycleChanged(foreground);
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnDeviceConnected(
        JNIEnv*, jclass) {
    media::AudioSessionManager::instance().onDeviceConnected();
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeOnDeviceDisconnected(
        JNIEnv*, jclass) {
    media::AudioSessionManager::instance().onDeviceDisconnected();
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_audio_AudioSessionBridge_nativeDestroy(JNIEnv* env, jclass) {
    media::AudioCoordinator::instance().shutdown();
    media::AudioSessionManager::instance().shutdown(env);

    // Hold g_jniMutex while deleting the global ref to prevent use-after-free
    // in concurrent jniStartSession/jniStopSession/jniSetAudioRoute calls.
    std::lock_guard<std::mutex> jniLock(g_jniMutex);
    jclass cls = g_bridgeClass.exchange(nullptr, std::memory_order_acq_rel);
    if (cls) {
        env->DeleteGlobalRef(cls);
    }
    g_startSessionMethod.store(nullptr, std::memory_order_relaxed);
    g_stopSessionMethod.store(nullptr, std::memory_order_relaxed);
    g_setAudioRouteMethod.store(nullptr, std::memory_order_relaxed);
    g_getAvailableAudioRoutesMethod.store(nullptr, std::memory_order_relaxed);
    g_getAvailableAudioDevicesMethod.store(nullptr, std::memory_order_relaxed);
    g_getCurrentRouteMethod.store(nullptr, std::memory_order_relaxed);
    g_routeToBestDeviceNowMethod.store(nullptr, std::memory_order_relaxed);
    g_validateDeviceStateMethod.store(nullptr, std::memory_order_relaxed);
    g_reassertVoiceModeMethod.store(nullptr, std::memory_order_relaxed);
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
        if (initialized_) return true;

        // Wire RouteHandler callbacks to existing infrastructure
        routeHandler_.setCallbacks({
            .restartStreams = [this]() { this->requestStreamRestart(); },
            .fireJsCallback = [this](AudioRoute route, EchoMode echoMode) {
                AudioRouteCallback cb;
                {
                    std::lock_guard<std::mutex> lk2(this->mutex_);
                    cb = this->routeCallback_;
                }
                if (cb) cb(route, echoMode);
            },
            .resetDrift = [this]() {
                if (this->driftResetCallback_) this->driftResetCallback_();
            },
        });

        initialized_ = true;
        MEDIA_LOG_I("AudioSessionManager initialized");
    }

    // Initialize: detect current route, notify JS
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        RouteDetectionResult result{
            currentRoute_.load(std::memory_order_acquire),
            currentEchoMode_.load(std::memory_order_acquire),
            currentDeviceId_
        };
        routeHandler_.onInitialRoute(result);
    }

    return true;
}

void AudioSessionManager::onAudioFocusChanged(AudioFocusState newState) noexcept {
    AudioFocusState oldState = focusState_.exchange(newState, std::memory_order_acq_rel);

    MEDIA_LOG_I("AudioFocus: %d -> %d", static_cast<int>(oldState), static_cast<int>(newState));

    AudioFocusCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = focusCallback_;
    }
    if (cb) cb(newState);
}

void AudioSessionManager::onLifecycleChanged(bool foreground) noexcept {
    bool wasForeground = isForeground_.exchange(foreground, std::memory_order_acq_rel);

    if (wasForeground == foreground) return;

    MEDIA_LOG_I("Lifecycle: %s -> %s",
                wasForeground ? "foreground" : "background",
                foreground ? "foreground" : "background");

    LifecycleCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = lifecycleCallback_;
    }
    if (cb) cb(foreground);

    // Xiaomi quirk: re-assert MODE_IN_COMMUNICATION on foreground resume.
    // Some Xiaomi devices silently reset the audio mode when backgrounded.
    if (foreground) {
        jniReassertVoiceMode();
    }
}

void AudioSessionManager::onAudioRouteChanged(AudioRoute newRoute, EchoMode newEchoMode,
                                               const std::string& deviceId) noexcept {
    // Samsung quirk guard: some Samsung devices fire spurious route changes with
    // Unknown route when the communication device is actually valid. Verify device
    // state and drop the event if it's a false alarm.
    if (newRoute == AudioRoute::Unknown && jniValidateDeviceState()) {
        MEDIA_LOG_D("onAudioRouteChanged: Samsung quirk — dropping spurious Unknown route");
        return;
    }

    AudioRoute oldRoute = currentRoute_.exchange(newRoute, std::memory_order_acq_rel);
    currentEchoMode_.store(newEchoMode, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        currentDeviceId_ = deviceId;
    }

    // Reset A2DP latency logging flag on route change to log again for new connection
    if (oldRoute != newRoute) {
        a2dpLatencyLogged_.store(false, std::memory_order_relaxed);
    }

    MEDIA_LOG_I("AudioRoute: %s -> %s (echo=%s, deviceId=%s)",
                audioRouteToString(oldRoute), audioRouteToString(newRoute),
                echoModeToString(newEchoMode), deviceId.c_str());

    // Let RouteHandler handle dedup, restart decisions, and JS callback dispatch.
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        routeHandler_.onRouteDetected({newRoute, newEchoMode, deviceId});
    }
}

void AudioSessionManager::onDeviceConnected() noexcept {
    jniRouteToBestDevice();
    // Fire JS callback so device list updates in the UI
    AudioRouteCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = routeCallback_;
    }
    if (cb) cb(currentRoute_.load(std::memory_order_acquire),
               currentEchoMode_.load(std::memory_order_acquire));
}

void AudioSessionManager::onDeviceDisconnected() noexcept {
    jniRouteToBestDevice();
    // Fire JS callback so device list updates in the UI
    AudioRouteCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = routeCallback_;
    }
    if (cb) cb(currentRoute_.load(std::memory_order_acquire),
               currentEchoMode_.load(std::memory_order_acquire));
}

void AudioSessionManager::shutdown([[maybe_unused]] JNIEnv* env) noexcept {
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        routeHandler_.reset();
    }

    std::lock_guard<std::mutex> lk(mutex_);
    routeCallback_ = nullptr;
    restartCallback_ = nullptr;
    focusCallback_ = nullptr;
    lifecycleCallback_ = nullptr;
    driftResetCallback_ = nullptr;
    initialized_ = false;

    // Reset all routing state to defaults for clean re-initialization (parity with iOS)
    currentRoute_.store(AudioRoute::Unknown, std::memory_order_relaxed);
    currentEchoMode_.store(EchoMode::HardwareAEC, std::memory_order_relaxed);
    focusState_.store(AudioFocusState::None, std::memory_order_relaxed);
    voiceModeEnabled_.store(false, std::memory_order_relaxed);
    isForeground_.store(true, std::memory_order_relaxed);
    currentSessionId_.store(AAUDIO_SESSION_ID_ALLOCATE, std::memory_order_relaxed);
    measuredOutputLatencyUs_.store(0, std::memory_order_relaxed);
    a2dpLatencyLogged_.store(false, std::memory_order_relaxed);
    currentDeviceId_.clear();
}

bool AudioSessionManager::setAudioRoute(AudioRoute route, const std::string& deviceId) noexcept {
    {
        std::lock_guard<std::mutex> rlk(routingMutex_);
        routeHandler_.onExplicitRouteRequested(route);
    }
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

}  // namespace media
