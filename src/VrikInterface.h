#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// VrikInterface.h — minimal vendored VRIK plugin interface (touch API, 2026-07-30).
//
// VTABLE COPIED VERBATIM from prog's published vrikinterface001.h (the copy HIGGS
// vendors at tools/_research/higgs/include/vrikinterface001.h) — the house rule:
// never retype an interface from memory. Types adapted to CommonLib (RE::NiPoint3
// for NiPoint3, same layout); slot order untouched.
//
// ⚠ TRUNCATED after restoreFingers (slot 13). PPB calls ONLY getBuildNumber (0)
// and getFingerPos (11). Never call past the truncation point — re-vendor the
// full header first if a later slot is ever needed.
//
// WHY: the touch API classifies FINGER / PALM / FIST. Geometry could not — the
// measured tip-to-palm distance is CONSTANT (~3.7-4.3u) across gestures on this
// rig, because the third-person finger bones the boxes ride do not articulate
// with the controller. VRIK is the system that DOES know the controller-driven
// hand pose, and exposes it directly:
//     getFingerPos(isLeft, finger)  ->  0 = closed .. 1 = open
//     fingers: 0 thumb, 1 index, 2 middle, 3 ring, 4 pinky
// Index open + middle closed = pointing. Both closed = fist. Both open = palm.
// ─────────────────────────────────────────────────────────────────────────────

namespace Vrik {

    class IVrikInterface001 {
    public:
        // slot 00
        virtual unsigned int getBuildNumber() = 0;
        // slots 01..04 — settings
        virtual double getSettingDouble(const char* name) = 0;
        virtual void   setSettingDouble(const char* name, double value) = 0;
        virtual void   getSettingString(const char* name, char* buffer, size_t bufferSize) = 0;
        virtual void   setSettingString(const char* name, const char* value) = 0;
        // slots 05..06
        virtual void saveSettings() = 0;
        virtual void restoreSettings() = 0;
        // slots 07..10 — gesture profile registration
        typedef void (*GestureCallback)(int pressCount);
        virtual void addGestureAction(GestureCallback callback, const char* mcmMenuName) = 0;
        virtual void beginGestureProfile() = 0;
        virtual void setProfileAction(int gestureNumber, GestureCallback callback) = 0;
        virtual void endGestureProfile() = 0;
        // slot 11 — THE ONE WE USE: live finger position, 0 = closed .. 1 = open
        virtual float getFingerPos(bool isLeft, int fingerIndex) = 0;
        // slots 12..13
        virtual void setFingerRange(bool isLeft, float min0, float max0, float min1, float max1,
                                    float min2, float max2, float min3, float max3,
                                    float min4, float max4) = 0;
        virtual void restoreFingers(bool isLeft) = 0;
        // ⚠ TRUNCATED — more slots exist upstream (camera offsets etc.). Do not call
        // beyond restoreFingers through this declaration.
    };

    // The acquisition message — constant verbatim from vrikinterface001.cpp
    // ("Randomly chosen by cat").
    struct VrikMessage {
        enum : unsigned int { kMessage_GetInterface = 0xF2AFAEE6 };
        void* (*getApiFunction)(unsigned int revisionNumber) = nullptr;
    };
}
