#pragma once
#include "PCH.h"

// ============================================================================
//  HIGGS interface shim (ported verbatim from AIHands for the PivFix grab
//  gate, 2026-07-07).
// ----------------------------------------------------------------------------
//  CommonLibVR-compatible re-declaration of HiggsPluginAPI::IHiggsInterface001
//  (source: tools/_research/higgs/include/higgsinterface001.h).  The original
//  header pulls skse64/* types that collide with RE::, so we hand-match the
//  vtable here: EVERY virtual is re-declared in EXACT order so the offsets line
//  up, with skse64 types translated to their RE:: equivalents (TESObjectREFR*,
//  TESForm*, NiObject*, NiTransform, BSFixedString — all identical layout/ABI).
//  NEVER instantiate — only reinterpret the HIGGS-returned ptr.
// ============================================================================
namespace Higgs {

    struct IHiggsInterface001
    {
        using PulledCallback    = void (*)(bool isLeft, RE::TESObjectREFR* refr);
        using GrabbedCallback   = void (*)(bool isLeft, RE::TESObjectREFR* refr);
        using DroppedCallback   = void (*)(bool isLeft, RE::TESObjectREFR* refr);
        using StashedCallback   = void (*)(bool isLeft, RE::TESForm* form);
        using ConsumedCallback  = void (*)(bool isLeft, RE::TESForm* form);
        using CollisionCallback = void (*)(bool isLeft, float mass, float separatingVelocity);
        using StartTwoHandingCallback = void (*)();
        using StopTwoHandingCallback  = void (*)();
        enum class CollisionFilterComparisonResult : std::uint8_t { Continue, Collide, Ignore };
        using CollisionFilterComparisonCallback = CollisionFilterComparisonResult (*)(void*, std::uint32_t, std::uint32_t);
        using PrePhysicsStepCallback = void (*)(void*);
        using NoArgCallback          = void (*)();

        virtual unsigned int      GetBuildNumber() = 0;                                            // 0
        virtual void              AddPulledCallback(PulledCallback) = 0;                           // 1
        virtual void              AddGrabbedCallback(GrabbedCallback) = 0;                         // 2
        virtual void              AddDroppedCallback(DroppedCallback) = 0;                         // 3
        virtual void              AddStashedCallback(StashedCallback) = 0;                         // 4
        virtual void              AddConsumedCallback(ConsumedCallback) = 0;                       // 5
        virtual void              AddCollisionCallback(CollisionCallback) = 0;                     // 6
        virtual void              GrabObject(RE::TESObjectREFR*, bool) = 0;                        // 7
        virtual RE::TESObjectREFR* GetGrabbedObject(bool) = 0;                                     // 8
        virtual bool              IsHandInGrabbableState(bool) = 0;                                // 9
        virtual void              DisableHand(bool) = 0;                                           // 10
        virtual void              EnableHand(bool) = 0;                                            // 11
        virtual bool              IsDisabled(bool) = 0;                                            // 12
        virtual void              DisableWeaponCollision(bool) = 0;                                // 13
        virtual void              EnableWeaponCollision(bool) = 0;                                 // 14
        virtual bool              IsWeaponCollisionDisabled(bool) = 0;                             // 15
        virtual bool              IsTwoHanding() = 0;                                              // 16
        virtual void              AddStartTwoHandingCallback(StartTwoHandingCallback) = 0;         // 17
        virtual void              AddStopTwoHandingCallback(StopTwoHandingCallback) = 0;           // 18
        virtual bool              CanGrabObject(bool) = 0;                                         // 19
        virtual void              AddCollisionFilterComparisonCallback(CollisionFilterComparisonCallback) = 0; // 20
        virtual void              AddPrePhysicsStepCallback(PrePhysicsStepCallback) = 0;           // 21
        virtual std::uint64_t     GetHiggsLayerBitfield() = 0;                                     // 22
        virtual void              SetHiggsLayerBitfield(std::uint64_t) = 0;                        // 23
        virtual RE::NiObject*     GetHandRigidBody(bool) = 0;                                      // 24
        virtual RE::NiObject*     GetWeaponRigidBody(bool) = 0;                                    // 25
        virtual RE::NiObject*     GetGrabbedRigidBody(bool) = 0;                                   // 26  (really bhkRigidBody)
        virtual void              ForceWeaponCollisionEnabled(bool) = 0;                           // 27
        virtual bool              IsHoldingObject(bool) = 0;                                       // 28
        virtual void              GetFingerValues(bool, float[5]) = 0;                             // 29
        virtual void              AddPreVrikPreHiggsCallback(NoArgCallback) = 0;                   // 30
        virtual void              AddPreVrikPostHiggsCallback(NoArgCallback) = 0;                  // 31
        virtual void              AddPostVrikPreHiggsCallback(NoArgCallback) = 0;                  // 32
        virtual void              AddPostVrikPostHiggsCallback(NoArgCallback) = 0;                 // 33
        virtual bool              Deprecated1(const std::string_view&, double&) = 0;               // 34
        virtual bool              Deprecated2(const std::string&, double) = 0;                     // 35
        virtual RE::NiTransform   GetGrabTransform(bool) = 0;                                      // 36
        virtual void              SetGrabTransform(bool, const RE::NiTransform&) = 0;              // 37
        virtual bool              GetSettingDouble(const char*, double&) = 0;                      // 38
        virtual bool              SetSettingDouble(const char*, double) = 0;                       // 39
        virtual RE::BSFixedString GetGrabbedNodeName(bool) = 0;                                    // 40
    };

}
