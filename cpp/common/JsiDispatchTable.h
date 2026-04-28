// Table-driven JSI method registration with centralized lifetime guard.
//
// Replaces the MethodId enum + lookupMethod() + switch-case + getPropertyNames()
// pattern with a single declarative table. Each method is registered once;
// at install time, buildApiObject() creates a plain jsi::Object with each
// method pre-bound as a Function property. JS accesses (e.g.
// __MediaPipeline.feedData(buf)) hit Hermes' native property hashtable —
// no per-call prop.utf8 alloc, no map lookup, no Function recreation.
//
// Per Margelo's "Make JSI run faster" (2024), this is ~5× faster than the
// HostObject + dispatch pattern (which forces virtual dispatch, std::string
// allocation, and Function creation on every property access).
//
// Usage:
//   table_.method("feedData", 1, [this](auto& rt, auto* args, size_t count) {
//       return feedData(rt, args[0]);
//   });
//
//   // At install time (once):
//   auto api = table_.buildApiObject(rt, weakOwner_);
//   rt.global().setProperty(rt, "__MediaPipeline", std::move(api));
#pragma once

#include <jsi/jsi.h>
#include <functional>
#include <vector>

namespace mediamodule {

namespace jsi = facebook::jsi;

// What to return when the owning module has been destroyed. JSI numbers round-trip
// to JS as a single `number` type, so int 0 and double 0.0 are indistinguishable
// for JS consumers — only one zero variant is retained.
enum class DeadReturn : uint8_t {
    False = 0,
    Undefined,
    Null,
    Zero
};

class JsiDispatchTable {
public:
    using Handler = std::function<jsi::Value(jsi::Runtime&, const jsi::Value*, size_t)>;

    // Register a method. Call during init (before buildApiObject).
    // `name` must outlive the table (typically a string literal).
    void method(const char* name, size_t argCount, Handler handler,
                DeadReturn dead = DeadReturn::False) {
        entries_.push_back({name, argCount, std::move(handler), dead});
    }

    // Build the API object: a plain jsi::Object with each registered method
    // pre-bound as a Function property. Call once at install time.
    //
    // Each Function captures a copy of the weak_ptr<void> owner — the
    // centralized lifetime guard. If the owner has been destroyed by the
    // time JS invokes the function, the Function returns the registered
    // dead value instead of touching the freed orchestrator.
    //
    // The returned Object is the JSI surface — JS does
    // `__MediaPipeline.feedData(buf)` and Hermes resolves `feedData` via
    // its property hashtable directly into the cached Function. No
    // std::string allocation, no virtual dispatch, no per-call Function
    // creation (vs the prior HostObject pattern).
    jsi::Object buildApiObject(jsi::Runtime& rt,
                                const std::weak_ptr<void>& owner) const {
        jsi::Object obj(rt);
        for (const auto& entry : entries_) {
            auto weak = owner;
            auto handler = entry.handler;  // copy std::function once, at install
            auto dead = entry.dead;
            auto fn = jsi::Function::createFromHostFunction(rt,
                jsi::PropNameID::forAscii(rt, entry.name),
                static_cast<unsigned int>(entry.argCount),
                [weak, handler = std::move(handler), dead](
                    jsi::Runtime& rt, const jsi::Value&,
                    const jsi::Value* args, size_t count) -> jsi::Value {
                    // Bind the shared_ptr for the full call scope; `if (!weak.lock())`
                    // would destroy the temporary at the `;`, leaving the handler's
                    // captured `this` unanchored for its execution.
                    auto owner = weak.lock();
                    if (!owner) return deadValue(rt, dead);
                    return handler(rt, args, count);
                });
            obj.setProperty(rt, entry.name, std::move(fn));
        }
        return obj;
    }

private:
    static jsi::Value deadValue(jsi::Runtime&, DeadReturn type) {
        switch (type) {
            case DeadReturn::Undefined: return jsi::Value::undefined();
            case DeadReturn::Null:      return jsi::Value::null();
            case DeadReturn::Zero:      return jsi::Value(0);
            default:                    return jsi::Value(false);
        }
    }

    struct Entry {
        const char* name;
        size_t argCount;
        Handler handler;
        DeadReturn dead;
    };
    std::vector<Entry> entries_;
};

}  // namespace mediamodule
