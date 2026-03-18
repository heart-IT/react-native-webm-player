#pragma once

#include <atomic>
#include <mutex>
#include <array>
#include <bit>
#include <unordered_map>
#include "common/MediaConfig.h"
#include "MediaTypes.h"

namespace media {

enum class ParticipantState : uint8_t {
    Disconnected = 0,
    Connected,
    Active
};

// Thread safety: All public methods are thread-safe.
// - Atomic operations for state, slot assignment, and activity tracking
// - Registry operations (add/remove/get) protected by external mutex
class Participant {
public:
    explicit Participant(int id = -1) noexcept : id_(id) {}

    [[nodiscard]] int id() const noexcept { return id_.load(std::memory_order_relaxed); }

    void resetWithId(int newId) noexcept {
        id_.store(newId, std::memory_order_relaxed);
        isSelf_.store(false, std::memory_order_relaxed);
        state_.store(ParticipantState::Disconnected, std::memory_order_relaxed);
        audioSlot_.store(config::sentinel::kNoSlot, std::memory_order_relaxed);
        hasAudio_.store(false, std::memory_order_relaxed);
        isMuted_.store(false, std::memory_order_relaxed);
        lastActivityUs_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isSelf() const noexcept { return isSelf_.load(std::memory_order_relaxed); }
    void setIsSelf(bool self) noexcept { isSelf_.store(self, std::memory_order_relaxed); }

    [[nodiscard]] ParticipantState state() const noexcept { return state_.load(std::memory_order_acquire); }
    void setState(ParticipantState s) noexcept { state_.store(s, std::memory_order_release); }

    [[nodiscard]] bool isConnected() const noexcept {
        auto s = state();
        return s == ParticipantState::Connected || s == ParticipantState::Active;
    }

    [[nodiscard]] int audioSlot() const noexcept { return audioSlot_.load(std::memory_order_acquire); }

    void setAudioSlot(int slot) noexcept {
        audioSlot_.store(slot, std::memory_order_release);
    }

    [[nodiscard]] bool hasAudioSlot() const noexcept { return audioSlot() >= 0; }

    [[nodiscard]] bool hasAudio() const noexcept { return hasAudio_.load(std::memory_order_acquire); }
    void setHasAudio(bool has) noexcept { hasAudio_.store(has, std::memory_order_release); }

    [[nodiscard]] bool isMuted() const noexcept { return isMuted_.load(std::memory_order_acquire); }
    void setMuted(bool muted) noexcept { isMuted_.store(muted, std::memory_order_release); }

    [[nodiscard]] int64_t lastActivityUs() const noexcept { return lastActivityUs_.load(std::memory_order_relaxed); }
    void updateActivity() noexcept { lastActivityUs_.store(nowUs(), std::memory_order_relaxed); }

private:
    std::atomic<int> id_;
    std::atomic<bool> isSelf_{false};
    std::atomic<ParticipantState> state_{ParticipantState::Disconnected};
    std::atomic<int> audioSlot_{config::sentinel::kNoSlot};
    std::atomic<bool> hasAudio_{false};
    std::atomic<bool> isMuted_{false};
    std::atomic<int64_t> lastActivityUs_{0};
};

class ParticipantRegistry {
public:
    static constexpr int kMaxParticipants = config::limits::kMaxParticipants;
    static constexpr int kInvalidExternalId = -1;

    static_assert(kMaxParticipants <= 64, "Implementation uses uint64_t bitmask");

    ParticipantRegistry() noexcept {
        for (size_t i = 0; i < static_cast<size_t>(kMaxParticipants); ++i) {
            participants_[i].resetWithId(static_cast<int>(i));
            internalToExternal_[i] = kInvalidExternalId;
        }
    }

    // Returns pointer to participant slot. Pointer remains valid for the registry's lifetime
    // (slots are a fixed-size array, never reallocated). Caller must not cache the pointer
    // across remove() calls — the slot may be reused for a different participant.
    [[nodiscard]] Participant* get(int id) noexcept {
        if (id < 0 || id >= kMaxParticipants) return nullptr;
        auto idx = static_cast<size_t>(id);
        uint64_t bits = connectedBits_.load(std::memory_order_acquire);
        if ((bits & (1ULL << idx)) == 0) return nullptr;
        return &participants_[idx];
    }

    [[nodiscard]] const Participant* get(int id) const noexcept {
        if (id < 0 || id >= kMaxParticipants) return nullptr;
        auto idx = static_cast<size_t>(id);
        uint64_t bits = connectedBits_.load(std::memory_order_acquire);
        if ((bits & (1ULL << idx)) == 0) return nullptr;
        return &participants_[idx];
    }

    [[nodiscard]] Participant* acquireSlot() {
        std::lock_guard<std::mutex> lk(mtx_);

        int freeSlot = findFreeSlotLocked();
        if (freeSlot < 0) return nullptr;

        auto idx = static_cast<size_t>(freeSlot);
        participants_[idx].resetWithId(freeSlot);
        connectedBits_.fetch_or(1ULL << idx, std::memory_order_release);

        return &participants_[idx];
    }

    [[nodiscard]] Participant* findOrCreate(int externalId) {
        std::lock_guard<std::mutex> lk(mtx_);

        auto it = externalToInternal_.find(externalId);
        if (it != externalToInternal_.end()) {
            return &participants_[static_cast<size_t>(it->second)];
        }

        int freeSlot = findFreeSlotLocked();
        if (freeSlot < 0) return nullptr;

        auto idx = static_cast<size_t>(freeSlot);
        participants_[idx].resetWithId(freeSlot);
        externalToInternal_[externalId] = freeSlot;
        internalToExternal_[idx] = externalId;
        connectedBits_.fetch_or(1ULL << idx, std::memory_order_release);

        return &participants_[idx];
    }

    void remove(int id) noexcept {
        if (id < 0 || id >= kMaxParticipants) return;

        std::lock_guard<std::mutex> lk(mtx_);

        auto idx = static_cast<size_t>(id);

        // Clear bit first so concurrent get() stops returning this slot,
        // then reset the slot state.
        connectedBits_.fetch_and(~(1ULL << idx), std::memory_order_release);
        participants_[idx].resetWithId(id);

        int externalId = internalToExternal_[idx];
        if (externalId != kInvalidExternalId) {
            externalToInternal_.erase(externalId);
            internalToExternal_[idx] = kInvalidExternalId;
        }
    }

    [[nodiscard]] int count() const noexcept {
        return std::popcount(connectedBits_.load(std::memory_order_relaxed));
    }

    // Lock-free iteration over connected participants. Takes a snapshot of
    // connectedBits_ and visits each set slot. Safe from any thread because:
    // - Participant fields are all atomic — concurrent remove() resets are visible
    // - remove() clears the bit before resetting, so stale visits see a
    //   partially-reset slot (all fields atomic, no torn reads)
    // - Slots are a fixed array — pointers are always valid
    template<typename Func>
    void forEach(Func&& fn) noexcept {
        uint64_t snapshot = connectedBits_.load(std::memory_order_acquire);
        while (snapshot) {
            auto i = static_cast<size_t>(std::countr_zero(snapshot));
            fn(participants_[i]);
            snapshot &= snapshot - 1;
        }
    }

    template<typename Func>
    void forEach(Func&& fn) const noexcept {
        uint64_t snapshot = connectedBits_.load(std::memory_order_acquire);
        while (snapshot) {
            auto i = static_cast<size_t>(std::countr_zero(snapshot));
            fn(participants_[i]);
            snapshot &= snapshot - 1;
        }
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < static_cast<size_t>(kMaxParticipants); ++i) {
            participants_[i].resetWithId(static_cast<int>(i));
            internalToExternal_[i] = kInvalidExternalId;
        }
        connectedBits_.store(0, std::memory_order_release);
        externalToInternal_.clear();
    }

private:
    [[nodiscard]] int findFreeSlotLocked() const noexcept {
        uint64_t bits = connectedBits_.load(std::memory_order_relaxed);
        // Mask off bits beyond kMaxParticipants so countr_zero finds
        // a free slot within range even when kMaxParticipants < 64.
        constexpr uint64_t kFullMask = (kMaxParticipants == 64)
            ? ~0ULL : (1ULL << static_cast<unsigned>(kMaxParticipants)) - 1;
        uint64_t free = ~bits & kFullMask;
        if (free == 0) return -1;
        return std::countr_zero(free);
    }

    std::array<Participant, kMaxParticipants> participants_;
    mutable std::mutex mtx_;
    std::atomic<uint64_t> connectedBits_{0};
    std::unordered_map<int, int> externalToInternal_;
    std::array<int, kMaxParticipants> internalToExternal_;
};

}  // namespace media
