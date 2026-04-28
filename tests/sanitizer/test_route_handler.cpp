// RouteHandler unit tests.
// Pure C++ — no platform dependencies, no Opus, no JNI.
// Build: cmake -DSANITIZER=address .. && make && ./test_route_handler

#include "test_common.h"
#include <string>
#include <vector>

#include "common/AudioRouteTypes.h"
#include "common/RouteHandler.h"

using namespace media;

// ============================================================
// Test harness: tracks callback invocations
// ============================================================

struct CallbackLog {
    int restartCount{0};
    int driftResetCount{0};
    std::vector<AudioRoute> jsCallbacks;

    void reset() {
        restartCount = 0;
        driftResetCount = 0;
        jsCallbacks.clear();
    }
};

static RouteHandler makeHandler(CallbackLog& log) {
    RouteHandler h;
    h.setCallbacks({
        .restartStreams = [&log]() { log.restartCount++; },
        .fireJsCallback = [&log](AudioRoute r) {
            log.jsCallbacks.push_back(r);
        },
        .resetDrift = [&log]() { log.driftResetCount++; },
    });
    return h;
}

// ============================================================
// Tests
// ============================================================

TEST(initial_route_sets_confirmed) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(h.currentRoute(), AudioRoute::Speaker);
    ASSERT_TRUE(h.initialized());
    ASSERT_EQ(log.jsCallbacks.size(), 1u);
    ASSERT_EQ(log.jsCallbacks[0], AudioRoute::Speaker);
    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 0);
}

TEST(dedup_same_route_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.jsCallbacks.size(), 1u);
    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 0);
}

TEST(route_change_fires_callback_and_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::BluetoothSco, "bt-1"});

    ASSERT_EQ(h.currentRoute(), AudioRoute::BluetoothSco);
    ASSERT_EQ(log.jsCallbacks.size(), 1u);
    ASSERT_EQ(log.jsCallbacks[0], AudioRoute::BluetoothSco);
    ASSERT_EQ(log.restartCount, 1);
    ASSERT_EQ(log.driftResetCount, 1);
}

TEST(speaker_to_earpiece_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Earpiece, "earpiece-1"});

    ASSERT_EQ(h.currentRoute(), AudioRoute::Earpiece);
    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 1);  // route changed, drift resets
    ASSERT_EQ(log.jsCallbacks.size(), 1u);
}

TEST(earpiece_to_speaker_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Earpiece, "earpiece-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 1);
}

TEST(a2dp_to_builtin_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::BluetoothA2dp, "a2dp-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 1);
}

TEST(builtin_to_a2dp_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::BluetoothA2dp, "a2dp-1"});

    ASSERT_EQ(log.restartCount, 0);
    ASSERT_EQ(log.driftResetCount, 1);
}

TEST(bt_sco_to_speaker_needs_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::BluetoothSco, "bt-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.restartCount, 1);
}

TEST(wired_to_speaker_needs_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::WiredHeadset, "wired-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.restartCount, 1);
}

TEST(usb_to_bt_sco_needs_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::UsbDevice, "usb-1"});
    log.reset();

    h.onRouteDetected({AudioRoute::BluetoothSco, "bt-1"});

    ASSERT_EQ(log.restartCount, 1);
}

TEST(unknown_to_anything_no_restart) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Unknown, ""});
    log.reset();

    h.onRouteDetected({AudioRoute::BluetoothSco, "bt-1"});

    ASSERT_EQ(log.restartCount, 0);
}

TEST(drift_reset_on_route_change_not_device_only) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    // Same route type, different device — no drift reset
    h.onRouteDetected({AudioRoute::Speaker, "speaker-2"});

    ASSERT_EQ(log.driftResetCount, 0);
    ASSERT_EQ(log.jsCallbacks.size(), 1u);
}

TEST(reset_clears_state) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::BluetoothSco, "bt-1"});

    ASSERT_TRUE(h.initialized());
    ASSERT_EQ(h.currentRoute(), AudioRoute::BluetoothSco);

    h.reset();

    ASSERT_FALSE(h.initialized());
    ASSERT_EQ(h.currentRoute(), AudioRoute::Unknown);
}

TEST(not_initialized_ignores_route_detected) {
    CallbackLog log;
    auto h = makeHandler(log);

    // No onInitialRoute called
    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});

    ASSERT_EQ(log.jsCallbacks.size(), 0u);
    ASSERT_EQ(log.restartCount, 0);
}

TEST(reset_then_reinitialize) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    h.reset();
    log.reset();

    h.onInitialRoute({AudioRoute::BluetoothSco, "bt-1"});

    ASSERT_EQ(h.currentRoute(), AudioRoute::BluetoothSco);
    ASSERT_EQ(log.jsCallbacks.size(), 1u);
    ASSERT_EQ(log.restartCount, 0);
}

TEST(needsRestart_matrix) {
    // Same route
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::Speaker));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::BluetoothSco, AudioRoute::BluetoothSco));

    // From unknown
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Unknown, AudioRoute::Speaker));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Unknown, AudioRoute::BluetoothSco));

    // Builtin <-> builtin
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::Earpiece));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Earpiece, AudioRoute::Speaker));

    // A2DP <-> builtin
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::BluetoothA2dp, AudioRoute::Speaker));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::BluetoothA2dp));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::BluetoothA2dp, AudioRoute::Earpiece));
    ASSERT_FALSE(RouteHandler::needsRestart(AudioRoute::Earpiece, AudioRoute::BluetoothA2dp));

    // SCO <-> builtin: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::BluetoothSco, AudioRoute::Speaker));
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::BluetoothSco));

    // Wired <-> builtin: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::WiredHeadset, AudioRoute::Speaker));
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::WiredHeadset));

    // USB <-> builtin: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::UsbDevice, AudioRoute::Speaker));
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::Speaker, AudioRoute::UsbDevice));

    // SCO <-> A2DP: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::BluetoothSco, AudioRoute::BluetoothA2dp));
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::BluetoothA2dp, AudioRoute::BluetoothSco));

    // Wired <-> SCO: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::WiredHeadset, AudioRoute::BluetoothSco));

    // USB <-> SCO: restart
    ASSERT_TRUE(RouteHandler::needsRestart(AudioRoute::UsbDevice, AudioRoute::BluetoothSco));
}

TEST(multiple_route_changes_accumulate) {
    CallbackLog log;
    auto h = makeHandler(log);

    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    log.reset();

    // Speaker -> BT SCO (restart)
    h.onRouteDetected({AudioRoute::BluetoothSco, "bt-1"});
    ASSERT_EQ(log.restartCount, 1);

    // BT SCO -> Earpiece (restart)
    h.onRouteDetected({AudioRoute::Earpiece, "earpiece-1"});
    ASSERT_EQ(log.restartCount, 2);

    // Earpiece -> Speaker (no restart)
    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});
    ASSERT_EQ(log.restartCount, 2);

    ASSERT_EQ(log.jsCallbacks.size(), 3u);
    ASSERT_EQ(log.driftResetCount, 3);
}

TEST(routeChangeCount_is_monotonic_and_deduped) {
    CallbackLog log;
    RouteHandler h;
    h.setCallbacks({
        .restartStreams = [&log]() { log.restartCount++; },
        .fireJsCallback = [&log](AudioRoute r) { log.jsCallbacks.push_back(r); },
        .resetDrift = [&log]() { log.driftResetCount++; },
    });

    // Initial route does NOT bump the change counter (not a change, it's first).
    h.onInitialRoute({AudioRoute::Speaker, "speaker-1"});
    ASSERT_EQ(h.routeChangeCount(), 0u);

    // Same route, same device: dedup path — no counter bump.
    h.onRouteDetected({AudioRoute::Speaker, "speaker-1"});
    ASSERT_EQ(h.routeChangeCount(), 0u);

    // Different route: bump.
    h.onRouteDetected({AudioRoute::BluetoothSco, "sco-1"});
    ASSERT_EQ(h.routeChangeCount(), 1u);

    // Different device on the same route type still counts as a change.
    h.onRouteDetected({AudioRoute::BluetoothSco, "sco-2"});
    ASSERT_EQ(h.routeChangeCount(), 2u);

    // Another route change.
    h.onRouteDetected({AudioRoute::Earpiece, "earpiece-1"});
    ASSERT_EQ(h.routeChangeCount(), 3u);

    // reset() resets confirmed_/initialized_ but counter is monotonic for the
    // session — verify by looking up the current value after reset().
    h.reset();
    // After reset, counter is preserved (session-long signal).
    ASSERT_EQ(h.routeChangeCount(), 3u);
}

// ============================================================
// Main
// ============================================================

TEST_MAIN("RouteHandler Tests")
