#pragma once

#include "PpbTouchAPI.h"

#include <array>
#include <cstdint>
#include <limits>

// Main-thread owned registrations. Posted tasks retain copied data, never client context.
class PpbFrameSubscriptions {
public:
    using Token = unsigned long long;
    static constexpr std::size_t kCapacity = 8;

    Token Add(PPBAPI::PpbFrameCallback callback, void* context)
    {
        if (!callback || nextToken_ == (std::numeric_limits<Token>::max)()) return 0;
        for (auto& entry : entries_) {
            if (entry.active || entry.inFlight) continue;
            entry = {++nextToken_, callback, context, true, 0};
            return entry.token;
        }
        return 0;
    }

    PPBAPI::PpbRemovalState Remove(Token token)
    {
        if (!token || token > nextToken_) return PPBAPI::PpbRemovalState::Unknown;
        for (auto& entry : entries_) {
            if (entry.token != token) continue;
            entry.active = false;
            if (entry.inFlight) return PPBAPI::PpbRemovalState::Pending;
            entry.callback = nullptr;
            entry.context = nullptr;
            return PPBAPI::PpbRemovalState::Drained;
        }
        return PPBAPI::PpbRemovalState::Drained;
    }

    std::array<Token, kCapacity> Recipients() const
    {
        std::array<Token, kCapacity> result{};
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].active) result[i] = entries_[i].token;
        return result;
    }

    void Deliver(Token token, const PPBAPI::PpbFrameEvent& event,
                 const PPBAPI::PpbTouchContact* contacts)
    {
        for (auto& entry : entries_) {
            if (!entry.active || entry.token != token) continue;
            ++entry.inFlight;
            try {
                entry.callback(&event, contacts, entry.context);
            } catch (...) {
                // A client exception retires that registration after this invocation.
                entry.active = false;
            }
            --entry.inFlight;
            if (!entry.active && !entry.inFlight) {
                entry.callback = nullptr;
                entry.context = nullptr;
            }
            return;
        }
    }

private:
    struct Entry {
        Token token = 0;
        PPBAPI::PpbFrameCallback callback = nullptr;
        void* context = nullptr;
        bool active = false;
        std::uint32_t inFlight = 0;
    };

    std::array<Entry, kCapacity> entries_{};
    Token nextToken_ = 0;
};
