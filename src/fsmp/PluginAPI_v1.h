#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PluginAPI_v1.h — the SMP plugin interface at MAJOR VERSION 1, used by
// HDT-SMP Flex (OgreWorks, "hdtSMP64.dll" with a capital SMP; 8.0.17-Alpha
// reports interface 1.0.0 / bullet 3.24.0).
//
// TRANSCRIBED VERBATIM from the upstream Skyrim VR fork this lineage comes
// from: github.com/alandtse/hdtSMP64 — hdtSMP64/PluginAPI.h + IEventListener.h.
// Namespaced `hdtv1` so it can coexist with fsmp/PluginAPI.h (interface 2.0.0,
// pinned to the Faster HDT-SMP v4.0.1 tag) in one translation unit.
//
// ── WHY A SECOND HEADER EXISTS ──────────────────────────────────────────────
// v1 and v2 are IDENTICAL in every respect that PPB touches except ONE, and
// that one is fatal:
//
//   PluginInterface vtable   v1 == v2   (6 slots, same declaration order)
//   PreStepEvent  layout     v1 == v2   ({ const btAlignedObjectArray<...>&
//   PostStepEvent layout     v1 == v2      objects; float timeStep{0.f}; })
//   BULLET_VERSION           v1 == v2   ({ 3, 24, 0 })
//   MSG_STARTUP              v1 == v2   (0)
//   Version / VersionInfo    v1 == v2   ({int major,minor,patch} x2)
//
//   LISTENER BASE            v1 != v2   ← the entire incompatibility
//     v1: hdt::IEventListener<T>   NO virtual dtor  → onEvent(const T&) is SLOT 0
//     v2: RE::BSTEventSink<T>      virtual dtor     → dtor is SLOT 0,
//                                                     ProcessEvent is SLOT 1
//
// So handing a v1 engine a BSTEventSink-derived object makes the engine call
// our DESTRUCTOR once per physics step, with the event reference in the
// argument registers. That is why FsmpLink's interface-major gate is a hard
// reject and MUST stay one — the fix is not to loosen the gate but to hand the
// v1 engine a v1-SHAPED listener, which is what this header enables.
//
// Verified three independent ways (2026-07-29):
//   1. Upstream source above — INTERFACE_VERSION{1,0,0}, BULLET_VERSION{3,24,0}.
//   2. Flex's own shipped PDB (mods/HDT-SMP Flex/SKSE/Plugins/hdtSMP64.pdb):
//      ?addListener@PluginInterfaceImpl@hdt@@UEAAXPEAV?$IEventListener@UPreStepEvent@hdt@@@2@@Z
//      ?addListener@...@UPostStepEvent@...  ?removeListener@... (both)
//      ?getVersionInfo@PluginInterfaceImpl@hdt@@UEBAAEBUVersionInfo@...
//      i.e. the same five virtuals, parameterised on IEventListener<T>, and the
//      symbols hdt::SkyrimPhysicsWorld::onEvent / hdt::ActorManager::onEvent
//      exist while `ProcessEvent` appears ONLY as unrelated Skyrim engine hooks.
//   3. A live SMP Flex user log: "MSG_STARTUP from 'hdtSMP64' — interface
//      1.0.0, bullet 3.24.0", i.e. the constants below, observed at runtime.
//
// ⚠ NOT YET PROVEN IN GAME. Flex is installed but DISABLED in this profile, so
// the v1 path has never executed. The receipt to look for is the existing
// "FSMPLINK LIVE: first PostStep observed" line naming sender 'hdtSMP64'.
// ─────────────────────────────────────────────────────────────────────────────

template <typename T> class btAlignedObjectArray;
class btCollisionObject;

namespace hdtv1
{
    // Upstream IEventListener.h, verbatim. The absence of a virtual destructor
    // is LOAD-BEARING: it puts onEvent at vtable slot 0. Do not add one, and do
    // not add any virtual member to a derived sink ahead of onEvent.
    template <class Event = void>
    class IEventListener
    {
    public:
        virtual void onEvent(const Event&) = 0;
    };

    // Sent right before the physics simulation begins updating.
    // Only forces and torques may be applied during this event.
    struct PreStepEvent
    {
        const btAlignedObjectArray<btCollisionObject*>& objects;
        float timeStep{ 0.0f };
    };

    // Sent right after the physics simulation has finished updating.
    // Read-only in every regard.
    struct PostStepEvent
    {
        const btAlignedObjectArray<btCollisionObject*>& objects;
        float timeStep{ 0.0f };
    };

    using IPreStepListener  = IEventListener<PreStepEvent>;
    using IPostStepListener = IEventListener<PostStepEvent>;

    class PluginInterface
    {
    public:
        enum MessageType : unsigned long
        {
            MSG_STARTUP,
        };

        struct Version
        {
            int major;
            int minor;
            int patch;
        };

        struct VersionInfo
        {
            Version interfaceVersion;
            Version bulletVersion;
        };

    public:
        constexpr static Version INTERFACE_VERSION{ 1, 0, 0 };
        constexpr static Version BULLET_VERSION{ 3, 24, 0 };

    public:
        virtual ~PluginInterface() = default;

        virtual const VersionInfo& getVersionInfo() const = 0;

        virtual void addListener(IPreStepListener*) = 0;
        virtual void removeListener(IPreStepListener*) = 0;

        virtual void addListener(IPostStepListener*) = 0;
        virtual void removeListener(IPostStepListener*) = 0;
    };
}
