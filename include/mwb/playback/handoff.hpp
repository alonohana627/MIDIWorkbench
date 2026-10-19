#pragma once

#include "mwb/core/error.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mwb::playback {

/// Bounded publication of immutable objects (usually playback snapshots) from a non-real-time
/// owner thread to the render thread (specification section 20.3).
///
/// Ownership never crosses to the render thread: every published object is held by a
/// `std::shared_ptr` in an owner-side slot. The render thread only exchanges slot indices through
/// atomics and reads raw pointers; when it stops using an object it marks the slot retired, and
/// the owner destroys retired objects in collect_retired(). The render thread therefore never
/// releases the last reference of a large object, and no `atomic<shared_ptr>` is needed.
///
/// Threading: publish(), collect_retired() and reset() are called by one owner thread;
/// acquire_pending(), current() and retire_previous() by one render thread.
template <class T, std::size_t SlotCount = 8>
class RetiringHandoff {
    static_assert(SlotCount >= 4, "a handoff needs room for current, previous, pending and new objects");

public:
    RetiringHandoff() {
        for (auto& state : states_)
            state.store(slot_free, std::memory_order_relaxed);
    }
    RetiringHandoff(const RetiringHandoff&) = delete;
    RetiringHandoff& operator=(const RetiringHandoff&) = delete;

    // --- Owner thread -------------------------------------------------------------------------

    /// Publishes a new object. A previously published object the render thread never picked up is
    /// discarded (on this thread). Fails with ErrorCode::busy when every slot is still in use
    /// because the render thread has not retired older objects.
    [[nodiscard]] Status publish(std::shared_ptr<const T> value) {
        if (!value)
            return make_error(ErrorCode::invalid_argument, "cannot publish an empty object");
        collect_retired();
        std::size_t slot = SlotCount;
        for (std::size_t index = 0; index < SlotCount; ++index) {
            if (states_[index].load(std::memory_order_acquire) == slot_free) {
                slot = index;
                break;
            }
        }
        if (slot == SlotCount)
            return make_error(ErrorCode::busy, "the render thread has not retired earlier snapshots yet");
        owned_[slot] = std::move(value);
        pointers_[slot] = owned_[slot].get();
        states_[slot].store(slot_published, std::memory_order_release);
        const int previous = pending_.exchange(static_cast<int>(slot), std::memory_order_acq_rel);
        if (previous >= 0) {
            // The render thread never took it (the exchange is atomic), so it is ours to free.
            owned_[static_cast<std::size_t>(previous)].reset();
            states_[static_cast<std::size_t>(previous)].store(slot_free, std::memory_order_release);
        }
        ++published_;
        return ok();
    }

    /// Destroys objects the render thread retired; returns how many were destroyed.
    std::size_t collect_retired() {
        std::size_t collected = 0;
        for (std::size_t index = 0; index < SlotCount; ++index) {
            if (states_[index].load(std::memory_order_acquire) == slot_retired) {
                owned_[index].reset();
                states_[index].store(slot_free, std::memory_order_release);
                ++collected;
            }
        }
        return collected;
    }

    /// Releases everything. Only valid while the render thread is not running.
    void reset() {
        pending_.store(-1, std::memory_order_relaxed);
        current_slot_ = -1;
        previous_slot_ = -1;
        for (std::size_t index = 0; index < SlotCount; ++index) {
            owned_[index].reset();
            pointers_[index] = nullptr;
            states_[index].store(slot_free, std::memory_order_relaxed);
        }
    }

    /// Number of successful publications.
    [[nodiscard]] std::uint64_t published_count() const noexcept { return published_; }
    /// Retirement epoch acknowledged by the render thread (objects it stopped using).
    [[nodiscard]] std::uint64_t retired_count() const noexcept { return retired_.load(std::memory_order_acquire); }

    // --- Render thread ------------------------------------------------------------------------

    /// Takes the newest published object, if any, and makes it current. The previous current
    /// object stays valid until retire_previous() is called. Returns nullptr when nothing new.
    [[nodiscard]] const T* acquire_pending() noexcept {
        const int slot = pending_.exchange(-1, std::memory_order_acq_rel);
        if (slot < 0)
            return nullptr;
        if (previous_slot_ >= 0)
            retire_slot(previous_slot_);
        previous_slot_ = current_slot_;
        current_slot_ = slot;
        return pointers_[static_cast<std::size_t>(slot)];
    }

    /// Marks the object replaced by the last acquire_pending() as retired.
    void retire_previous() noexcept {
        if (previous_slot_ >= 0) {
            retire_slot(previous_slot_);
            previous_slot_ = -1;
        }
    }

    /// Object currently used by the render thread, or nullptr.
    [[nodiscard]] const T* current() const noexcept {
        return current_slot_ >= 0 ? pointers_[static_cast<std::size_t>(current_slot_)] : nullptr;
    }

private:
    static constexpr std::uint8_t slot_free = 0;
    static constexpr std::uint8_t slot_published = 1;
    static constexpr std::uint8_t slot_retired = 2;

    void retire_slot(int slot) noexcept {
        states_[static_cast<std::size_t>(slot)].store(slot_retired, std::memory_order_release);
        retired_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::array<std::shared_ptr<const T>, SlotCount> owned_{};
    std::array<const T*, SlotCount> pointers_{};
    std::array<std::atomic<std::uint8_t>, SlotCount> states_{};
    std::atomic<int> pending_{-1};
    std::atomic<std::uint64_t> retired_{0};
    std::uint64_t published_ = 0;
    int current_slot_ = -1;   ///< Render-thread private.
    int previous_slot_ = -1;  ///< Render-thread private.
};

} // namespace mwb::playback
