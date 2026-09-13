#include "PCH.h"
#include "OutfitGuard.h"
#include "HoldPool.h"
#include "Tuning.h"

#include <cstring>

namespace logger = SKSE::log;

namespace OutfitGuard
{
    namespace
    {
        // Actor::HasOutfitItems(BGSOutfit*) — VR 1.4.15 offset 0x29F420 (VR Address Library
        // version-1-4-15-0.csv row "19265,029f420"; CommonLibVR's Actor::HasOutfitItems wrapper is
        // the same ID). REL::Offset is the resolution path every other PPB hook uses (Hooks.cpp);
        // the plugin never loads the ID database, so REL::ID is not an option here.
        constexpr std::uintptr_t kVROffset = 0x29F420;

        using Fn = bool (*)(RE::Actor*, RE::BGSOutfit*);
        Fn            g_orig      = nullptr;
        bool          g_installed = false;
        std::uint32_t g_answers   = 0;      // how many times we said "yes, she has her outfit"

        bool Thunk(RE::Actor* a, RE::BGSOutfit* outfit)
        {
            // Runs on whichever thread loads her 3D / evaluates her AI. HoldPool::IsHeldByUs reads
            // 4-byte slot IDs and the quest's own locked alias lookup — safe off the main thread.
            if (a && ObjectHold::OutfitGuardOn() && HoldPool::IsHeldByUs(a)) {
                // ★ THE ANSWER THAT KEEPS HER AS SHE IS: "yes, her outfit is present" — the engine
                // then neither rebuilds it from the record nor re-equips anything. Receipt on the
                // first few and then every 64th (this runs on every 3D load / AI evaluation).
                if (g_answers < 4 || (g_answers & 63) == 0)
                    logger::info("OUTFITGUARD {:08X} '{}': HasOutfitItems answered TRUE (held in the pool) — no outfit re-issue",
                                 a->GetFormID(), a->GetDisplayFullName() ? a->GetDisplayFullName() : "?");
                ++g_answers;
                return true;
            }
            return g_orig ? g_orig(a, outfit) : false;
        }

        // Length of ONE instruction at p if it is on the whitelist of position-independent forms
        // MSVC emits in prologues; 0 = not whitelisted (anything relative, RIP-relative, two-byte
        // opcode or unknown). Whitelist: push r64; mov/xor/test/lea reg,reg or reg,[base+disp] (no
        // RIP-relative); sub/add/and r64, imm8|imm32.
        int InsnLen(const std::uint8_t* p)
        {
            int i = 0;
            if (p[i] >= 0x40 && p[i] <= 0x4F) ++i;                    // REX prefix
            const std::uint8_t op = p[i];
            if (op >= 0x50 && op <= 0x57) return i + 1;              // push r64
            if (op == 0x89 || op == 0x8B || op == 0x33 || op == 0x31 || op == 0x85 || op == 0x8D) {
                const std::uint8_t modrm = p[i + 1];
                const int mod = modrm >> 6, rm = modrm & 7;
                if (mod == 3) return i + 2;                            // reg, reg
                if (mod == 0 && rm == 5) return 0;                     // RIP-relative — refuse
                int len = i + 2;
                if (rm == 4) {                                         // SIB
                    len += 1;
                    if (mod == 0 && (p[i + 2] & 7) == 5) len += 4;    // no base: disp32
                }
                if (mod == 1) len += 1; else if (mod == 2) len += 4;   // disp8 / disp32
                return len;
            }
            if (op == 0x83 || op == 0x81) {                            // op r/m64, imm8 / imm32
                const std::uint8_t modrm = p[i + 1];
                if ((modrm >> 6) != 3) return 0;                       // register forms only
                return i + (op == 0x83 ? 3 : 6);
            }
            return 0;
        }
    }

    void Install()
    {
        if (g_installed) return;
        g_installed = true;
        if (!ObjectHold::OutfitGuardOn()) {
            logger::info("OUTFITGUARD: off (outfitGuard 0) — the engine may re-issue a stripped generic's outfit");
            return;
        }
        const std::uintptr_t base = REL::Module::get().base();
        const std::uintptr_t src  = base + kVROffset;
        const auto* p = reinterpret_cast<const std::uint8_t*>(src);
        char hex[64]; int hn = 0;
        for (int k = 0; k < 16 && hn < 60; ++k) hn += std::snprintf(hex + hn, sizeof(hex) - hn, "%02X ", p[k]);
        logger::info("OUTFITGUARD: Actor::HasOutfitItems at +0x{:X} — first bytes {}", kVROffset, hex);
        // decode the prologue: whole whitelisted instructions until >= 5 bytes are covered
        int steal = 0;
        while (steal < 5) {
            const int l = InsnLen(p + steal);
            if (l <= 0 || steal + l > 24) {
                logger::warn("OUTFITGUARD: prologue at +{} is not a whitelisted instruction ({:02X} {:02X} {:02X}) — NOT installing "
                             "(fail closed: another hook already sits on the entry, or an unlisted form — report 31 §8)",
                             steal, p[steal], p[steal + 1], p[steal + 2]);
                return;
            }
            steal += l;
        }
        // every installer funds its own trampoline bytes (Hooks.cpp convention)
        SKSE::AllocTrampoline(64);
        auto& tr = SKSE::GetTrampoline();
        // stub B — the ORIGINAL: stolen prologue, then an absolute jump to src + steal
        auto* stubB = static_cast<std::uint8_t*>(tr.allocate(static_cast<std::size_t>(steal) + 14));
        // stub A — the DETOUR TARGET: an absolute jump to the thunk (PPB.dll may sit > 2 GB from the exe)
        auto* stubA = static_cast<std::uint8_t*>(tr.allocate(14));
        if (!stubA || !stubB) {
            logger::warn("OUTFITGUARD: trampoline allocation failed — NOT installing");
            return;
        }
        std::memcpy(stubB, p, static_cast<std::size_t>(steal));
        const std::uint32_t zero = 0;
        stubB[steal] = 0xFF; stubB[steal + 1] = 0x25; std::memcpy(stubB + steal + 2, &zero, 4);
        const std::uint64_t back = static_cast<std::uint64_t>(src) + static_cast<std::uint64_t>(steal);
        std::memcpy(stubB + steal + 6, &back, 8);
        stubA[0] = 0xFF; stubA[1] = 0x25; std::memcpy(stubA + 2, &zero, 4);
        const std::uint64_t thunk = reinterpret_cast<std::uint64_t>(&Thunk);
        std::memcpy(stubA + 6, &thunk, 8);
        // the 5-byte JMP at the function entry must reach stub A (the trampoline lives near the exe)
        const std::int64_t rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stubA)) -
                                 static_cast<std::int64_t>(src + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) {
            logger::warn("OUTFITGUARD: trampoline out of rel32 range — NOT installing");
            return;
        }
        g_orig = reinterpret_cast<Fn>(stubB);
        std::uint8_t patch[24];
        patch[0] = 0xE9;
        const std::int32_t rel32 = static_cast<std::int32_t>(rel);
        std::memcpy(patch + 1, &rel32, 4);
        for (int k = 5; k < steal; ++k) patch[k] = 0x90;
        REL::safe_write(src, patch, static_cast<std::size_t>(steal));
        logger::info("OUTFITGUARD: installed — {} prologue bytes stolen; a hold-pool actor's outfit is never re-issued", steal);
    }
}
