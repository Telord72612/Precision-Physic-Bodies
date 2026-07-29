# 08 — Contact & Performance (the many-capsule design's one real hazard, SOLVED)

Combined reference (was Reports 11/12/13). Three sub-parts, each a synthesis of independent
investigations against the on-disk Havok 2010.2 SDK + PLANCK/HIGGS/PPB source:
- **Part A — hkpListShape performance** (what to measure; modelled from instruction counts).
- **Part B — the 256-contact-point cliff & two-actor scene physics** (the hazard, adjudicated).
- **Part C — no-LOD solutions to the cliff** (keep the fat orifice on both actors — which path).
Verdict (README): the cliff is UNDERSTOOD and CONFIRMED BENIGN in-VR (Havok drops overflow
gracefully, no CTD); the only cost is a mild perf stutter from contact VOLUME. Ship = full collision.

---

# 11 — `hkpListShape` Performance: The Authoritative Verdict on 96 Sub-Shapes per Actor

**Status:** synthesis of four independent investigations (`perf:narrowphase` 0.85, `perf:enabledchildren` 0.92, `perf:worstcase` 0.80, `perf:measure` 0.82), reconciled and adjudicated.
**Date:** 2026-07-08
**Applies to:** Precision Physic Bodies wave-2 bake (18 ragdoll bodies, ~96 `bhkCapsuleShape` children across 11 `bhkListShape` bodies).
**Nothing in this report is a measurement.** Every millisecond figure is modelled from disassembled instruction counts. The report exists to tell you *what to measure* and *what would make it unsafe* — §5 is the part that produces facts.

> **Marking convention used throughout:**
> **[V]** — VERIFIED. Read directly from headers, disassembly, source, or the NIF binary this session.
> **[I]** — INFERRED. Reasoned from verified facts. May be wrong.
> **[U]** — UNVERIFIED / UNKNOWN. We tried and could not establish it. Never build on these.
> **[D]** — DISPUTED between investigations. Adjudicated inline.

---

## 1. BOTTOM LINE

**~96 sub-shapes per actor is MARGINAL, not reckless — and the reason it is marginal has nothing to do with frame time.** The frame-time cost is real but small: broadphase is completely unaffected, narrowphase is culled before it runs, and even the pessimistic model puts the whole change at **+0.1–0.5 ms in the worst realistic scene** (1–4.5 % of an 11.1 ms frame) — *confidence 75 %, modelled not measured*. **The ship-blocker is a correctness cliff, not a cost:** Havok stages at most **256 contact points per body pair** with **no runtime clamp in a release build** [V], and our layout produces body pairs with **231 child pairs intra-actor (COM 21 × Spine2 11)** and **441 cross-actor (COM 21 × COM 21)** [V] — at 3–4 contact points per child pair, both can breach 256 [I]. **Do not bake COM at 21 children until the contact-point counter (§5) has run.**

---

## 2. ADJUDICATION — where the four investigations disagree

The four agents did not see the same thing. Three disagreements are material.

### 2.1 [D] Does the list narrowphase cull child pairs, or iterate all of them?

| Agent | Claim | Evidence |
|---|---|---|
| `perf:narrowphase` | **Culled.** A sorted 1-axis sweep over cached per-child AABBs. `hkListAgent3` (the naive one) **is not registered**. | Disassembled `hkpAgentRegisterUtil::_registerListAgents` — `hkListAgent3::registerAgent3` is never called. Disassembled `hkpCollectionCollectionAgent3::process_gatherShapeKeys` → `hk1AxisSweep::collide`. Disassembled `hkpCollectionAgent3::process` → wraps the convex side in a stack-temp 1-child list and delegates to CollectionCollection. |
| `perf:enabledchildren` | **Linear.** `hkListAgent3::process` emits *every* enabled child key each frame. "hkpListShape has no midphase AABB culling." | Disassembled `hkListAgent3::process` and found the linear key-emit loop. **Did not check whether that agent is registered.** |
| `perf:worstcase` | **Culled.** List-vs-list → `hkpCollectionCollectionAgent3` → `process_gatherShapeKeys` → `hk1AxisSweep`. | Headers + `.inl` only (`hkpListShape.h` `#include`s `hk1AxisSweep.h`; `hk1AxisSweep::IteratorAB` is a two-array sorted SAP). |

**Verdict: CULLED. I trust `perf:narrowphase`, ~85 %.** Reasons, in order of weight:

1. It is the only investigation that read the **registration function**. The other two reasoned about what a function *would* cost *if* it ran. `perf:enabledchildren` presented "the function the dispatcher registers as `Agent3Funcs::m_processFunc`" as a premise — that is an **[I] laundered as [V]**, and it is the one load-bearing error in an otherwise excellent report.
2. `_registerListAgents` brackets the two `registerAgent3` calls with `setEnableChecks(false)` / `setEnableChecks(true)` — Havok's idiom for "I am deliberately overwriting an existing table entry, suppress the assert." That is exactly what overwriting the LIST/LIST slot with the COLLECTION/COLLECTION agent looks like [I, high].
3. `perf:worstcase` reached the same conclusion **independently, from headers alone**, without seeing the disassembly. Two independent derivations beat one.
4. `hkpListShape.h` `#include`s `hk1AxisSweep.h`. A header does not include a sweep-and-prune algorithm it never uses.

**Both agents disassembled `hkListAgent3::process` and got byte-identical code.** They agree on what it does. They disagree on whether it runs. The registration disassembly settles it.

**Why this matters and why it also doesn't:**
- It matters for the **touch-window cost** (§4): the culled model gives ~30–80 live capsule agents on the touched actor; the linear model gives ~400–600. That is the difference between +0.02 ms and +0.2 ms.
- It does **not** matter for the **Enable-0 feature** (§3): the bitmask is consulted at query time on *both* paths [V by both agents independently].
- It does **not** matter for the **256-contact cliff** (§4.4): culling reduces *how often* you approach it, never *how high the ceiling is*.

**Residual risk:** ~15 %. If `perf:enabledchildren` is right and the naive agent runs, multiply every touch-window and crowd figure in §6 by ~5×. That still does not blow the frame budget — it takes the worst realistic case from ~1 ms to ~2.5 ms. **The perf conclusion is robust to this disagreement; the cost decomposition is not.** §5's protocol measures the truth either way.

### 2.2 [D] Is the per-child AABB cache live, and is there an always-on tax?

| Agent | Claim |
|---|---|
| `perf:narrowphase` | `hkpEntity::setCachedShapeData` **always** allocates `m_boundingVolumeData::m_childShapeAabbs` for a COLLECTION-alternate shape, on any motion type; `hkpEntityAabbUtil::entityBatchRecalcAabb` rebuilds + quantizes + **sorts** it once per body per frame, gated on `m_capacityChildShapeAabbs != 0`. **≈1.4 µs/actor/frame, paid even with zero contacts.** |
| `perf:enabledchildren` | `dumpbin -symbols` finds **zero** `BoundingVolumeData` references in `hkpListAgent3.obj`, `hkpBvTreeAgent3.obj`, `hkpAgent1nMachineProcess.obj`. "Not used on this path." |
| `perf:worstcase` | "Idle NPC, nobody touching → ~0.00 ms — child AABBs are only computed for bodies inside a collection agent." |

**Verdict: the cache is live; `perf:narrowphase` is right, ~90 %.** The two claims are not actually contradictory — `perf:enabledchildren` scanned the wrong object files. The consumer is `hkpCollectionCollectionAgent3.obj` (via `hkCollectionBvTreeAgent3_extractCachedAabbsOrRecalculate`, which reads `collidable->m_childShapeAabbs` and short-circuits to a recalc if `bvd.m_min[0] > bvd.m_max[0]`), and the producer is `hkpEntityAabbUtil` — neither of which was in `perf:enabledchildren`'s scan set. `perf:worstcase` asserted "0.00 ms" without disassembly; `perf:narrowphase` disassembled both the producer and the consumer.

**Consequence:** there is a small, unconditional, per-active-body tax that a bare `hkpCapsuleShape` does not pay at all (a bare capsule is not a collection, so `setCachedShapeData` allocates nothing and the pass never runs for it). **≈1.4 µs per 96-child actor per frame; 25 active ragdolls ⇒ ≈35 µs ⇒ 0.3 % of frame.** [I, from instruction counts; the 80-instr/child inner loop is an estimate, plausible range 50–200.]

This is the *only* cost that scales with crowd size regardless of what anyone is touching. It is also the cost that the enabled-children LOD (§3.6) eliminates for free, because `entityBatchRecalcAabb` walks the container via `getFirstKey`/`getNextKey`, which skip disabled children [V].

### 2.3 [D] Can PPB time the Havok step from the hooks it already owns?

| Agent | Proposal |
|---|---|
| `perf:narrowphase` | "Bracket the physics step: `rdtsc` at `0xB266AB` (pre-drive) and `0xB268DC` (post-physics)." |
| `perf:worstcase` | "HIGGS's `AddPrePhysicsStepCallback` + your existing post-physics `0xB268DC` hook." |
| `perf:measure` | **"`0xB266AB` fires in the *animation* phase, not the physics step. `driveToPose` sets motor targets; collision and the solver run later, in `bhkWorld::Update`. Measuring only here would produce a falsely reassuring number. This is the single most important thing to get right."** |

**Verdict: `perf:measure` is right, ~90 %.** `0xB266AB` and `0xB268DC` are **0x231 = 561 bytes apart** — almost certainly two call sites in the same routine (`BShkbAnimationGraph::UpdateAnimation`). Bracketing them contains the behaviour-graph work, not `hkpWorld::stepDeltaTime`. **Following `perf:narrowphase`'s protocol step 2 would have produced a number that looks great and means nothing.**

`perf:measure` found the correct seam, with source-line proof:
- **`0xDFB722` is a 5-byte `CALL` to `hkpWorld::stepDeltaTime(ahkpWorld*, float)` inside `bhkWorld::Update`** [V — `higgs/src/hooks.cpp:89, 704-717, 1419-1421`, where HIGGS `Write5Call`s it].
- **PLANCK does not hook it** [V — grep of `planck/src`; PLANCK consumes HIGGS's `AddPrePhysicsStepCallback` instead].
- Therefore a PPB `write_call<5>` at `kDataLoaded` lands cleanly on top of HIGGS, and the chain is correct whether or not HIGGS is present.

**Do not trust any timing taken from `0xB266AB`/`0xB268DC` until the phase-order trace (§5, step 9) proves where the step falls.** [U] whether `postPhysics` is genuinely after the Havok step in wall-clock order.

### 2.4 [D] Contact points per capsule-capsule child pair: 3 or 4? And is 256 a clamp or a corruption?

- `perf:narrowphase`: `hkpCapsuleCapsuleAgent` holds `hkContactPointId m_contactPointId[3]` ⇒ **3** [V]. Registered last in `_registerTerminalAgents`, after `hkPredGskAgent3::registerAgent3(0x13,0x13)`, so it overrides GSK for capsule pairs [I, high — the table-fill was not disassembled].
- `perf:worstcase`: **3** if the capsule-capsule agent runs, **4** if the GSK path runs (`hkpClosestPointManifold::HK_NUM_MAX_CONTACTS = 4`). Could not determine. Uses 4 for the safety bound.

**Verdict: expect 3; bound with 4.** Costs nothing to be conservative.

On the ceiling itself, the four reports say two different things and **nobody tested it**:
- `hkpProcessCollisionData.h:20` — `enum { HK_MAX_CONTACT_POINT = 256 }`, `hkpProcessCdPoint m_contactPoints[256]` [V]. `hkpProcessCollisionOutput::commitContactPoints(int n)` guards overflow with `HK_ASSERT2` **only** [V] — which compiles out of a release build ⇒ silent overrun into `m_toi` and beyond [I, high].
- The official 2010.2 User Guide says: *"Havok has a limit of **250 contact points** which may be created between two rigid bodies. If this limit is reached a warning is generated, and any more generated contact points will be **automatically rejected**."* [V] — i.e. a **graceful reject**, not a corruption.

These are probably two different stages: 256 is the agent-side staging buffer in `hkpProcessCollisionOutput` (assert-guarded), 250 is the `hkpSimpleConstraintContactMgr` / contact-atom limit (reject-guarded), and the staging buffer fills *before* the manager gets a chance to reject. **[U] — unresolved.** The honest position:

> **The 256-point-per-body-pair ceiling is a real structural constraint [V]. Whether breaching it degrades gracefully or corrupts memory is UNKNOWN. The cost of finding out empirically is a CTD in a save you care about. The cost of avoiding it is one contact listener. Avoid it.**

---

## 3. HOW HAVOK COST ACTUALLY SCALES WITH `hkpListShape` CHILDREN

### 3.1 The shape has no acceleration structure [V, 99 %]

`hkpListShape.h:30` — the class comment is Havok telling you the answer:
```cpp
/// If your list shape contains many subshapes, consider using a hkpBvTreeShape for faster access
class hkpListShape : public hkpShapeCollection
{
    hkArray<struct ChildInfo> m_childInfo;   // {const hkpShape*, u32 collisionFilterInfo, int shapeSize, int numChildShapes}
    hkUint16 m_flags;
    hkUint16 m_numDisabledChildren;
    hkVector4 m_aabbHalfExtents;             // <- the ONLY thing broadphase reads
    hkVector4 m_aabbCenter;                  // <-
    hkUint32 m_enabledChildren[8];           // 256-bit enable bitmask
};
```
No tree. No per-child AABB stored *in the shape*. But it `#include`s `hk1AxisSweep.h`, and its `disableChild` doc names `hkpCollidable::m_boundingVolumeData::m_childShapeAabbs`. Those two facts are the entire cost model.

**Skyrim's layout matches member-for-member** (`CommonLibVR-4.14.0/include/RE/H/hkpListShape.h`): `childInfo`@0x30, `flags`@0x40, `numDisabledChildren`@0x42, `aabbHalfExtents`@0x50, `aabbCenter`@0x60, `enabledChildren[8]`@0x70, `sizeof == 0x90`, `ChildInfo` stride 0x20 with `collisionFilterInfo` at +0x08 [V]. Strong evidence Bethesda link stock `hkpCollide` [I, 80 % — the exe's dispatcher init was not disassembled; `SkyrimVR.exe`'s `.text` is DRM-encrypted on disk, measured Shannon entropy **7.997 bits/byte** vs `.rdata` 4.80].

### 3.2 BROADPHASE — completely unaffected by child count [V, 97 %]

- One entity → one `hkpCollidable` → **exactly one** `hkpTypedBroadPhaseHandle`. There is no per-child proxy [V, `hkpCollidable.h:195`].
- `hkpListShape::getAabbImpl` **overrides** the slow base implementation. Its disassembly reads only `m_aabbHalfExtents` and `m_aabbCenter`, builds a transformed OBB→AABB, and **never touches `m_childInfo`**. O(1) [V].
  - By contrast `hkpShapeCollection::getAabbImpl` is documented as *"rather slow and just iterates over all children."* `hkpListShape` does not use it.

> **18 bodies with 96 children produce exactly the same 18 broadphase proxies and the same broadphase pair count as 18 bodies with 18 capsules.** The only broadphase lever is the *size* of the cached union AABB.

This is why `RepatchListAabb` (`tools/PPB-plugin/src/CapFix.cpp:125-152`) is load-bearing and why its comment is correct:
- **Union AABB too small** ⇒ the broadphase proxy under-covers the children; the body pair may never be created; children outside the box are **dead** [V]. Second-order: `entityBatchRecalcAabb` quantizes child AABBs relative to the collidable's own box via `hkVector4Util::convertToUint32WithClip` — a child outside the union gets **clipped**, corrupting the sweep [I, from the symbol name].
- **Union AABB too large** ⇒ more broadphase pairs, more midphase invocations, and for list-vs-MOPP, `hkBvTreeAgent3::calcAabbAndQueryTree` queries the world tree with the *body's* AABB, pulling in more world triangles. Narrowphase is protected by the sweep; the MOPP query is not [V, structural].

**Repatch tight, repatch always. PPB does. Keep doing it.**

### 3.3 MIDPHASE — a sorted 1-axis sweep, not a linear scan [V, 85 % — see §2.1]

Every list narrowphase — convex-vs-list, list-vs-list, list-vs-MOPP — funnels through **`hkpCollectionCollectionAgent3::process`**. (`hkpCollectionAgent3::process` literally wraps the convex body in a stack-temporary 1-child `hkpListShape` and delegates. Confirmed in the disassembly: `hkpListShape::hkpListShape(shapes, numShapes=1, refPolicy)` on the stack, then `call hkpCollectionCollectionAgent3::process`.) [V]

Its per-pair call graph, extracted from the disassembly of `process_gatherShapeKeys`:
```
2× hkCollectionBvTreeAgent3_extractCachedAabbsOrRecalculate(...)   ; pull the pre-sorted per-child AABB cache
2× hkBvTreeAgent3::getShapeCollectionIfBvTreeSupportsAabbQueries(...)
2× hkBvTreeAgent3::calcAabbAndQueryTree(...)                       ; only if one side is a MOPP
1× hk1AxisSweep::collide(pa,numA, pb,numB, pairsOut, maxNumPairs=0x7FF, numSkipped)
1× hkAlgorithm::quickSortRecursive<hk1AxisSweep::AabbInt>          ; only if the expansion term is nonzero
1× hkAlgorithm::quickSortRecursive<hkpShapeKeyPair>
```
`hk1AxisSweep` quantizes AABBs to `hkAabbUint32`, sorts ascending on `m_min[0]`, then:
```cpp
while (potentialPtr->m_min[0] < currentPtr->m_max[0]) {
    if (!yzDisjoint(*currentPtr, *potentialPtr)) return;   // overlap -> emit pair
    potentialPtr++;
}
```
`yzDisjoint` = 4 integer subtracts + 3 ORs + 1 AND against `0x80000000` ≈ **8–10 integer ops per candidate** [V].

Pair-output cap: `0x7FF = 2047` (`HK_MAX_NUM_HITS_PER_AABB_QUERY = 2048 = HK_MAX_AGENTS_IN_1N_MACHINE`). Our theoretical worst is 441. Not a constraint [V].

### 3.4 The real cost model

| Stage | Frequency | Complexity | Culled? |
|---|---|---|---|
| Broadphase | once per body | **O(1)** — cached union AABB | n/a |
| Child-AABB build + quantize + **sort** (`entityBatchRecalcAabb`) | **once per active body per frame** | **O(N log N)** | **NO — always-on tax** |
| memcpy cache → sweep buffer (+ optional expand & re-sort) | once per overlapping **body** pair | O(N_A + N_B) | — |
| `hk1AxisSweep::collide` | once per overlapping body pair | O(N_A + N_B + k); worst O(N_A·N_B) **integer** tests | — |
| capsule↔capsule narrowphase | **only surviving child pairs** | ~1,290 instructions each | **YES** |
| Contact points → solver Jacobians + listener callbacks | per contact point per solver iteration | linear | **NO** |
| **`castRay` / `linearCast`** | per ray crossing the body AABB | **O(children), no AABB rejection at all** | **NO** |

**Verified instruction counts** (`dumpbin /DISASM`, x86 release lib):
- `hkpCapsuleCapsuleAgent::processCollision` — **318** instructions, 0 sqrt/div.
- `hkCollideCapsuleUtilManifoldCapsVsCaps` — **970** instructions, 12 sqrt/div sites.
- ⇒ **≈1,290 instructions per surviving capsule-vs-capsule child pair ≈ 0.12–0.37 µs** at 3.5 GHz, IPC 2–3. **[I]** — this is an estimate from instruction count, not a measurement, and Skyrim's x64/SSE build will be leaner.

Also verified from the User Guide: wrapping children in `hkpTransformShape` costs *"broadphase 17 % slower … narrowphase 23 % slower."* **PPB uses bare `bhkCapsuleShape` children with baked endpoints** (`CapFix.cpp:129-152` only ever walks `RE::hkpShapeType::kCapsule`) — so you pay **none** of that. Correct design choice. Keep it.

### 3.5 ⚠ THE COST NOBODY ASKED ABOUT: RAYCASTS ARE O(children), UNCULLED [V, 96 %]

`hkpShapeCollection.h:79-81`:
> *"Implements the castRay function. Note that for shape collections with many sub-shapes this function can be very slow. It is better to use a `hkpBvTreeShape::castRay` instead."*

`hkpListShape::castRayImpl` overrides it — and the disassembly (141 instructions) is a **plain loop over children with a virtual `castRay` per child and no AABB rejection whatsoever** [V]. Same for `castRayWithCollectorImpl` [V]. Both honour the enabled-children bitmask [V].

SkyrimVR raycasts and linear-casts actor bodies constantly: HIGGS grab/loot selection (`hkpWorld_LinearCast` of a sphere every frame while the hand is near an NPC — `higgs/src/hand.cpp:302,348,459`), crosshair pick, projectiles, LOS.

**A ray through a torso that previously tested 3 capsules (COM + Spine1 + Spine2) now tests 43. That is ~14×.** It is invisible to any physics-step timer, it is not bounded by the sweep, and it is the single largest unculled multiplier in this change. [V structure; [I] impact magnitude — the actual ray count per frame was never measured.]

**The character-proxy path is the same shape of risk.** PLANCK routes the player's `hkpCharacterProxy` to collide with NPC ragdoll bodies (`enablePlayerBipedCollision`), and the proxy builds its manifold with `getClosestPoints`/`linearCast` every frame — dispatched to `hkpListAgent::staticGetClosestPoints` / `staticLinearCast`, whose implementations are absent from the vendored SDK. **[U] whether those AABB-reject children.** Cost is paid whenever the player stands near an NPC, touch or no touch.

### 3.6 List-vs-list worst case, honestly stated

126 (or 231, or 441) is the ceiling on **integer AABB candidate tests**, not on narrowphase calls. For COM(21) vs Thigh(6):
- Build cost: already paid in the per-body AABB pass.
- Per pair: memcpy 27 × 32 B ≈ 864 B (~200 instr), then the sweep. Worst case 126 × ~10 int ops ≈ **1,260 instructions ≈ 0.15 µs**.
- Narrowphase runs only on children whose AABBs actually overlap — typically 1–6 pairs for a hand on a torso, not 126.

**The one place our design fights the sweep:** the X-sweep + `yzDisjoint` culls on all three axes, and a capsule *stack along the spine* is spread on one axis, so most candidates die. But **concentric / nested capsules with near-identical AABBs do not cull** — and that is precisely the "thin core rod + fat flesh capsules at the same station" pattern in Report 09. If *k* of COM's 21 children are co-located, a partner capsule entering that region survives to narrowphase against **all k**, at k × 1,290 instructions and up to **3k contact points**.

**Practical rule that falls out: co-located children are the expensive kind. Spread-along-the-bone children are nearly free.**

### 3.7 What is NOT available to us

`hkpListShape.h` recommends `hkpBvTreeShape`. That is not reachable:
- **Legal in principle** — the User Guide's *Removing Child Shapes from Compound Shapes* says *"Compound dynamic bodies must have an `hkpListShape` optionally wrapped with an `hkpMoppBvTreeShape`."* [V]
- **Bake-time only.** MOPP code comes from `hkpMoppUtility::buildCode()`. Building it at runtime = allocation + a shape-*type* change on a live ragdoll body = **Pitfall Ledger rule 2, twice-proven CTD.**
- **It would probably be slower anyway.** `hkpEntity::setCachedShapeData` allocates the free, pre-sorted, per-frame child-AABB cache for COLLECTION-alternate shapes but *skips* it for BV_TREE-alternate shapes on keyframed/fixed bodies. Wrapping in a MOPP risks **losing the cache and paying a tree query per pair instead.** At N ≤ 21 a MOPP walk has more fixed overhead than a 21-element sorted sweep whose input was built for free. [V mechanism; [I] the net.]
- **[U]** whether Skyrim's NIF loader will even construct a `bhkMoppBvTreeShape` over a `bhkListShape`. Vanilla only ever pairs MOPP with `bhkCompressedMeshShape` / `bhkPackedNiTriStripsShape`. Do not assume.

> **Child count is the only lever Havok gives us. Plan accordingly.**

---

## 4. THE ENABLE-0 TRAP — AND THE REAL CHILD ON/OFF SWITCH

**This is the key feature finding of the whole investigation.**

### 4.1 The trap [V]

PPB's tuning-file `Enable 0` removes a capsule from *PPB's* dial list. It does not remove it from the `bhkListShape` baked into `skeleton_female.nif`. `hkpListShape::getChildShape(key, buffer)` is a **raw indexed accessor that does not consult any enable state** [V, disassembly]. So an `Enable 0` child:
- still has its AABB rebuilt every frame,
- still enters the sweep,
- still gets a narrowphase agent when it overlaps,
- still generates contact points,
- still gets hit by every raycast.

**`Enable 0` is a dialing convenience, not a physics switch. Every baked child collides forever.** If PPB ships wave-2 with `Enable 0` on the orifice inner capsules believing they are inert, they are not.

### 4.2 The switch exists, it is real, and it is in the shipped VR binary [V, 95 %]

`hkpListShape::disableChild(hkpShapeKey)` / `enableChild(hkpShapeKey)`. Compiled code, Havok build #20101115:
```asm
?disableChild@hkpListShape@@QAEXI@Z:
  push esi ; mov esi,ecx ; mov ecx,[esp+8]      ; ecx = index
  mov edx,ecx ; and ecx,1Fh                     ; bit  = index & 31
  mov eax,1 ; shl eax,cl ; shr edx,5            ; word = index >> 5
  push edi
  mov edi,[esi+edx*4+50h]                       ; load m_enabledChildren[word]
  not eax ; and eax,edi
  cmp edi,eax
  je  <skip>                                    ; <-- IDEMPOTENCE GUARD
  mov [esi+edx*4+50h],eax                       ; store
  inc word ptr [esi+26h]                        ; ++m_numDisabledChildren
  pop edi ; pop esi ; ret 4
```
Exactly the bitmask plus the counter. **No calls. No allocation. No AABB touch. No `recalcAabbExtents`. Idempotent** (the `cmp/je`). Same for `enableChild` with `or` and a decrement.

**Present in SkyrimVR at `0xA9C3F0` / `0xA9C420`** — PLANCK ships those RelocAddrs (`planck_src/src/RE/offsets.cpp:286-287`), from the same file that carries `driveToPoseHookLoc(0xB266AB)` [V].

**Independent proof they cannot allocate in the shipped VR binary, despite the encrypted `.text`:** a scan of all 118,450 `RUNTIME_FUNCTION` records in `.pdata` (plaintext) finds **neither address has an unwind entry**, and both sit in the gap `[0xA9C3DC, 0xA9C450)` between entries. Both are 16-byte aligned and exactly **0x30 = 48 bytes** each, matching the lib byte-for-byte in size and ordering. On the Windows x64 ABI, any function that makes a call must reserve 32 bytes of shadow space, hence adjust RSP, hence carry unwind data. **No unwind data ⟹ cannot call ⟹ cannot allocate.** [V — this is the single most elegant piece of evidence in the whole investigation.]

### 4.3 The agent consults the bitmask **live, every frame** [V, 95 %]

Not at agent-creation time. At query time. Verified independently in **five** places:

| Consumer | Evidence |
|---|---|
| `hkListAgent3::process` | `cmp eax,100h` / `mov ecx,[ebx+ecx*4+50h]` / `and ecx,edx` / `je next` — key never emitted, per frame |
| `hkListAgent3::process_midphase` | identical loop |
| `hkpListAgent::processCollision` (Agent2 fallback) | identical loop |
| `hkpListShape::castRayImpl` **and** `castRayWithCollectorImpl` | both inline the `100h` bound and the `[esi+ecx*4+50h]` read |
| `hkpEntityAabbUtil::entityBatchRecalcAabb` (the per-frame child-AABB pass) | walks the container via vtable `getFirstKey` (+0x08) / `getNextKey` (+0x0C) → `hkpListShape::getNextKey` calls `isChildEnabled` → **disabled children leave the AABB pass and therefore the sweep** |

**Whichever agent Skyrim registers — and regardless of the §2.1 dispute — a disabled child's key is never produced.** It gets no narrowphase agent, no contact manifold, no persistent memory, and no raycast test. It costs ~4 instructions in a key loop and nothing else. **The toggle takes effect on the very next physics step.**

Bounds: `isChildEnabled(i)` returns `true` **unconditionally for i ≥ 256** [V] — children above index 255 can never be disabled. Our max list is 21. `MAX_CHILDREN_FOR_SPU_MIDPHASE = 252` is PS3-only. `hkpListShape` ctor stores `0xFFFFFFFF` into all 8 words ⇒ **all children enabled on load** [V].

Children are **never renumbered** — a disabled child keeps its index, and `hkpShapeKey == child index` for a list shape (`getNextKey` literally returns the loop counter) [V]. So per-region grab identification survives disabling. SkyrimVR exposes shape keys at contact time (`planck_src/src/main.cpp:1471-1472` does `evnt.getShapeKeys(...)` → `bhkShape_GetMaterialId(shape, *shapeKey)`), and every ragdoll body already ships `m_numShapeKeysInContactPointProperties = 3` in the NIF, stock **and** baked [V, pynifly]. **Task #11 (per-capsule grab ID) works with no NIF change.**

Havok's own runtime sample does exactly this: `Demo/Demos/Physics/Test/Feature/Rigidbody/BreakOffPartsAndSpu/BreakOffPartsAndSpuDemo.cpp:419-446` calls `list->disableChild(key)` on a **live, in-world body mid-simulation** [V]. And `hkpBreakOffPartsUtil::removeKeysFromListShape` — Havok's own sanctioned live-disable helper — is documented as *"Safely removes a piece from a listshape, **or a listshape wrapped in a moppshape**"*, and its disassembly runs `hkpRemoveTerminalsMoppModifier` and then calls `disableChild` [V].

### 4.4 What invalidation is required

The header's warning (`hkpListShape.h:81`) is:
> *"Warning: you also have to invalidate the corresponding cached AABB in the `hkpCollidable::m_boundingVolumeData::m_childShapeAabbs`."*

**[D]** — `perf:enabledchildren` argues this targets only MOPP-wrapped lists and phantoms (because `hkpBreakOffPartsUtil` exists to handle exactly that, and PPB's bodies are `bhkRigidBody → bhkListShape` with no MOPP [V, from the baked NIF: `block 28 bhkRigidBody shape->27 (bhkListShape)`]). `perf:narrowphase` disassembled the producer and consumer and showed the cache **is** allocated for every list-shaped entity on any motion type, and **is** read by `hkCollectionBvTreeAgent3_extractCachedAabbsOrRecalculate`. **Adjudication: `perf:narrowphase` wins (§2.2). The cache is live.**

But `extractCachedAabbsOrRecalculate` also short-circuits to a full recalc when `bvd.m_min[0] > bvd.m_max[0]` — i.e. when the cache is marked invalid [V]. And `entityBatchRecalcAabb` rebuilds it every frame for moving bodies [V]. So for an *active* ragdoll, the cache is refreshed anyway.

**Recommendation (cheap, safe, belt-and-braces):**
```
After ANY child mutation (enable/disable, or PPB's per-frame capsule float writes):
    collidable->m_boundingVolumeData.invalidate();   // m_min[0] = 1; m_max[0] = 0;   two dword writes
```
This forces a recalc on the next query. It costs two stores. It closes the one real hole: **a deactivated / sleeping Havok island may not run `entityBatchRecalcAabb`, so a live-dialed or newly-disabled child could be invisible to (or wrongly present in) collision until the body re-activates** [I, ~70 %; nobody tested a sleeping island]. `RE::hkpCollidable::boundingVolumeData` is at collidable+0x30; `numChildShapeAabbs` at +0x20, `capacityChildShapeAabbs` at +0x22, `childShapeAabbs` at +0x28 [V, CommonLibVR].

**Agent-track invalidation: NOT required.** `hkAgent1nMachine_Process` reconciles the freshly built key list against the persistent `hkpAgent1nTrack` every call. The **same** `_hkAgent1nMachine_Process` is used by `hkpBvTreeAgent3`, whose key list is a MOPP query result that changes *every frame* as bodies move. Key appearance/disappearance is the machine's normal, first-class, per-frame operation. [V that they share the function; **[I], very strong**, that the merge tears down agents for vanished keys — the merge loop was not stepped instruction-by-instruction.]

`hkpWorld::updateCollisionFilterOnEntity` → `hkListAgent3::updateFilter` → `_hkAgent1nMachine_UpdateShapeCollectionFilter` **does** destroy/recreate child agents **and allocates**. **You do not need it for enable/disable.** If you call it anyway (e.g. to be safe), do it from the pre-drive hook, under `BSWriteLocker(bhkWorld->worldLock)`, **never from inside the step** — PLANCK does exactly this on live ragdoll bones at `planck_src/src/main.cpp:290-300`, so the precedent is proven [V].

### 4.5 The four things that will bite you

1. **`m_numDisabledChildren` must exactly equal the count of cleared bits.** `getNumChildShapes() = childInfo.getSize() − m_numDisabledChildren`, and that sizes the child-AABB cache. **Replicate Havok's `cmp/je` idempotence guard** — a naive `mask &= ~bit; ++count;` double-counts on a repeated call and corrupts `getNumChildShapes()`, producing an out-of-bounds read in the midphase.
2. **Re-enabling is more dangerous than disabling.** `BoundingVolumeData::allocate(n)` sized `m_capacityChildShapeAabbs` from `getNumChildShapes()` **at world-add time**. Never enable more children than existed when the body entered the world. PPB never *adds* children, so disable-then-re-enable within the baked count is safe by construction — but **assert `capacityChildShapeAabbs >= childInfo.size()` before ever setting a bit back.**
3. **Disabling also removes the child from raycasts** [V]. For orifice inner capsules that is probably what you want. But it means a disabled child is **not HIGGS-grabbable** and won't register on crosshair picks. The bitmask is all-or-nothing: there is no "collides but not grabbable."
4. **`RepatchListAabb` currently unions *all* children.** Decide deliberately. Leaving it is *conservative and matches Havok's own `recalcAabbExtents`* (which also does not skip disabled children [V]). Skipping disabled children gives a tighter broadphase AABB — **the only way a disabled orifice capsule stops inflating the body's proxy** — and is pure float writes. Recommendation: **skip disabled children**, but never let the AABB become tighter than the union of the *enabled* ones.
5. **Shape sharing.** `disableChild` mutates the `hkpListShape` object. If Skyrim ever refcount-shares a ragdoll shape across actors, a disable leaks to every sharer. This is **exactly** the exposure PPB's existing per-actor capsule float writes already have — no new risk class, but do not assume per-actor without the same evidence you already rely on.

**Do not hardcode `0xA9C3F0`.** Reimplement the ten instructions inline. It is version-proof, and it is a `u32` read-modify-write plus a `u16` increment into the *same* `hkpListShape` PPB already mutates — **strictly less invasive than the `hkVector4 vertexA/vertexB` writes it does today.** This sits squarely inside Pitfall-Ledger rule 1 (int/float edits to existing shape fields are safe live) and nowhere near rule 2 (no type change, no allocation).

### 4.6 Per-child `collisionFilterInfo` — real field, real setter, ONE UNVERIFIED LINK

```asm
?getCollisionFilterInfo@hkpListShape@@UBEII@Z:  ; vtable slot 04 of hkpShapeContainer
  mov ecx,[ecx+8] ; shl eax,4 ; mov eax,[eax+ecx+4] ; ret 4      ; pure load
?setCollisionFilterInfo@hkpListShape@@QAEXII@Z:
  mov ecx,[ecx+18h] ; shl eax,4 ; mov [eax+ecx+4],edx ; ret 8    ; pure store, no alloc, no invalidation
```
[V] On x64: `childInfo.data + key*0x20 + 0x08`. CommonLibVR exposes it as `list->childInfo[i].collisionFilterInfo`. **No relocation needed.**

**Bake side is wired but PPB is not using it** [V]. `bhkListShapeCinfo { hkArray<const hkpShape*> shapes; hkArray<hkUint32> filterInfos; }` exists, and `hkpListShape::setShapes`' disassembly shows the null path: `filterInfoArray == NULL ⇒ m_collisionFilterInfo = 0`. A raw binary parse of the baked `skeleton_female.nif` (NIF 20.2.0.7, userVer 12, bsVer 100, 1008 blocks, `consumed == blockSize` on every block) shows **all 11 `bhkListShape` blocks have `numUnknownInts = 0`** ⇒ **every child's filter info is currently 0.** [V]

Semantics (from PLANCK's decompiled `bhkCollisionFilter_CompareFilterInfosEx`, `planck_src/src/RE/havok.cpp:370-425`) [V]:
- bits 0–6 = collision layer; **bit 14 (`0x4000`) = NO-COLLISION**, tested first
- bit 15 + part number (bits 8–12) → the ragdoll adjacency rule
- bits 16–31 = system group; **`group == 0` on either side ⇒ "collide with everything"**

That last clause is why all-zero children are harmless today: the shape-collection filter runs **after** the collidable-collidable filter has already approved the pair, so it can only further *restrict*; a zero child filter returns "allow." (Cross-check: `Animated Armoury/…/AkaviriClaw.nif` has `numUnknownInts=2`, both zero — all-zero children are the norm.)

> **⚠ THE ONE UNVERIFIED LINK, agreed by all three agents that looked at it [U]:**
> **Does Skyrim's `bhkCollisionFilter` actually implement the *shape-collection* overloads of `isCollisionEnabled(input, a, b, bContainer, bKey)` in terms of the child's filter info — or does it just `return true`?**
>
> Evidence *for*: stock `hkpGroupFilter` documents exactly that behaviour; `hkpShapeCollectionFilter.h:22-24` says *"for each sub-shape of the shape collection that needs to be tested against the shape the filter is called"*; `hkpCpuSingleContainerIterator::isCollisionEnabled` calls `input->m_filter->isCollisionEnabled(*input, *a, *b, *m_container, m_lastShapeKey)`; and `bhkListShapeCinfo::filterInfos` would be pointless otherwise. CommonLibVR's `bhkCollisionFilter` only declares the destructor. **Nobody could read the VR binary's override** (encrypted `.text`).
>
> **Decisive 10-minute in-game test:** write `0x4000` into ONE child's `collisionFilterInfo` at runtime and poke it. If it stops colliding, we get a *second*, independent, allocation-free per-child kill switch **plus per-child collision layers** (which would give the orifice ring its own layer, and could give COM's children part numbers that make them "adjacent" to Spine2 — see §5.4). If it still collides, only the `enabledChildren` bitmask works — which is fine, because that one is verified.

### 4.7 The NIF / pynifly side [V]

- `pynifly`'s `bhkListShapeProps` exposes `childShape_{data,size,flags}` and `childFilter_{data,size,flags}` — those are the two `hkWorldObjCinfoProperty` structs, **not** per-child data and **not** a bitmask.
- **`pynifly` does not expose the per-child `Unknown Ints` array at all.** Neither does the underlying `NiflyDLL`. To author per-child filter info you need NifSkope or a direct binary patch — and the block grows by `4*n` bytes, so the header's `blockSize[i]` must be updated too.
- **There is no "enabled bitmask" in the NIF.** `m_enabledChildren` is runtime-only; the ctor sets all-ones.
- Minor divergence worth a glance: the reference mod writes `childShapeProperty = childFilterProperty = (0, 0, 0x80000000)` (the hkArray "don't deallocate" flag) where PPB writes `(0,0,0)`. Both have `size = 0` so this is almost certainly benign.

Baked-NIF census [V, raw parse]:

| block | node | subShapes | numUnknownInts |
|---|---|---|---|
| 27 / 111 | R/L Thigh | 6 | 0 |
| 37 / 121 | R/L Calf | 4 | 0 |
| 47 / 131 | R/L Foot | 3 | 0 |
| 603 / 682 | R/L UpperArm | 2 | 0 |
| 611 / 690 | R/L Forearm | 2 | 0 |
| 620 | R Hand | 3 | 0 |

⚠ **That is the *wave-1* bake — 11 list bodies, and no torso lists at all in this dump.** Report 09:112 says wave-1 took the census "21→45". The brief says the baseline is 18×1. **These three numbers cannot all be true. §5 step 5 emits ground truth at runtime. Do not plan against any of them.**

### 4.8 VERDICT ON THE FEATURE

> **YES. `hkpListShape::disableChild(i)` is a live, allocation-free, leaf-function bit flip. Both the naive and the sweep-culled agent rebuild their child key list from `m_enabledChildren` on every `process()` call, and `castRayImpl` and the per-frame child-AABB pass honour it too. Clearing a bit stops that capsule colliding *and* raycasting on the very next physics step, with zero agent invalidation and zero allocation.**
>
> Required extras: replicate the idempotence guard; keep `m_numDisabledChildren` exact; invalidate `hkpCollidable::m_boundingVolumeData` (two dword writes); optionally teach `RepatchListAabb` to skip disabled children.
>
> **Confidence 90 %.** The mechanism is verified end-to-end from headers, disassembly, PLANCK's shipped VR relocations, and Havok's own live demo. **But it has never been spike-tested on a live Skyrim ragdoll. Do not ship it on the strength of this report. §5 step 8 is a 30-minute spike; run it, on one body, one actor, statue mode, with a save beforehand.**

---

## 5. WORST CASE IN THIS STACK

### 5.1 PLANCK's non-adjacent self-collision window [V — this is the single biggest de-risking fact]

`planck_src/src/main.cpp:4356-4376`, per frame per active actor:
```cpp
if (Config::options.doBipedSelfCollision && collisionGroup != 0) {
    if (race has ActorTypeNPC keyword || race in additionalSelfCollisionRaces) {
        if (g_physicsListener.IsCollided(actor) || isHeld) {
            g_selfCollidableBipedGroups.insert(collisionGroup);  UpdateCollisionFilterOnAllBones(actor);
        } else { erase + UpdateCollisionFilterOnAllBones(actor); }
    }
}
```
And `IsCollided` reads `collidedRefs[0]|[1]`, which is populated **only** from collisions where one body `IsHiggsRigidBody(...)` (`main.cpp:2304-2330`: `if (!isAhiggs && !isBhiggs) continue;`).

> **Non-adjacent intra-actor self-collision is ON exactly while the player's HIGGS hand / held weapon / held object is touching that NPC, or while the NPC is held. It is capped at 2 actors — one per hand. It does not scale with the crowd.**

Deployed `activeragdoll.ini`: `doBipedSelfCollision = 1` (505), `doBipedSelfCollisionForNPCs = 1` (508), plus 24 draugr/skeleton/troll races.

**Transition cost:** each toggle calls `UpdateCollisionFilterOnAllBones` → `hkpWorld_UpdateCollisionFilterOnEntity(..., FULL_CHECK, IGNORE_SHAPE_COLLECTIONS)` on all 18 bodies under a `BSWriteLocker`. That is a **one-frame agent create/destroy spike at touch-start and touch-end**, not a steady cost — but with 96 children it allocates far more `hkpAgent1nSector`s (512 B each) than before. **Expect a hitch on the first frame the player's hand lands on an NPC.** [I, high]

### 5.2 "Adjacent" is a 32×32 bit matrix, not the constraint graph [V — and it hands us a free win]

PLANCK re-implements the vanilla comparator (`planck_src/src/RE/havok.cpp:372-404`). For same-group biped bodies it ends at:
```cpp
long bipedBitfield = _this->bipedBitfields[(filterInfoA >> 8) & 0x1F];
return _bittest(&bipedBitfield, (filterInfoB >> 8) & 0x1F);
```
Adjacency is `bhkCollisionFilter::bipedBitfields[32]` (at `+0x50`), indexed by the 5-bit **biped part number** (`filterInfo` bits 8–12). **Not** derived at runtime.

Part numbers, from a pynifly dump of both the PPB bake and the untouched XP32 original (identical ⇒ stock) [V]:

| part | body | | part | body |
|---|---|---|---|---|
| 0 | Neck | | 8 / 14 | L/R Thigh |
| 1 | Head | | 9 / 15 | L/R Calf |
| **2** | **COM *and* Spine0** | | 10 / 16 | L/R Foot |
| 3 | Spine1 | | 5 / 11 | L/R UpperArm |
| 4 | Spine2 | | 6 / 12 | L/R Forearm |
| | | | 7 / 13 | L/R Hand |

(Cross-check: PLANCK's `IsRagdollHandFilter` hardcodes `part == 13 || part == 7`. Exact match.)

★ **COM and Spine0 share part number 2.** The filter physically cannot tell them apart. Since Spine0↔Spine1 must be disabled (constrained), **COM↔Spine1 is also disabled — for free.** That kills a 231-child-pair body pair at zero cost. Likewise COM↔Thigh disabled ⇒ Spine0↔Thigh disabled.

**[I], the one soft premise in §5:** that `bipedBitfields` disables exactly the constraint-tree neighbours and nothing else. **The vanilla bit values have never been read** — the game was closed. If vanilla also disables, e.g., hand↔upperarm, every count below shrinks.
**Probe: 5 read-only lines from the existing per-frame hook — `RE::bhkCollisionFilter::GetSingleton()`, log `bipedBitfields[0..23]` once.** This is the highest-value unknown in the entire analysis and it costs nothing.

### 5.3 Exact intra-actor pair count for our layout

Constraint tree (17 edges, `PivFix.cpp:253-261`): Hand→Forearm→UpperArm→Spine2; Spine0→COM; Spine1→Spine0; Spine2→Spine1; Neck→Spine2; Head→Neck; Thigh→COM; Calf→Thigh; Foot→Calf.

Mapping through part numbers: 17 disabled part-pairs ⇒ **20 disabled body pairs** (the 17 constrained ones plus COM×Spine1, Spine0×LThigh, Spine0×RThigh, all via the part-2 alias).

> **Allowed (non-adjacent) intra-actor body pairs = C(18,2) − 20 = 133.**
> **Σ nᵢ·nⱼ over those 133 pairs = 2,891 child pairs** (was 133 with single capsules — **21.7×**).

That 2,891 is an **absolute ceiling**, reached only if all 133 body AABBs overlapped at once. Worst individual pairs:

| body pair | child pairs | max contact pts (×3) | (×4) |
|---|---|---|---|
| **COM × Spine2** | **231** | 693 | 924 |
| **Spine0 × Spine2** | **121** | 363 | 484 |
| COM × Calf (L/R) | 84 | 252 | 336 |
| Spine1 / Spine2 × Thigh | 66 | 198 | 264 |
| COM × Hand, COM × Foot | 63 | 189 | **252** |
| LThigh × RThigh | 36 | 108 | 144 |
| **cross-actor COM × COM** | **441** | 1,323 | 1,764 |

**Real per-frame work is far smaller.** A geometric test — transform each child capsule by its body's world quaternion+translation from the NIF, build world AABBs, run the same sweep — found that in **bind pose only 6 of the 133 allowed body pairs have overlapping body AABBs, and only 4 child-AABB pairs survive.** Bind pose is a T/A-pose, so in a standing idle expect roughly **12–20 overlapping body pairs and 30–80 surviving child pairs per actor** [I]. With single capsules the same pose gives ~10–15.

> **During the touch window you pay ~30–80 capsule-capsule agents on ONE actor, not 2,891.** (Under the pessimistic linear model of §2.1, ~400–600. Still not fatal.)

### 5.4 ★★ THE CLIFF: 256 contact points per body pair, no runtime clamp

`hkpProcessCollisionData.h:20` [V]:
```cpp
enum { HK_MAX_CONTACT_POINT = 256 };
hkpProcessCdPoint m_contactPoints[HK_MAX_CONTACT_POINT];
```
`hkpProcessCollisionOutput.h:41-47` [V]:
```cpp
inline void commitContactPoints(int n) {
    m_firstFreeContactPoint += n;
    HK_ASSERT2(0xf0100101, m_firstFreeContactPoint < &m_contactPoints[HK_MAX_CONTACT_POINT],
               "ContactPoint Overflow in hkpProcessCollisionOutput");
}
```
`reset()` rewinds `m_firstFreeContactPoint` per agent entry ⇒ the buffer is **per collidable pair** (`hkpSimulation::processAgentCollideDiscrete(hkpAgentNnEntry*, input, output)` — one output per entry) [V structure; [I], high, that nested list agents accumulate into the same 256-slot buffer]. **In a retail build `HK_ASSERT2` compiles out.** Overflow walks past the array into `m_representativeContacts` / `m_potentialContacts` / `m_toi`.

Counter-evidence, from the official User Guide [V]: *"Havok has a limit of 250 contact points which may be created between two rigid bodies. If this limit is reached a warning is generated, and any more generated contact points will be automatically rejected."* Those are probably two stages (agent staging buffer at 256 vs. contact-manager at 250) — and the staging buffer fills first. **[U] Unresolved. See §2.4.**

**Structural safety bound (assuming 4 contact points per child pair):**

> **For any collision-enabled body pair, `nᵢ · nⱼ ≤ 63` makes overflow *impossible*. Above that, overflow is *possible*.**

Our layout violates it on:
- **COM (21) × Spine2 (11) = 231** — 3.7× over. Non-adjacent (parts 2 vs 4). Fires only during the touch window, on one actor.
- **Spine0 (11) × Spine2 (11) = 121** — 1.9× over. Abdomen vs lower ribs, two hops apart, anatomically the pair **most likely to actually co-contact.**
- COM × Calf = 84 — reachable in a crouch/kneel.
- COM × Hand, COM × Foot = 63 — exactly at the line.
- **cross-actor COM × COM = 441** — 7× over, **and it has no `bipedBitfields` protection at all** (different collision groups ⇒ the biped bitfield test never runs).

Overflow requires those child pairs to be within Havok's **collision tolerance** simultaneously, not merely AABB-overlapping. Anatomy separates pelvis from chest, so it is *unlikely* in a good fit. **[U] Skyrim's `hkpWorldCinfo::m_collisionTolerance` / `hkpCollisionQualityInfo::m_createContactThreshold` were never read.** Havok's default tolerance is 0.1 m ≈ 7 game units — which would be alarming. **Read it at runtime before trusting any "they'll never touch" argument.**

> **"Unlikely" is not a budget. Probability unknown, consequence unbounded, cost of measuring it is one contact listener. Do not bake COM at 21 without the counter.**

### 5.5 Two more cliffs, both real

**Cliff 2 — contact-atom reallocation inside the collide step.** `hkpSimpleConstraintContactMgr::reserveContactPointsImpl(int)` grows the contact atom via `hkpConstraintAtomUtil::allocateAtom(n, …)`, sized `sizeof(atom) + n*(sizeof(hkContactPoint) + sizeof(hkpContactPointProperties) + …)` [V]. A body pair jumping from 2 → 40 contacts triggers a **heap allocation inside `hkpWorld::stepDeltaTime`**, on the frame the player first presses a hand into the torso. Exactly the frame you least want a hitch. [V mechanism.]

**Cliff 3 — HIGGS's 70 Hz substep cliff.** `higgs/include/config.h:176` — `minPhysicsFrameRate = 70` ⇒ `maxPhysicsFrameTime = 14.29 ms`. At 90 Hz (11.1 ms) that is **exactly 1 physics step per frame**. Cross 14.29 ms and the step **splits in two and physics cost roughly doubles** — which pushes you further below 70 [V, `higgs/src/hooks.cpp:607-622`]. Once you cross this, the mod is not slow; it is **unstable**. Instrument the steps/frame histogram.

**Cliff 4 — PLANCK's world-wide contact listener.** `hkpWorld_addContactListener` (`main.cpp:3968`), so **every new contact point anywhere in the Havok world** runs `PhysicsListener::contactPointCallback` — `GetRefFromCollidable`, an RTTI `DYNAMIC_CAST`, `std::map` / `unordered_set` lookups. Order **0.5–2 µs per contact point** [I]. Mitigating fact [V, from the NIF]: every ragdoll body ships `processContactCallbackDelay = 65535`, so the callback fires only on **new** contact points. **But if your capsules buzz** — contacts added and removed each frame at a marginal overlap — **this cost goes from once to every frame.** Co-located capsules at the same station are exactly the geometry that buzzes.

**Caps you are safely under** [V]: `MAX_CHILDREN_FOR_SPU_MIDPHASE = 252` (PS3-only); `MAX_DISABLED_CHILDREN = 256` (indices ≥ 256 can never be disabled — our max is 21); `HK_MAX_NUM_HITS_PER_AABB_QUERY = 2048 = HK_MAX_AGENTS_IN_1N_MACHINE` (our ceiling is 441); `hkpCollidable::BoundingVolumeData::m_numChildShapeAabbs` is `hkUint16`.

### 5.6 Crowds

- **Active-ragdoll count is uncapped** [V]: `shouldBeActive = |actor.pos − player.pos| * havokWorldScale < activeRagdollStartDistance` (ini = **50 m**). The only throttle is `minFramesBetweenActorAdds = 3` — an add *rate*, not a steady-state ceiling. Realistically 10–30 in a modded city, 40–80 in a large battle. [I on the counts.]
- **Self-collision does NOT scale with the crowd** (§5.1). ≤ 2 actors ever. This is the single biggest de-risk.
- **But cross-actor ragdoll-vs-ragdoll DOES, and it is on by default** [V]: `doBipedNonSelfCollision = 1` / `enableBipedBipedCollision = 1` (`activeragdoll.ini:512/516`) → `layerBitfields[Biped] |= 1<<Biped`. Two NPCs' 18 bodies collide with each other's 18. **Different collision group ⇒ no `bipedBitfields` filtering at all — all 324 body pairs are filter-enabled**, subject only to broadphase. Two NPCs brushing past each other hit this every frame, not just during a touch.
- `stopRagdollNonSelfCollisionForCloseActors` **does not save you**: it fires in `contactPointCallback` and only sets `CONTACT_IS_DISABLED` — midphase, narrowphase and manifold work are already paid. And `closeActorMinDistance = 2.0` is in **game units** (no `havokWorldScale` multiply, unlike line 4295) ≈ **2.9 cm**. It catches actors clipped into each other, not crowds. [V]
- **The always-on tax** (§2.2): every active list-shaped body pays an O(N log N) child-AABB rebuild+sort every frame, touched or not. ≈1.4 µs/actor at 96 children ⇒ 25 actors ≈ **35 µs ≈ 0.3 % of frame**. Bare capsules pay **zero** — this cost is entirely new.
- **Two un-gated consumers now iterate 96 children instead of 18** [I]: the player's `hkpCharacterProxy` manifold (`getClosestPoints` / `linearCast` every frame the player stands near an NPC), and HIGGS's grab-selection `hkpWorld_LinearCast` (every frame the hand is near an NPC).
- **FSMP** (`hdtsmp64.dll`) is a separate Bullet world with its own threads — contends for *cores*, never for the Havok step. Almost certainly your biggest physics consumer overall, and **orthogonal to this change.** **CBPC** is analytic sphere/capsule math in NiNode space, zero Havok symbols — untouched. **Precision (Ersh) is not installed.** PLANCK's PD-drive is per-*body* (18) and per-*constraint* (17), shape-agnostic — unchanged.

---

## 6. THE MEASUREMENT PROTOCOL

**Nothing above is a measurement.** This section produces the numbers. Follow it in order.

### 6.0 Where the instrument goes

| Seam | What it measures | Verdict |
|---|---|---|
| `0xB266AB` pre-drive chain (PPB already owns it) | **`ppbMs`** — bracket `ArmIK::ApplyToPoseTrack`. **`driveMs`** — bracket `s_chainedDriveToPose` (times collviz + AIHands + PLANCK's drive hooks + engine `driveToPose`). | ✅ Useful as confounders. ❌ **Does NOT contain the Havok step.** Capsule count will barely move `driveMs`. |
| `0xB268DC` post-physics (PPB already owns it) | frame-phase marker | ⚠ **[U]** whether the Havok step falls between it and `0xB266AB`. They are 0x231 bytes apart — probably the same routine. **Prove it before trusting it.** |
| **`0xDFB722`** — 5-byte `CALL` to `hkpWorld::stepDeltaTime(ahkpWorld*, float)` in `bhkWorld::Update` | **`stepMs`** — the actual physics step | ✅ **THE instrument.** HIGGS `Write5Call`s it; PLANCK does not touch it. A `kDataLoaded` `write_call<5>` from PPB lands cleanly on top. |
| `higgs->AddPostVrikPostHiggsCallback` (vtable idx 33, already declared in `PPB-plugin/src/HiggsInterface.h:66`) | authoritative once-per-game-frame tick + CSV flush | ✅ Free, no new hook. |
| **World `hkpContactListener`** (`hkpWorld_addContactListener` @ VR `0xAB5580`, PLANCK's exact pattern) | `contactPoints/step`, `collisionsAdded`, **`maxPointsInOnePair`** | ✅ **The alarm. This is the ship gate.** Overhead ~45 k virtual calls/s ≈ <0.05 ms, present in both A and B ⇒ cancels. |
| In-process **OpenVR** `Compositor_FrameTiming` (`openvr` is already in PPB's `vcpkg.json`; the game already called `VR_Init`, so `vr::VRCompositor()` resolves in-process — proven pattern at `tools/VRBackpack-plugin/src/main.cpp:8725-8990`) | `m_flClientFrameIntervalMs`, `m_flPreSubmitGpuMs`, `m_nNumFramePresents`, `m_nNumDroppedFrames`, `m_nReprojectionFlags` | ✅ **Biggest rigor upgrade for ~40 lines.** Lands on the same CSV row as `stepMs`. No cross-tool clock alignment. |

The chain hook:
```cpp
using StepFn = std::int32_t (*)(void* ahkpWorld, float dt);   // hkpStepResult = int
static StepFn s_chainedStep = nullptr;

static std::int32_t StepChainHook(void* world, float dt) {
    const auto t0 = QPC();
    const auto r  = s_chainedStep ? s_chainedStep(world, dt) : 0;
    Perf::OnPhysicsStep(world, dt, QpcToMs(QPC() - t0));
    return r;                                   // MUST forward hkpStepResult
}
// Install at kDataLoaded. Guard: if (*(uint8_t*)site != 0xE8) { log; bail; }   // the existing self-abort pattern
// Log ResolveModuleName(prev) so PPB.log proves we landed on top of HIGGS.
```
Two impurities, both **constant across the A/B and therefore cancelling in the delta**: HIGGS's own pre/post callbacks and PLANCK's `g_prePhysicsStepJobs` are inside the window.

**⚠ Do NOT use `AddCollisionFilterComparisonCallback` as a broadphase-pair counter.** HIGGS returns on the first callback that answers Collide/Ignore (`higgs/src/pluginapi.cpp:208-218`) and PLANCK registers early — your counter sees only the pairs PLANCK let fall through. **Silently biased.**

**Console command:** `perf`. Donor `ToggleWaterSystem` (fallback `ShowQuestStages`). **Zero parameters**, `referenceFunction = false` + `SetParameters()` — the `statue`/`trf` recipe. 1st press: reset ring, emit runtime census, open CSV, sample. 2nd press: stop, sort a copy of the ring for **exact** percentiles (no streaming estimators), print + log + close. Ring 16,384 frames ≈ 182 s @ 90 Hz ≈ 640 KB. QPC ≈ 25 ns/call; ~6 calls/frame ⇒ instrument overhead < 0.01 % of a 1 ms step.

**CSV schema — one row per frame; this is the artifact, everything else is derived offline:**
```
frameIdx,wallMs,gameDeltaMs,physSteps,physStepSumMs,physStep0Ms,physStep1Ms,physDt0Ms,
driveSumMs,driveCalls,ppbSumMs,
drivenActors,ragdolledActors,higgsHoldL,higgsHoldR,grabbedActors,
contactPoints,collisionsAdded,maxPointsInOnePair,
compClientIntervalMs,compPreSubmitGpuMs,compFramePresents,compDroppedFrames,compReprojFlags
```
`drivenActors` / `ragdolledActors` / `higgsHold*` make each run **self-describing** — you recover the phase boundaries offline without a manual marker.

### 6.1 A/B method, ranked by rigor

| Rank | Method | Rigor | Cost | Verdict |
|---|---|---|---|---|
| **1** | **Two skeleton NIFs, swapped between runs** | **10/10 — GROUND TRUTH** | one MO2 profile + a restart per config | **Do this unconditionally. Everything else is an accelerator on top, not a replacement.** Confounds: restart perturbs allocator, shader cache, weather, CPU boost. Mitigate with interior cell, fixed save, `coc` from the same save, 5 s warm-up discard, and **A/B/A/B across four restarts.** If `stepMs(A₁)` vs `stepMs(A₂)` differ by >10 % of the A→B delta, the delta isn't real. |
| **2** | **Live child-disable via `m_enabledChildren`** (§4) | **8/10** | ~2 h build + 30 min spike | **Best rigor-per-cost — IF the §6.2 step-8 spike passes.** Gives *within-run paired sampling*: flip A→B every 15 s in one session; drift, thermals, weather, allocator state all cancel. Also gives you the child-LOD in §7 for free. |
| **3** | **Per-child `collisionFilterInfo` = `0x4000`** | **6/10** | 10 min to disprove | Fallback if (2) proves unsafe. Prunes narrowphase but **not** child enumeration, the AABB test, or the filter call ⇒ measures a **strict subset** of what the NIF swap removes. Load-bearing unknown (§4.6). |
| **4** | **Shrink children to r ≈ 0.001** | **NOT AN A/B** | — | Children are still enumerated, AABB-tested, and dispatched. Suppresses contacts + solver rows while leaving all midphase cost in place. **It would report "capsules are almost free" and be wrong in the reassuring direction — the worst kind of wrong.** Also `r → 0` is a degenerate Havok capsule. **Keep it for exactly one purpose:** as a *decomposition diagnostic* (full-children-at-half-radius vs full-children-normal-radius splits Δstep into `midphase+dispatch` vs `contacts+solver`), and only after (1) has shown a delta worth decomposing. |

### 6.2 The executable checklist

**Phase I — build the instrument (no game required)**

1. **`Perf.cpp` / `Perf.h`.** QPC helpers. Ring buffer of 16,384 frame records. Atomic per-frame accumulators. Exact percentiles (sort a copy on stop).
2. **`Hooks::InstallPhysicsStepHook()`** at `kDataLoaded`, after the existing three. Site `base + 0xDFB722`. **Reuse the `p[0] != 0xE8` self-abort guard** (`Hooks.cpp:236-240, 255-259`). Signature `std::int32_t(*)(void*, float)`; **forward the `hkpStepResult` return value.** Log `ResolveModuleName(prev)` + a `ChainVerdict()`-style line.
3. **Bracket the two seams you already own** in `Hooks::PreDriveChainHook` (`Hooks.cpp:164-174`): `ppbMs` around `ArmIK::ApplyToPoseTrack`, `driveMs` around `s_chainedDriveToPose`.
4. **Frame tick** = `higgs->AddPostVrikPostHiggsCallback(&Perf::OnFrame)`. On each tick read `vr::VRCompositor()->GetFrameTiming(&t, 0)` (guard `nullptr`, retry on a later frame), snapshot accumulators, push a CSV row, reset.
5. **Runtime census** on `perf` arm, for the console-selected actor (reuse `StatueTargetActor`): walk every ragdoll body; log node name, `shape->type`, and for `kList` the `childInfo.size()`, `GetNumChildShapes()`, `numDisabledChildren`, and each child's radius + segment length. **This settles the 18-vs-45-vs-96 census with data instead of a report.** Assert `enabledChildren[]` is all-ones on a fresh load.
6. **`perf` console command** (§6.0).
7. **World contact listener** — `contactPoints/step`, `collisionsAdded`, bucketed by `(bodyA, bodyB)` → **`maxPointsInOnePair`**. **This is not optional. It is the ship gate.**
8. **The 30-minute `disableChild` spike.** Build `capdis <slot> <childMask>`. Disable R-hand children 1..2 on one NPC, statue mode, save first. Verify **all three**: (i) `probe` shows `GetNumChildShapes()` dropped; (ii) `trd` collviz tubes stop registering contacts; (iii) the HIGGS hand physically passes through where child 1 used to be. Apply every guard in §4.5. **If any of the three fails, method (2) is dead — fall through to (1)-only, and run the `0x4000` disproof (§4.6) as a 10-minute consolation.**
   *Also, while you are in there:* log `RE::bhkCollisionFilter::GetSingleton()->bipedBitfields[0..23]` once. Five read-only lines. **Highest-value unknown in the report.** And read `hkpWorldCinfo::m_collisionTolerance` while you're at it.

**Phase II — validate the instrument (~20 min in-game)**

9. **Phase-order trace.** Log 3 frames of ordered `{QPC, threadId, event}` for `prePhysicsStep`, every `driveToPose`, every `postPhysics`, and the frame tick. **Settles:** steps/frame (expect **1**); whether `postPhysics` is really after the step (currently **[U]** — §2.3); whether the step shares the frame tick's thread (currently **[I]** high). **If thread IDs differ, physics may overlap other main-thread work and `stepMs` no longer adds 1:1 to frame time** — you'd need ETW. **Settle this before trusting any number.**
10. **Null test.** Run the full scenario twice with the *same* skeleton. `Δmean(stepMs)` between them is your **noise floor.** Any A/B delta smaller than ~2× the noise floor is not a result. If the null Δ exceeds 0.10 ms, tighten the scenario before proceeding.
11. **Overhead test.** `perf` armed vs disarmed, compare `m_flClientFrameIntervalMs`. Must be < 0.05 ms.

**Phase III — the scenario (~30 min of sampling)**

**Cell:** `coc QASmoke` — interior, no weather, no wandering AI, negligible GPU, clutter asleep and identical across every run. Stand on a fixed spot, face a wall to pin GPU load. **Fixed save:** on the mark, `tgm` on, `tcai` on, `set gamehour to 12`. Hard-save. **That save is the origin of every run.** **Spawn base:** resolve once (`help "bandit" 4 NPC_`), reuse forever; the race must carry `ActorTypeNPC` (PLANCK's self-collision gate).

| Phase | Player does | What it exercises |
|---|---|---|
| **P1 IDLE** | Stand still, hands at sides | PD-driven bodies, motors on, no contacts. The **always-on tax** (§2.2). |
| **P2 TOUCH** | Push both hands into one NPC's torso and hold | `IsCollided` ⇒ **non-adjacent self-collision ON.** HIGGS hand bodies vs COM(21). **The capsule-count case.** |
| **P3 PILE** | `<ref>.pushactoraway player 0` on all N, let them settle | cross-actor list-vs-list + 96 children vs the floor mesh |
| **P4 PILE + TOUCH** | Shove both hands into the pile and stir | **THE WORST CASE. Everything at once.** |

**N sweep:** N ∈ {0, 1, 2, 4, 8}. N=0 gives the world baseline α.
**Sampling:** 45 s per (config, N, phase); **discard the first 5 s** (agent creation, island activation, boost ramp) ⇒ ~3,600 usable frames.
**Matrix:** full N-sweep for **P1** and **P4** (3 configs × 5 × 2 = 30 runs); **P2** and **P3** at N=4 only (6 runs). 36 × 45 s ≈ 27 min of sampling.
**Per run:** load the fixed save → `coc QASmoke` → `player.placeatme <BASE> 1` × N → walk to the mark → `perf` → 45 s → `perf` → reload.

12. **Config C0 = stock skeleton (18 children).** Produces **S₀**, the baseline. **Do it first.**
13. **Config C1 = wave-1 (46).** The already-VR-confirmed known-good rung.
14. **Config C2 = wave-2 (96).**
15. **A/B/A/B drift check:** repeat C0's `P4, N=8` cell last. Drift >10 % of the C0→C2 delta ⇒ session contaminated, rerun.
16. If step 8 passed: **redo `P4, N=8` as a within-run paired A/B**, alternating full / child-0-only every 15 s for 4 minutes. Paired deltas kill drift entirely. **This is your highest-quality number**; the restart runs become its corroborator.

**Phase IV — decide**

17. **Fit** `stepMs(N) ≈ α + β·N + γ·N²` per config, per phase.
    - **α** = world/floor baseline, config-independent.
    - **β** = linear term — each actor's children vs static geometry + the always-on AABB tax. Scales with `Σᵢ nᵢ` (18 / 46 / 96).
    - **γ** = pairwise term — body-pair child products. Scales with `Σ_{overlapping pairs} nᵢ·nⱼ`.
    Two noisy point estimates become **three fitted coefficients with error bars**, and Δβ / Δγ tells you **which kind** of cost the capsules bought. That is the rigorous answer to "how much does each cut buy."
18. Apply the table in §6.3 to the `P4, N=8` cell.
19. **Check `maxPointsInOnePair` in EVERY cell.** > 200 anywhere ⇒ **cut COM immediately, independent of the ms verdict.** Correctness cliff, not performance.
20. **Check the steps/frame histogram in every cell.** Any `2` ⇒ Red.
21. If Amber/Red: **predict** each rung from the fitted β, γ (§7), pick the shallowest rung that lands Green, **and re-measure that rung.** Never ship on a prediction.
22. **Build the child-LOD (§7) regardless of the verdict**, if step 8 passed.
23. **Amend `04_Pitfall_Ledger.md` rule 3.** Strike *"sub-shapes are ~free (<1 % frame at realistic counts)."* Replace with the measured β, γ, the `nᵢ·nⱼ ≤ 63` contact-point rule, the raycast multiplier, and the 70 Hz substep cliff. **That assumption is exactly what this exercise was called to test. Do not let it survive unamended.** The user is right to have objected to it.

### 6.3 PASS / FAIL — defined up front, against the 11.1 ms budget

**90 Hz ⇒ 11.1 ms/frame.** Practical CPU budget is ~9–10 ms once the compositor's running-start is subtracted. Modded SkyrimVR is CPU-bound long before it is GPU-bound.

**All thresholds are Δ against S₀ (stock skeleton) in the same matrix cell.**

| Verdict | Condition (**ALL** must hold) |
|---|---|
| 🟢 **GREEN — ship as-is** | Δmean(`stepMs`) ≤ **0.20 ms** · Δp95 ≤ **0.50 ms** · Δp99 ≤ **1.0 ms** · Δmax ≤ **2.0 ms** · steps/frame still 1 · Δ(frames with `m_nNumFramePresents > 1`) ≤ **+1 pp** · **`maxPointsInOnePair` ≤ 128** · Δβ ≤ **0.05 ms/actor** |
| 🟡 **AMBER — ship a reduced-capsule default, full set as opt-in** | 0.20 < Δmean ≤ **0.50 ms** **and** Δp99 ≤ **2.0 ms** **and** `maxPointsInOnePair` ≤ **160** |
| 🔴 **RED — cut capsules now** | Δmean > **0.50 ms** · **OR** Δp99 > 2.0 ms · **OR** Δmax > **5 ms** (a single-frame hitch is a VR-sickness event, not a statistic) · **OR** steps/frame goes 1→2 attributable to us (**the 70 Hz cliff — physics cost then doubles and you are in a feedback loop**) · **OR** reprojected-frame ratio > +2 pp · **OR** **`maxPointsInOnePair` > 200** |

**Rationale, stated honestly:** 0.20 ms = 1.8 % of frame; 0.50 ms = 4.5 %. Given "SkyrimVR needs all the juice," 4.5 % of the *entire* frame budget for a physics-fidelity mod **in its worst case** is where I would draw the line. **These are policy choices, not physics.** Also read them as a fraction of S₀: if S₀ = 2 ms, then +0.5 ms is **+25 % physics.** Both framings should be in front of you when you decide.

**The two non-negotiable, non-ms failures:**
1. **`maxPointsInOnePair` > 200** — see §5.4. Two 21-child COM bodies interpenetrating is up to **441 child pairs** feeding one 256-slot buffer.
2. **steps/frame 1 → 2** — see §5.5, Cliff 3.

### 6.4 External tools — which isolates CPU physics

**None of them. Not one.** External tools split CPU-total from GPU-total. **Only the `0xDFB722` wrap isolates the Havok step.** Use externals to answer *"did the frame actually break,"* never *"how much did the capsules cost."*

1. **In-process OpenVR compositor timings (from PPB itself) — BEST.** Same CSV row as `stepMs`. ~40 lines. Proven pattern in `VRBackpack-plugin`.
2. **fpsVR** (Steam, ~$4) — per-frame CPU/GPU/reprojection **CSV export**. Reads the same `Compositor_FrameTiming`. Use as an **independent corroborator** of #1; if they disagree, #1 is wrong.
3. **SteamVR → Settings → Performance → Display Frame Timing** (or `vrmonitor` → Developer → Advanced Frame Timing). Live stacked graph. **No CSV.** Excellent for a real-time read while you're in the headset.
4. **Intel PresentMon** — VR presents route through the SteamVR compositor; attribution gets muddy. Cross-check only.
5. **ETW / WPA, Superluminal, VTune** — the only tools giving true per-thread CPU time and `hkpWorld::stepDeltaTime` by name. Reach for this **only if #1 and the internal timer disagree.**
6. **ENB counters** — unreliable in VR (per-eye rendering), usually absent. Skip.
7. **Skyrim's own** — there is nothing. `tm`, `tdt`, `sdt` report no frame timing. Papyrus profiling is irrelevant to Havok. **The game cannot measure this.**

---

## 7. IF IT'S TOO SLOW: THE CUT LIST

**Do not guess the savings.** The 3-config × N-sweep gives you `stepMs ≈ α + β·(Σᵢ nᵢ) + γ·(Σ_{overlapping pairs} nᵢ·nⱼ)`. Fit β and γ from the three measured configs (18 / 46 / 96), then **predict** every rung below and re-measure the one you pick.

**Census:** COM 21 · Spine0/1/2 11×3 = 33 · Neck 1 · Head 1 · arms (2+2+3)×2 = 14 · legs (6+4+3)×2 = 26 → **96.**
**The torso is 54/96 = 56 %. COM alone is 22 %.**

| Level | Change from L0 | Σn | Worst pair | What it buys |
|---|---|---|---|---|
| **L0** | wave-2 full | **96** | COM×COM = **441** ❌ | breaches the 256 ceiling |
| **L1** | foot 3→1, calf 4→2 | 88 | 441 | Merge the sole rods into the main capsule. Feet only matter *while ragdolled*; the char controller owns standing. **Cheapest, least-felt cut.** Pure β. |
| **L2** | + thigh 6→3 (drop rings 4–6) | 82 | 441 | Thighs are the most-contacted limb in a floor pile ⇒ good β savings. |
| **L3** | + spine0/1/2 11→5 each | 64 | COM×COM 441; **Spine0×Spine2 → 25 ✅** | −18 children from the second-most-contacted bodies. **Fixes the most-likely-to-actually-contact overflow pair.** Big γ. |
| **L4** | + **COM 21→9** | **52** | **COM×COM 81 ✅ · COM×Spine2 45 ✅** | **The highest-leverage single cut.** Only cut that also fixes a *correctness* cliff. |
| **L4b** | COM 21→7, Spine 11→7 | 44 | 49 ✅ | Fully inside the hard `nᵢ·nⱼ ≤ 63` bound everywhere. |
| **L5** | torso reverted to wave-1 singles | **46** | 1 | ≈ the shipped, **in-VR-confirmed** wave-1 bake. **Your known-good anchor.** |
| **L6** | stock | 18 | 1 | The measurement baseline. |

### Why COM is the first thing to cut
It is simultaneously (a) the largest child count, (b) the body resting on the floor mesh (β), (c) the body other NPCs pile onto (γ, cross-actor), (d) the body the player's hands touch (γ, self-collision), and (e) **the body that breaches the 256-point ceiling against another COM.** It loses on every axis. **21 → 9 is the single change with the best fidelity-per-millisecond ratio, and the only one that also closes a correctness cliff.**

### ★ The cut you should make *instead* of cutting — child LOD

Report 09 establishes that COM's 21 children exist mostly so a grab can name *glute* vs *belly* vs *hip*. **That fidelity is only needed for the actor the player is actually touching** — which, per §5.1, is at most **two** actors, ever.

If the §6.2 step-8 spike passes, you already have the machinery:
- **Full child set** for actors that are grabbed, being touched, or within ~2 m of the player.
- **Child 0 only** (via `disableChild` on children 1..N) for everyone else.
- Hysteresis on the distance band; **only toggle when the actor is not in contact**, so the collision envelope never pops mid-touch.

This converts:
- the **always-on AABB tax** from `O(actors × 96)` to `O(actors × 1)`,
- the **raycast multiplier** from 14× to 1× for every NPC except the touched one,
- the **cross-actor COM×COM = 441** cliff to **COM(21) × COM(1) = 21** — ✅ **structurally safe.**

> **The enabled-children LOD is not an optimization. It is the correctness fix for the cross-actor contact-point cliff.** Build it even on a Green result.
>
> It does **not** fix the *intra-actor* pair (COM 21 × Spine2 11 = 231), because that fires on the one actor that has full children. **You still need L3 or L4.**

**Guards, restated, because this is where the next CTD lives:**
`m_numDisabledChildren` exactly equals the cleared-bit count (replicate the `cmp/je`) · invalidate `hkpCollidable::boundingVolumeData` after every mutation · assert `capacityChildShapeAabbs >= childInfo.size()` **before re-enabling** · `RepatchListAabb()` skipping disabled children · if you call `hkpWorld_UpdateCollisionFilterOnEntity(..., PROCESS_SHAPE_COLLECTIONS)` at all, do it under `BSWriteLocker(worldLock)` from the **pre-drive hook, never from inside the step.**

### What is not available
No Havok-side acceleration structure exists for a capsule list (§3.7). **Child count is the only lever.**

---

## 8. PREDICTION, WITH ERROR BARS

Basis: **verified structure** (which loops run, how many times) × **modelled per-operation costs** (x86 instruction counts at 3.5 GHz / IPC 2–3). **Treat the ms figures as ±3× and the structure as reliable.** All figures are Δ vs the stock 18-capsule build.

| Scenario | Δ ms/frame (culled model, §2.1 verdict) | Δ ms/frame (linear model, if §2.1 is wrong) | Confidence |
|---|---|---|---|
| Idle NPC, nobody touching, no crowd | **+0.001 ms/actor** (the always-on AABB tax) | same | high |
| City crowd, 25 active ragdolls, nobody touched | **+0.03–0.05 ms** | same | high |
| Player touching **one** NPC (self-collision ON) | **+0.01–0.03 ms** | +0.10–0.20 ms | medium |
| Same, but the fit interpenetrates (~60 live contacts) | **+0.10–0.30 ms** (solver rows + contact-atom growth + PLANCK's world contact callback on every add) | +0.2–0.5 ms | medium |
| City crowd, 25 ragdolls, ~10 actor-pairs brushing | **+0.05–0.15 ms** | +0.2–0.6 ms | medium-low |
| **Worst realistic** (player grabs an NPC in a crowd, pushes into a corpse pile) | **+0.5–1.5 ms** (4–14 % of budget) | +1.0–2.5 ms | **low** |
| Raycast-heavy moment (HIGGS hand near a torso, projectile through it) | **not in any of the above.** ~14× on that path. Bounded by ray count, not capsule count. | same | structure high, magnitude **[U]** |
| Contact-buffer overflow on COM×Spine2, Spine0×Spine2, or COM×COM | **not a slowdown — warn+reject, or memory corruption. [U] which.** | same | *possibility* verified; *probability* **[U]** |

**Where you are actually in trouble:** not at any total child count. **The 11.1 ms budget is never the binding constraint** — even the absurd 2,891-child-pair ceiling at 200 ns/agent is 0.58 ms. **The binding constraint is 256 contact points on a single body pair.**

**Free wins already in hand:** the COM/Spine0 part-number alias kills COM×Spine1 (231 pairs) for nothing. The 21.7× self-collision multiplier only ever applies to **at most one actor at a time.** And PPB's bare-capsule children (no `hkpTransformShape`) already avoid Havok's documented 17 %/23 % compound-shape penalty.

---

## 9. OPEN QUESTIONS — WHAT WE COULD NOT ESTABLISH

Ordered by how much they change the answer.

| # | Unknown | Impact | How to settle |
|---|---|---|---|
| **1** | **Does Skyrim's `bhkCollisionFilter` implement the *shape-collection* `isCollisionEnabled(input, a, b, container, key)` overloads using the child's `collisionFilterInfo`, or does it `return true`?** | Gates per-child layers, the orifice ring's non-colliding inner capsules, and a possible fix for COM×Spine2 via part-number reassignment. Sole load-bearing assumption of A/B method 3. | Write `0x4000` into ONE child's `collisionFilterInfo` at runtime and poke it. **10 minutes.** |
| **2** | **`bhkCollisionFilter::bipedBitfields[0..23]` values.** Assumed to disable exactly the constraint-tree neighbours. | The 133-allowed-pairs figure is an *upper bound*. If vanilla disables more, every γ number shrinks. | `RE::bhkCollisionFilter::GetSingleton()`, log 24 u32s once. **5 read-only lines.** |
| **3** | **`hkpWorldCinfo::m_collisionTolerance` / `m_createContactThreshold` in SkyrimVR.** Havok default is 0.1 m ≈ 7 game units. | Determines how far apart two capsules can be and still generate a contact point — i.e. **how easily the 256 cap is reached.** | Read at runtime. **Do this before trusting any "they'll never touch" argument.** |
| **4** | **Is the 256-point breach a graceful reject (User Guide) or a buffer overrun (header assert)?** | Determines whether the cliff is "a warning in a log you don't read" or "memory corruption." | Do not find out empirically. Instrument `maxPointsInOnePair` and stay under 200. |
| **5** | **Does `hkpCollectionCollectionAgent3` really run, or `hkListAgent3`?** (§2.1) | 5× on all touch-window and crowd γ figures. Does *not* change the enable/disable verdict or the 256 cliff. | The measured γ coefficient from step 17 answers it. Or disassemble `hkpCollisionDispatcher::internalRegisterCollisionAgent`'s table fill. |
| **6** | **Is the Havok step on the same thread as the frame tick, and is `postPhysics` (0xB268DC) really after it?** | If not, `stepMs` no longer adds 1:1 to frame time and you need ETW. | Step 9's phase-order trace. |
| **7** | **Which agent handles capsule-vs-capsule inside a collection — `hkpCapsuleCapsuleAgent` (3 contact points) or GSK (4)?** | Changes the hard-safe bound from `nᵢ·nⱼ ≤ 83` to `≤ 63`. | Use 4. Costs nothing to be conservative. |
| **8** | **Do `hkpListAgent::staticGetClosestPoints` / `staticLinearCast` (char-proxy + HIGGS grab path) AABB-reject children?** | Determines whether the player standing near an NPC pays O(96) every frame. Implementation absent from the vendored SDK. | Microbench: QPC around 1,000 fixed linear-casts through a statue NPC's torso at 18 / 46 / 96 children. |
| **9** | **`entityBatchRecalcAabb`'s inner-loop instruction count** (80/child assumed; plausible 50–200). | ±2.5× on the always-on tax (0.03 → 0.08 ms at 25 actors). Small either way. | Falls out of the fitted β. |
| **10** | **The actual current capsule census.** Brief says 18; Report 09:112 says wave-1 already reached 45; the baked NIF I parsed shows 11 list bodies and **no torso lists**. | You cannot plan a cut list against a census you don't have. | Step 5 emits ground truth. **Do this first.** |
| **11** | **Whether the `expansionFlag` in `extractCachedAabbsOrRecalculate` is set by Skyrim's collision input** (`[input+0x60]->[+0x10]`). | Determines whether you pay a per-pair `quickSort<AabbInt>` or just a memcpy. | Read the field. Minor. |
| **12** | **Whether Skyrim's NIF loader will construct `bhkMoppBvTreeShape` over a `bhkListShape`.** | Would open the (probably worse — §3.7) MOPP route. | Don't. Vanilla never pairs them. |
| **13** | **All per-operation nanosecond figures.** Nothing in the SDK, the disassembly, or the web gives *measured* Havok 2010 narrowphase costs. The libs are x86; Skyrim is x64/SSE. | Every ms figure in §8. | §6. That is the whole point of §6. |

---

## 10. SOURCES

**Primary (local, exact-version, authoritative):**
- Havok Physics 2010.2.0-r1 SDK — headers, release static libs, PDBs, and the official User Guide PDF. Two independent copies on this machine: `F:/CHIM Emotion/Havok/` and `G:/Claude Workspace/tools/Havok 2010 Files/`. Version confirmed at `Source/Common/Base/Config/hkConfigVersion.h:27-29` (2010.2.0, "r1") and by the header footer `BUILD(#20101115)` — **the exact build Skyrim ships.**
- Implementation recovered via `lib.exe /EXTRACT` + `dumpbin /DISASM /SYMBOLS` on `hkpCollide.lib`, `hkpDynamics.lib`, `hkpUtilities.lib` (MSVC 14.51). Physics `.cpp` sources are absent from the SDK ("NO SOURCE PC DOWNLOAD").
- `D:/Games/My Skyrim/Tools/CommonLibVR-4.14.0/include/RE/H/hkpListShape.h`, `.../hkpCollidable.h`
- `G:/Claude Workspace/tools/_research/planck_src/src/main.cpp`, `.../src/RE/havok.cpp`, `.../src/RE/offsets.cpp`, `.../include/utils.h`, `.../include/config.h`
- `G:/Claude Workspace/tools/_research/higgs/src/hooks.cpp`, `.../src/pluginapi.cpp`, `.../src/hand.cpp`, `.../include/config.h`, `.../include/higgsinterface001.h`
- `G:/Claude Workspace/tools/PPB-plugin/src/CapFix.cpp` (`RepatchListAabb`, 125-152), `.../src/Hooks.cpp`, `.../src/PPBHook.cpp`, `.../src/HiggsInterface.h`, `.../vcpkg.json`
- `G:/Claude Workspace/tools/VRBackpack-plugin/src/main.cpp:8725-8990` (in-process OpenVR acquisition pattern)
- `D:/Games/My Skyrim/mods/Precision Physic Bodies/meshes/actors/character/character assets female/skeleton_female.nif` (raw binary parse + pynifly)
- `D:/Games/My Skyrim/mods/PLANCK - Physical Animation and Character Kinetics NEW/SKSE/Plugins/activeragdoll.ini` (lines 442, 493, 505-516)
- `SkyrimVR.exe` `.pdata` — 118,450 `RUNTIME_FUNCTION` records scanned. `.text` is DRM-encrypted on disk (Shannon entropy 7.997 b/B vs `.rdata` 4.80, `.pdata` 6.22). **This is the residual gap behind every "Skyrim uses stock X" inference in this report.**

**Web:** corroborating only. Nothing on the web was load-bearing; the local SDK superseded it. (Vendored Havok header mirrors on GitHub; the BOTW `hkStubs` decomp.)

---

## 11. CONFIDENCE SUMMARY

| Claim | Conf. | Basis |
|---|---|---|
| `hkpListShape` has no acceleration structure | 99 % | header + disasm |
| **Broadphase is completely unaffected by child count** | 97 % | single `hkpTypedBroadPhaseHandle`; `getAabbImpl` is O(1) |
| **`disableChild`/`enableChild` are allocation-free, call-free, idempotent, and present in SkyrimVR at `0xA9C3F0`/`0xA9C420`** | 95 % | disasm + PLANCK RelocAddrs + `.pdata` leaf-function proof |
| **Both agents rebuild the child key list from `m_enabledChildren` on every `process()`; `castRayImpl` honours it too** | 95 % | five independent disassemblies |
| Raycast against a list is O(children), unculled | 96 % | `castRayImpl` disasm + header doc |
| `HK_MAX_CONTACT_POINT = 256` per body pair; assert-only guard | 95 % | `hkpProcessCollisionData.h:20`, `hkpProcessCollisionOutput.h:41-47` |
| Up to 3 contact points per capsule-capsule child pair (4 for GSK) | 90 % | `m_contactPointId[3]`; `HK_NUM_MAX_CONTACTS = 4` |
| PLANCK's non-adjacent self-collision is gated on HIGGS-body contact ⇒ ≤ 2 actors | 95 % | `main.cpp:4356-4376`, `:2304-2330` |
| COM and Spine0 share biped part number 2 (⇒ COM×Spine1 free) | 93 % | pynifly dump of PPB + stock XP32 |
| **List collisions cull via a sorted 1-axis sweep, not linear iteration** | **85 %** | `_registerListAgents` disasm + `process_gatherShapeKeys` call graph. **[D] — one agent dissents; see §2.1** |
| `0xDFB722` is the 5-byte CALL to `hkpWorld::stepDeltaTime` | 92 % | HIGGS `Write5Call` site, source line numbers |
| The per-child AABB cache is live and rebuilt per body per frame | 90 % | `setCachedShapeData` + `entityBatchRecalcAabb` disasm. **[D] — see §2.2** |
| Skyrim VR uses this exact stock agent registration | 80 % | struct layout matches byte-for-byte; **exe dispatcher init not disassembled (encrypted `.text`)** |
| `m_boundingVolumeData.invalidate()` is needed after child mutation | 70 % | header warning + the sleeping-island hole; never tested |
| **`m_enabledChildren` is a safe live LOD lever on a real Skyrim ragdoll** | **75 %** | mechanism verified end-to-end; **never spike-tested in-game.** §6.2 step 8. |
| Absolute µs / ms figures anywhere in this report | **55 %** | instruction counts, **not measured**; x86 lib vs x64 game |
| Per-child `collisionFilterInfo` is consulted by `bhkCollisionFilter` | **[U]** | all three agents that looked could not read the override |

---

## 12. THE THREE SENTENCES, AGAIN

1. **96 sub-shapes per actor will not break your frame budget** — broadphase is untouched, narrowphase is sweep-culled, and the worst realistic scene costs an estimated **+0.5–1.5 ms (±3×)**, most of which lands on the *one* actor the player is touching.
2. **It will, however, put two body pairs (COM×Spine2 = 231 child pairs intra-actor, COM×COM = 441 cross-actor) above Havok's 256-contact-point-per-pair buffer**, whose only guard compiles out of the retail EXE — **that is a correctness cliff, and it is the reason not to bake COM at 21.**
3. **`hkpListShape::disableChild` gives you a real, verified, allocation-free, live per-child off switch** that the agent, the raycast, and the per-frame AABB pass all honour on the very next step — **so the fix is not to cut the design, it is to LOD it: full children for the touched actor, child 0 for everyone else, plus COM 21→9 to close the intra-actor pair.**

---

*Written 2026-07-08. Synthesized from four independent source-level investigations. Every ms figure herein is modelled. §6 exists to replace them with facts. When it does, amend `04_Pitfall_Ledger.md:7-8` and this report together.*
# 12 — The Contact-Point Cliff & Two-Actor Scene Physics (2026-07-08)

**Synthesis of four independent investigations** (limit-source · overflow-behavior · planck-scene · solution-arch). Where they disagree it is called out and adjudicated.

**Marking:** **[V]** verified from Havok 2010.2 SDK headers/.inl on disk, PLANCK source, PPB source, or the live measurement · **[I]** inferred from [V] · **[U]** unverifiable this session (the load-bearing `.cpp` bodies and the encrypted `SkyrimVR.exe` `.text` are not readable).

Primary cites: `tools/Havok 2010 Files/Source/Physics/Collide/Agent/hkpProcessCollisionData.h:20,71` · `.../hkpProcessCollisionOutput.h:44-48,103,106` · `.../ConstraintSolver/Constraint/Atom/hkpConstraintAtom.h:331,377,392` · `.../Dynamics/Constraint/Contact/hkpDynamicsCpIdMgr.h:19-21,46` · `.../World/Simulation/Multithreaded/Spu/hkpSpuConfig.h:24-29` · `tools/_research/planck_src/src/RE/havok.cpp:372-424` · `.../src/main.cpp:2007-2047` · `tools/_research/planck_src/include/config.h:201-223` · PPB `Diag.cpp` / `CapFix.cpp` · Reports 04, 11.

---

## 1. BOTTOM LINE

**Yes, a sustained two-actor physics sex scene is feasible with our bodies — but only if the fine interaction is NOT built as a fat orifice ring inside the ragdoll COM list.** The 256 contact-point ceiling is a compile-time Havok array size that cannot be raised at runtime by any means; the only real fix is structural — stop funneling both actors' 21-child COMs into one shared per-body-pair buffer. All four investigations converge on the same architecture: **(a)** keep the always-on ragdoll pelvis COARSE (an always-enabled core of ≤7 capsules per body); **(b)** carry the actual "which orifice / how deep" gameplay signal on a *handful of dedicated cavity-sensor follower bodies* that are separate rigid bodies (the proven finger-capsule / collviz model), body-level-filtered into a per-scene shared collision group so only sensor↔appendage collide (~6 pairs, forever under budget); **(c)** collapse each COM's extra "flesh-ring" children via the already-built `disableChild` bitmask for the duration of a scene; and **(d)** let CBPC/SMP do the soft-tissue *feel*, which never touches Havok at all. On this design a single body pair never approaches 200 simultaneous points, so whether the overflow is a graceful reject or a memory corruption becomes moot. **The one thing you must never do is put a detailed orifice ring in *both* driven actors' Havok COM lists with PLANCK cross-actor collision left on — that is the 441-child-pair worst case and the only path to the cliff.**

---

## 2. Can the 256 box be raised? — **NO. It is a compile-time C-array dimension, not a runtime field.** [V, confidence 96]

All three investigations that examined the mechanism agree, and this is the highest-confidence finding in the whole synthesis.

- The constant is `enum { HK_MAX_CONTACT_POINT = 256 };` (`hkpProcessCollisionData.h:20`), used to size a **fixed C-array member**: `hkpProcessCdPoint m_contactPoints[HK_MAX_CONTACT_POINT];` (`:71`). It is baked into `sizeof(hkpProcessCollisionOutput)` and therefore into every allocation, struct offset, and stack frame in the Havok code linked into `SkyrimVR.exe`. There is **no runtime field, no `hkpWorldCinfo` member, no `.ini` value** that stores "256". [V]
- Havok's own source labels it un-tunable-at-runtime. `hkpSpuConfig.h:24-29` sits under the banner *"These values are Havok tunable + require a full rebuild"* and defines the SPU contact limit directly from `HK_MAX_CONTACT_POINT`. Changing it means **recompiling `hkpCollide`**, which is impossible against a statically-linked, DRM-encrypted, integrity-checked retail exe. [V]
- **The one runtime-looking lever is a trap.** `hkpSimpleContactConstraintAtom::m_maxNumContactPoints` is an `hkUint16` you could physically write >256 into (`hkpConstraintAtom.h:392`), but the SPU/schema buffers it drives are sized by `HK_MAX_CONTACT_POINT` at compile time, and the atom comment says *"It holds up to 256 contact points"* (`:331`). Writing a bigger value relocates the overflow into other fixed buffers = **corruption, not capacity.** It is not a usable knob. [V]
- Two *sibling* 256-arrays (`m_representativeContacts[256]`, `m_potentialContacts[256]`, `hkpProcessCollisionOutput.h:103,106`) do the cross-child welding, so even a hypothetical recompile would have to raise all three in lockstep. [V]

**Risk if you try to fake it:** any patch that spoofs a higher ceiling (writing `m_maxNumContactPoints`, NOP-ing a compare) overruns a fixed array into adjacent members → save-corruption / CTD. **Do not attempt.** [V impossible / [I]-high on the corruption consequence.]

**Verdict:** the 256 box is un-raisable. Every workable option reduces *how many child-pair points one body pair can generate*, never the ceiling. This supersedes Report 11 §2.4/§5.4, which left the mechanism `[I]/[U]`.

---

## 3. What overflow actually DOES + what the 90 s-no-crash means

This is the **one place the investigations genuinely disagree.** Adjudication below.

### 3a. The disagreement
- **overflow-behavior (conf 83):** retail Havok **gracefully REJECTS** excess points at a ~255/pair manifold cap — no corruption. Evidence: `hkpContactMgr::reserveContactPoints()` returns `hkResult` with the contract *"if this function fails, no contact points have been reserved"* (`hkpContactMgr.h:57-61`); the persistent manifold stores contact IDs as `hkUchar` with `FREE_VALUE=0xff` → a hard **255** cap (`hkpDynamicsCpIdMgr.h:19-21,46`), whose assert literally reads *"The system only handles 255 contact points or less between two objects."* Matches the Havok 2010.2 User Guide verbatim (*"a warning is generated, and any more generated contact points will be automatically rejected"*). The real failure mode is therefore **silent contact-starvation / clipping / mushy resistance**, not a crash.
- **limit-source (conf 96) / solution-arch (conf 87):** overflow of the **staging buffer** (`m_contactPoints[256]`) is guarded ONLY by `HK_ASSERT2`, which **compiles out of retail** (`hkpProcessCollisionOutput.h:44-48`) → no runtime clamp on *that* buffer; treat as **unbounded corruption** and never reach it. solution-arch notes (per Report 11 §2.4) the staging buffer may fill *before* the manager's reserve gate rejects.

### 3b. Adjudication — I trust the graceful-reject model as MORE LIKELY (~80%), but do NOT rely on it
The two positions are about **two different stages** and are mostly reconcilable:
1. **Staging buffer** (`hkpProcessCollisionOutput::m_contactPoints[256]`) — assert-only, corruption-capable if written past 256. [V that the guard compiles out]
2. **Persistent manifold** (contact manager) — real `hkResult` reserve gate + `hkUchar` 255 hard cap. [V that these exist]

overflow-behavior's thesis is that the manifold gate stops points **upstream** (reserve-then-write), so the staging buffer never actually overflows. limit-source/solution-arch's fear is the opposite ordering (write-then-reserve), where narrowphase fills the staging buffer within one collide call before any gate fires.

**Which ordering is real is [U]** — the deciding `.cpp` bodies (`addContactPointImpl`, `reserveContactPointsImpl`, `hkpSimulation`) are absent from this "headers/.inl-only" SDK dump, and the exe is encrypted. Neither side can close it statically.

**I lean graceful-reject (~80%) for three reasons:** (i) the `reserveContactPoints` contract — *"if this function fails, no contact points have been reserved"* — is the semantics of a pre-commit gate, not a post-hoc cleanup; (ii) the `hkUchar` ID space is a **hard data-type cap of 255** — the manifold literally cannot *address* more points regardless of the staging buffer; (iii) prior-art evidence of absence (§3c).

**But I explicitly refuse to build on it, siding with solution-arch's caution, because:** the prior-art evidence (§3c) does **not** cover our novel case. Vanilla Skyrim never creates a *multi-child-list-vs-multi-child-list* body pair — vanilla ragdoll bodies are mostly single primitives, so a ragdoll pile is *many pairs each with few points*, never *one pair with 1764 points*. PPB's 21-child COM list is unprecedented, so "ragdoll piles don't crash" is weaker evidence for our exact scenario than it looks. The honest position: **graceful-reject is probably true, but unproven for our worst case, and the failure cost if wrong is save corruption — so design to never reach 256 and prove it empirically (§7) before shipping.**

### 3c. Prior art [V — evidence of absence]
No documented Skyrim/SkyrimVR CTD is attributable to contact-point-buffer overflow. The famous physics CTDs are a **different mechanism**: "Collision Sentinel" targets corrupted **MOPP bytecode** (broadphase/midphase mesh-tree corruption), and HDT-SMP "too many collision objects" is a **Bullet** limit in a separate physics world. Ragdoll piles and mass-death heaps are infamous for *lag and Havok "floating,"* not crashes — which is the reject/degradation signature, not a corruption one. `KNOWLEDGEBASE.md` has no contact-overflow entry. Corroborates graceful-reject for *vanilla-shaped* collision.

### 3d. What the live 90 s run actually tells us [V]
- **CPU is a non-issue and stays one.** `stepDeltaTime` mean **0.18 ms** / max **0.62 ms**. The contact-point cliff — not frame budget — is the only binding constraint. Ignore performance worries.
- **`maxPointsInOnePair = 4527 [UPPER-BOUND]` did NOT measure an overflow.** It is a **monotonic accumulator**, not an instantaneous count. Every deployed ragdoll body ships `contactPointCallbackDelay = 65535`, so the exact full-manifold walk essentially never runs and `newPoints` climbs cumulatively over 90 s, resetting only on Add/Remove callbacks. `r = 2263 pts/pair` is physically impossible — that's the tell it's accumulated. So the run proves neither safety nor overflow.
- **It exercised the WRONG case.** A single NPC groped = `HIGGS-hand(few bodies) × COM(21)` ≈ 63–252 points — near the line but survivable. It **never** exercised `COM(21) × COM(21)`, which needs two actors' pelvises interpenetrating. **The sex scene is the untested true worst case; "no crash at 90 s" is weak positive evidence, not proof.**
- **`childKeyOverflow = 238` and `droppedEvents = 0` are OUR diagnostic's counters, not Havok's** — completely benign census artifacts (a child shape-key ≥24 clamped by `SetChildBit`; our lock-free pair table never saturating). Unrelated to the contact limit.

**Net:** overflow most likely = **graceful reject → silent contact-starvation (clipping/mushy/jitter), a QUALITY cliff**, with a residual un-ruled-out corruption path for our novel multi-child case. The 90 s run vindicates the CPU model and settles nothing about the cliff.

---

## 4. Does the 441-cross-actor case even OCCUR? — **Yes by default, but it is unnecessary and avoidable.**

### 4a. PLANCK enables cross-actor ragdoll collision by DEFAULT [V]
`enableBipedBipedCollision = true`, `enableBipedBipedCollisionNoCC = true`, `doBipedNonSelfCollision = true` (`config.h:208-212`); the deployed `activeragdoll.ini` ships these = 1 (Report 11 §5.6). PLANCK flips the vanilla filter (`main.cpp:3982-4006`, `RE/havok.cpp:231-242`) so the Biped layer collides with itself — vanilla live characters pass through each other; under PLANCK they don't.

### 4b. Two driven NPCs' COMs DO collide, and there is no per-part rescue cross-actor [V]
The governing comparator `bhkCollisionFilter_CompareFilterInfosEx` (`RE/havok.cpp:372-424`) branches on collision group:
- **Same actor** (same system group) → per-part `bipedBitfields[partA]` bittest `partB` (`:400-401`) — the self-collision matrix.
- **Different actors** (different system group — the sex scene) → falls to `_bittest64(&layerBitfield, layerB)` (`:405-411`), a **layer-level** decision. **There is NO `bipedBitfields` part test cross-actor.** So with Biped×Biped enabled, *every* body pair between two NPCs is filter-approved, and COM×COM feeds all 441 child-pairs into one 256 buffer. Do NOT read the `bipedBitfields[0..23]` perf dump as permission/denial for the two-actor case — it governs one actor's spine-vs-limb only.

Condition to be "on": both actors alive, PLANCK-active (within `activeRagdollStartDistance` ≈ 50 m), COM AABBs overlapping. Gated by proximity + broadphase only — **not** by combat/knockdown. So two driven NPCs standing in a scene are eligible at all times. **The 441 worst case is genuinely reachable in a driven scene, not just a ragdoll pile.** [V]

### 4c. PLANCK's own guards do NOT save the 256 buffer [V/I-high — critical]
`stopRagdollNonSelfCollisionForCloseActors` (default on; `closeActorMinDistance = 2.0` **game units ≈ 2.9 cm**) and `IsRagdollCollisionIgnored` / the `AddRagdollCollisionIgnoredActor` API both fire inside `contactPointCallback` (`main.cpp:2007-2047`), which runs **after** narrowphase has already staged the manifold points. They set `CONTACT_IS_DISABLED` — suppressing the constraint solve and the gameplay hit — but do **not** unstage the points. `AddRagdollCollisionIgnoredActor` is a `std::set` insert consulted at callback time (`pluginapi.cpp:99-112`), **not** a broadphase filter edit. **Only broadphase-level filter edits (layer/group/no-collision-bit) stop the buffer from filling; PLANCK's gameplay guards are post-manifold and leave the buffer exposed.** Do not rely on them.

### 4d. The reframe that can make the cliff MOOT — the intimate interaction is a CBPC/SMP job, not a ragdoll job [V]
Every mainstream Skyrim adult-collision system runs its intimate collision **outside Havok** and never touches the ragdoll or the 256 buffer:
- **CBPC** = analytic sphere/capsule math in NiNode (bone) space, per frame, **zero Havok symbols** (nexus 21224; Report 11 §5.6; Pitfall Ledger 04:134-139). Ships a VR build; it is the standard VR touch layer PPB already feeds.
- **HDT-SMP / FSMP** = a separate **Bullet** simulation independent of Havok; genital/orifice collision authored in XML (`hdtvagina.xml`). Contends for CPU cores, never for the Havok step.
- **SOS** rides SMP/CBPC genital bones; **OStim/SexLab** scene physics = SMP + CBPC (OSmp/OUranos). None route intimate interaction through the Havok ragdoll.

**Consequence:** the soft breast/belly/genital/orifice *feel* that makes it a "physics scene" is CBPC/SMP, which is immune to the 256 cliff. The Havok ragdoll's only 256-buffer consumers are whole-body ragdoll/knockdown, **HIGGS hand↔body**, **weapon↔body**, and **cross-actor ragdoll push**. So the 441 disaster only exists *if you choose to put a detailed orifice ring in both actors' Havok COMs*. **Don't, and the cliff never applies to the scene.**

**Where the investigations differ on the interaction layer:** planck-scene would put the orifice ring **entirely in CBPC/SMP** and call the Havok cliff moot. solution-arch (and the user's goals charter — *"3 collision capsule cavity inside the female body… two for men"*) wants a **handful of dedicated Havok follower bodies** as cavity *sensors* to carry the gameplay signal (which orifice, how deep), with CBPC/SMP for feel. These are **not contradictory** — both reject a fat orifice ring in the ragdoll COM; they differ only on whether the *sense* is analytic (CBPC) or a few dedicated Havok bodies. limit-source independently arrives at "give the orifice ring its own dedicated body." **The synthesis in §6 keeps the dedicated-sensor path** because it matches the charter and uses **VERIFIED body-level filtering**, while treating CBPC/SMP as the feel layer.

---

## 5. Does BAKING reduce the risk? — **NO.** [V]

The contact-point count is purely `f(child-capsule counts, spatial overlap)`. That function is **identical whether a `bhkListShape`'s children were authored in the NIF or written live** — Havok sees the same list of capsules at collide time either way. Baking only relocates *authorship* (runtime CapFix float-edits / collviz body-creation → NIF blocks) and gives load-time presence; it does not add, remove, or merge capsules, and `disableChild` operates identically on baked children. **Baking neither creates nor cures the overflow — only child count and filtering do.**

One **directional caveat** that constrains the bake: the AABB-cache capacity is sized from `getNumChildShapes()` at world-add, so you can `disableChild` down but can **never runtime-grow past the baked maximum** (`Diag.cpp` capacity assert; Pitfall rules 2). **Therefore bake the LARGEST child set you will ever enable (≤21) and LOD downward** — never bake fewer and hope to add later.

---

## 6. RECOMMENDED ARCHITECTURE (ranked, concrete, build-first)

**Split the pelvis into three independently-filtered layers.** This leans only on VERIFIED mechanisms (body-level filter, `disableChild` bitmask, follower bodies, float edits) and treats the one unverified per-child filter as a bonus, never a dependency.

**Layer 1 — Coarse ragdoll core (always on).** Bake COM/pelvic so the *always-enabled* core is **≤7 capsules** per body (7²×4 = 196 < 256, safe cross-actor by construction even if all LOD/filtering fails). Also cut spine0/1/2 to **≤5** children each — this independently kills the *intra-actor* COM(21)×Spine2(11)=231-pair spike that fires whenever the player hand-touches a single actor (Report 11 §7). This core handles gross pelvis-vs-pelvis ragdoll push. Cost: bake-only. Risk: low. **[Rank 1 for the ragdoll layer]**

**Layer 2 — Fine flesh-ring = LOD-managed CHILDREN of COM.** Bake the ring (up to ~14 extra children, total ≤21) as `disableChild`-able children. Default idle + **during any scene: disabled on both participants** → each COM collapses to its ≤7 core → cross-actor becomes ≤7×7 = 196. Enabled only for the ≤2 actors the player is actively hand-touching (single-actor case, safe). Uses the **VERIFIED** `disableChild` bitmask (allocation-free bit flip, already reimplemented as `DisableChildInline`/`EnableChildInline`/`InvalidateBvd` + the `capdis` console command, `Diag.cpp:456-559`). **[Rank 1 — the correctness fix, not an optimization]**
- **Caveat [U→prove first]:** `disableChild` on a live Skyrim ragdoll is verified in mechanism (~90%) but **never spike-tested on a live ragdoll** (~75%). Run the §7 spike before depending on it.

**Layer 3 — Cavity sensors = dedicated FOLLOWER bodies (the heart of the scene signal).** Build the fine sex interaction as a *handful* of dedicated capsules — 1 mouth + 2 pelvic (female) / 2 (male) per the charter — that are **separate rigid bodies** hung off cavity nodes (the proven finger-capsule / collviz-marker model: NIF bodies *outside* the 18-body ragdoll instance; Pitfall Ledger explicitly ALLOWS "our own bodies"). Because they are standalone bodies they use **body-level `collisionFilterInfo` filtering, which is VERIFIED to work in Skyrim** — no dependence on the unverified per-child link. On scene start, stamp both participants' sensor + appendage bodies with a **shared non-zero collision group** so only `sensor↔partner-appendage` pairs collide; everything else gets the `0x4000` no-collision bit. Result cross-actor: ~3 sensors × ~2 appendage caps = **~6 body pairs, forever trivially under 256**, and this is what actually carries the "which orifice / how deep" gameplay signal. Clear the group on scene end. Cost: bake sensor bodies + wire scene-start group assignment. Risk: low (proven follower-body class). **[Rank 1 for the interaction layer]**

**Layer 4 — CBPC/SMP for the soft-tissue FEEL.** Immune to the cliff (§4d). Use it for visual deformation and the touch/deform signal; keep Havok coarse. Cost: none new (existing systems). **[Rank 2 — pragmatic soft layer]**

**Optional enhancement — per-child `0x4000` on ring inner-capsules.** Would let the ring stay enabled longer without cross-actor ring-ring risk, but **hard-depends on the [U] question of whether Skyrim's `bhkCollisionFilter` honors the shape-collection `isCollisionEnabled` overloads** (`hkpShapeCollectionFilter.h:41,46`) or just returns true. A 10-minute test settles it (§7). **Do NOT architect the ship build to depend on it** — it is a bonus if the test passes, and Layer 2's bitmask covers the same need if it fails. **[Rank 4 — not on critical path]**

### What to build FIRST (order)
1. **Run the already-built diagnostics in a TWO-ACTOR pose** (not the single grope): spawn 2 NPCs, interpenetrate pelvises, `perf` → read the REAL `maxPointsInOnePair` for COM21×COM21, plus `DumpCollisionFilter` (`bipedBitfields[0..23]` + `hkpCollisionInput::tolerance`). This + the §7 exact counter settles the three load-bearing [U]s. **Save first** (spike can theoretically CTD).
2. **Promote `disableChild` to a scene API** (Papyrus-native or the scene/LLM controller hook) that collapses both participants' COM ring-children on scene start and restores on scene end; verify it drops COM×COM to the safe core in the two-actor pose. Highest-leverage safety fix; reuses written code.
3. **Bake + wire the cavity-sensor follower bodies** + their per-scene shared group; verify only sensor↔appendage collides cross-actor.
4. **Bake the reduced-spine (≤5) + LOD-partitioned COM** (≤7 core, ring as disable-able children up to ≤21 total).
5. *(only if step-1's per-child test passed)* add per-child `0x4000` on ring inner capsules.

---

## 7. THE SAFE EMPIRICAL TEST (learn the truth without risking a real save)

### 7a. Per-frame SIMULTANEOUS-PEAK counter — the exact `Diag.cpp` change
The machinery to measure the TRUE instantaneous peak already exists — the `firingCallbacksForFullManifold` "EXACT" path (`Diag.cpp:257-265`) computes an exact per-walk count; it just never runs because every ragdoll body ships `contactPointCallbackDelay = 65535`. Flip that on the two armed actors and the existing exact path becomes the per-frame peak:
1. Add a tuning knob `perfExactDelay` (0 default) alongside `lodSpike` in `Tuning.h`/`Tuning.cpp`.
2. In `Diag::OnPreDrive(actor)`, when `perfExactDelay` is set AND the actor is the armed target, walk its 18 ragdoll bodies (reuse `ResolveBody`/`kNode18`) and write `rb->contactPointCallbackDelay = 0` — a `hkUint16` write to an **existing field** at `hkpEntity+0xFE` under the existing `BSWriteLockGuard`. **Pitfall-rule-1 safe (int edit to an existing field, no alloc, no type change).**
   - **Propagation caveat [I]:** the contact manager copies the delay from the entity at manager-creation time, so **set the field at ARM time, BEFORE forcing the pelvis overlap** — managers created for the new overlaps then inherit delay 0 and fire the full-manifold walk every step. For already-colliding pairs, toggle arm off/on to recycle managers.
   - **Gold-standard alternative (zero field writes):** read `hkpSimpleContactConstraintAtom::m_numContactPoints` (`hkUint16`, `hkpConstraintAtom.h:377`) directly off each pair's contact manager once per frame — the exact simultaneous count with no callback and no delay edit, but needs new RE traversal from `hkpRigidBody` to the manager.
3. With delay 0 the existing code writes `s->exact`/`maxFromExact=true` every step; feed a `maxLiveExact` field so `maxPointsInOnePair` reports a **true instantaneous peak** and the `[EXACT]` tag becomes truthful. Re-tag the ship-gate thresholds against this number.
4. The full-manifold walk self-delimits per pair per step (`firstCallbackForFullManifold` resets), so no separate per-step reset is needed; keep session-max as the reported peak. Note the callback fires on **4 thread ids** — the counter is already lock-free.

**Decisive outcomes:** with delay 0, if `maxPointsInOnePair [EXACT]` **plateaus at/just under ~255 no matter how hard you press** → graceful-reject CONFIRMED (SAFE). If it **sails past 256 with no incident** → the buffer is bigger than believed (investigate). If it **climbs toward 256 and then CTDs** → the corruption path is real and child counts MUST be hard-capped. Any of the three is a cheap, decisive result in ~10 minutes.

### 7b. Throwaway-save protocol (never touches the main save)
1. **Isolate.** Launch, `coc QASmoke` (empty interior), `tgm`, `tcai`, `set timescale to 0`. Make a NAMED hard save `PPB_OVERFLOW_TEST` and from here on only ever `load PPB_OVERFLOW_TEST`. Optionally run from a throwaway MO2 profile so even the save folder is disposable. **Never overwrite a real save.**
2. **Spawn two test NPCs** (`help "bandit" 4 NPC_`; `player.placeatme <BASE> 1` ×2). Race must carry `ActorTypeNPC` so PLANCK arms active-ragdoll. Wait until both are active-ragdoll.
3. **Arm.** Click NPC #1 → `perf` (census + filter dump). Set `perfExactDelay 1` in `PPB_tuning.txt` (auto hot-reloads).
4. **Force pelvis overlap.** With `tgm`+`tcai`: `moveto`/`setpos` both so the two COM(21) bodies deeply interpenetrate — the sustained COM×COM = 441-pair worst case. Add player HIGGS hands to the pile for the full case.
5. **Watch the peak climb** in `PPB.log` (the `capfix`/`perf` file loop already tails). Observe plateau ≤255 (SAFE) vs >256 (investigate) vs CTD (corruption → hard-cap child counts).
6. `load PPB_OVERFLOW_TEST` to reset between trials. A CTD/corruption in an empty named throwaway save costs nothing.

**Read the collision tolerance first (don't guess):** `DumpCollisionFilter` already logs `hkpCollisionInput::tolerance`. Havok's default is ~0.1 m ≈ 7 game units; if Skyrim's is that loose, many child pairs are simultaneously "in contact" even at visible separation — the real driver of manifold saturation. Treat it as a read-only diagnostic (no safe ini override established). [U whether ini-exposed]

### 7c. Kill switch (instant collapse to single-capsule) — ~90% already built
Generalize the existing `capdis` spike into `PpbCollapseAll(on)`: for every list body queue `mask = (all child bits) & ~1` (disable children 1..N-1, keep child 0 = the MAIN capsule) through the existing deferred, world-locked `ApplyPendingSpike` path (`Diag.cpp:509-559`, which already invalidates the BVD cache and audits `numDisabledChildren`); restore with `mask = 0`. Bind to a `killToSingle` knob **and** an automatic tripwire: if `maxPointsInOnePair [EXACT] > 240` on any pair for K consecutive steps, self-collapse and log — an in-engine safety that reacts before the 255 cap even if the reject model is wrong. Keep it behind the `lodSpike`-style double gate so it can never fire in a shipped build by accident. Note: disabling a child also removes it from raycasts (HIGGS grab / crosshair) but never renumbers indices, so grab-by-region survives; re-enabling is the guarded direction (the `EnableChildInline` capacity assert prevents growing past the world-add cache size).

---

## 8. Confidence + open questions

**Overall confidence in the architecture: ~88%.** It depends only on VERIFIED mechanisms and is robust to every open [U] because it keeps any single body pair under ~200 simultaneous points, so the graceful-reject-vs-corruption question never has to be answered to ship safely.

**High confidence [V, ~96%]:** 256 is a compile-time array, un-raisable at runtime; per-body-pair aggregate across all bhkListShape children; PLANCK enables cross-actor Biped×Biped by default and its guards are post-manifold; CBPC/SMP never touch the Havok buffer; baking changes nothing about contact count; the `disableChild` bitmask and body-level filter and follower-body class are the safe write surface.

**Open questions (ranked by how much they gate the plan):**
1. **[U→gate D] `disableChild` on a live Skyrim ragdoll** (~75%). The §7b step-1 spike confirms or kills the scene-LOD path. If it fails: fall back to a hard-cut baked COM ≤7 (lose ring granularity) + the follower-body sensors.
2. **[U→gate C-optional] per-child `collisionFilterInfo` honored by `bhkCollisionFilter`.** The 10-min `0x4000` test gates only the optional per-child enhancement; the plan does not depend on it.
3. **[U, ~80% graceful-reject] overflow = reject vs corruption.** Do NOT determine by risking a real save; the §7 exact counter settles it cheaply, and the design stays under 200 so it never matters in the shipped mod. The residual worry is that vanilla prior-art doesn't cover our novel multi-child-list case.
4. **[U] `hkpWorldCinfo::m_collisionTolerance`** (Havok default 0.1 m ≈ 7 u). Read it in step 1; if large, the coarse-core + dedicated-sensor split is *more* necessary, not less.
5. **[U] Skyrim VR's actual `m_maxNumContactPoints`** (documented "up to 256"). If Bethesda raised it above 256 the reserve gate would not protect the 256 staging buffer — the §7a peak would reveal a value >256.

**Cross-report reconciliation:** this report supersedes Report 11 §2.4/§5.4 on the overflow *mechanism* (now [V] compile-time array, per-pair aggregate) and confirms its §7 conclusion that the `disableChild` COM collapse is "the correctness fix, not an optimization." Report 11's structural math (441-pair worst case, `nᵢ·nⱼ ≤ 63` no-overflow bound) stands.

---

## ★★ IN-VR CONFIRMED RESULT (2026-07-09) — the reject-vs-corrupt unknown is now a MEASURED FACT
The safe test (this report's §7) was run: throwaway save, 2 NPCs, pelvis-grinding, `perf` armed.
- **maxPointsInOnePair = 1772** (7× over 256) and **NO CTD.** `droppedEvents > 0` = Havok GRACEFULLY
  DROPPING the overflow. So the 256 overflow is **BENIGN on this build/machine** — the ~80% graceful-reject
  theory is now empirically confirmed. **The physics sex-scene will NOT crash from the contact cliff.**
- Worst offender was `RThigh × LThigh [INTRA] = 1772` — ONE ragdolled NPC's own crossed thighs (6 caps
  each), not the pelvis cross (COM/Spn0 cross = 1046, #3). mean pts/pair = 48.92 over 437 pairs (MOST
  pairs never overflow — only the few deep-overlap ones do).
- **THE REAL ISSUE IS PERF, NOT CTD.** `STEPS/FRAME [2]=243` over ~105s = **2.3 catch-up double-steps/sec**
  (matches the user's felt "3/sec stutter"). The physics STEP itself is CHEAP (mean 0.33ms, max 0.88ms of
  11.1ms) — the stutter is the sheer contact VOLUME (437k events, 55k manifold walks) occasionally spiking
  a frame past 90Hz → Havok double-steps to catch up. Avg fps during test ~72.
- ⚠ CONFOUND: a big chunk of those 55k manifold walks are the `perf` COUNTER itself (armed). With `perf`
  OFF, gameplay is lighter — re-check the stutter perf-disarmed to separate counter-cost from real cost.
- **CONSEQUENCE for the recommendations above**: child-LOD / disableChild is DE-PRIORITIZED (it fixed a
  now-benign CTD). The scene fix is PERF-motivated: (1) collision FILTERING — drop the self-thigh and
  cross-flesh pairs nobody interacts with (biggest safe win); (2) the ~7-game-unit collision TOLERANCE
  (dominant volume driver — capsules "touch" from 7u away); (3) contact DECIMATION (keep deepest N pts) —
  the user's "don't process 50%" idea, valid but physics is only ~3% of frame so it smooths spikes rather
  than boosting fps. The no-LOD partition (report 13) still helps but is no longer urgent.
# 13 — No-LOD Solutions to the 256-Contact Cliff (2026-07-09)

**Synthesis of four fresh investigations** (mesh-mopp · shell-perimeter · buffer-redirect · peak-test-tweak), building on Report 12. Written to answer one question the user pinned: **the fat orifice detail must stay on BOTH actors — no child-LOD. Is there a no-LOD path, and which?**

**Marking:** **[V]** verified from the Havok 2010.2 SDK headers/.inl on disk, PLANCK/HIGGS source, PPB source, or a live measurement · **[I]** inferred from [V] · **[U]** unverifiable this session (the load-bearing `.cpp` bodies are absent from the SDK dump and `SkyrimVR.exe` `.text` is DRM-encrypted).

**Primary cites (spot-checked this session):** `tools/Havok 2010 Files/Source/Physics/Collide/Agent/hkpProcessCollisionData.h:20,71` (the 256 inline array) · `.../ConvexAgent/CapsuleCapsule/hkpCapsuleCapsuleAgent.h:54` (`m_contactPointId[3]`) · `.../hkpProcessCollisionOutput.h:37-48,103,106` · `.../CompoundAgent/BvTree/hkpMoppAgent.h:17-22` + `hkpBvTreeAgent.h:21` · `.../Shape/Compound/Tree/hkpBvTreeShape.h:29-47` · `.../Util/Welding/hkpWeldingUtility.h:29,43` · `.../ConvexAgent/Gjk/hkpGskConvexConvexAgent.h:48-49` + `hkpClosestPointManifold.h:22` · `.../Shape/Convex/ConvexVertices/hkpConvexVerticesShape.h:21-23` · `.../Shape/Deprecated/ConvexList/hkpConvexListShape.h:22-24` · `.../Shape/Compound/Collection/List/hkpListShape.h:43,127,130` · `.../Dynamics/World/hkpWorldCinfo.h:242` · `.../Constraint/Contact/hkpDynamicsCpIdMgr.h:19-21` · `CommonLibVR-4.14.0/include/RE/H/hkpEntity.h:103` + `hkpContactPointEvent.h` · `tools/_research/planck_src/src/main.cpp:5018` + `src/RE/havok.cpp:372-424` · PPB `Diag.cpp` / `Tuning.cpp` / `CapFix.cpp` · `tools/pynifly/.../pyn/mopp_compiler.py`.

---

## 1. BOTTOM LINE

**Yes — a no-LOD solution exists, and it is not a compromise on detail. It is a re-grouping of the same capsules.**

The whole cliff is a consequence of one fact: **the 256-point ceiling is PER BODY-PAIR, not per actor and not global.** Every simultaneously-overlapping child-pair of one colliding rigid-body pair funnels into a single inline `m_contactPoints[256]` staging array. Both actors' fat 21-child pelvis rings live inside their *one* ragdoll COM rigid body, so COM_A × COM_B is a **single** pair that must carry up to 21×21 = 441 child-pairs. That is the only reason the ceiling is in reach.

**The no-LOD fix keeps every capsule on both actors and instead spreads them across more RIGID BODIES.** Because the ceiling is per-pair, partitioning the 20-capsule orifice ring across **K separate follower rigid bodies** turns one 441-child pair into K×K pairs, each carrying ~(20/K)² child-pairs — worst-case per-pair load falls ≈ K². K=4 takes the worst case from ~441 to ~25 child-pairs per pair. **Zero capsules removed. Full orifice detail on both actors. No LOD.** These follower bodies are standalone NIF-authored rigid bodies hung off cavity nodes — *outside* the 18-body ragdoll instance — so this is HARD-RULE compliant (it is the proven finger-capsule / `collviz_markers.dll` / baked-R-hand class, not a new ragdoll body).

Two supporting levers make it bulletproof and are worth shipping alongside: **(a)** a **convex** coarse pelvis core (one `bhkConvexVerticesShape` per pelvis) so the ever-present COM×COM bulk pair welds to ≈4 points instead of tens; **(b)** optionally, move the "which orifice / how deep" signal entirely OUT of the simulation manifold into an on-demand **query-sensor** (the HIGGS `getClosestPoints` pattern), which is cliff-immune by construction.

**The one thing you must never do** is leave the detailed ring in *both* driven COM lists as a single fat body with PLANCK cross-actor collision on. That is the 441-child worst case and the only path to the cliff.

---

## 2. Does the cliff even get hit? — **YES on the current single-list design; 441 is a ceiling never reached, but the real median still breaches budget.** [I-high, from verified geometry]

The single most important correction from this round changes the budget itself:

- **[V] Capsule-vs-capsule generates at most 3 points, not 4.** `hkpCapsuleCapsuleAgent` carries `hkContactPointId m_contactPointId[3]` (`hkpCapsuleCapsuleAgent.h:54`, read this session). Capsule-vs-triangle is likewise ≤3 (`hkpCapsuleTriangleAgent.h`). Only the generic GSK convex-convex manifold caps at **4** (`hkpClosestPointManifold.h:22` / `hkpGskConvexConvexAgent.h:48-49`). PPB's projection table hard-codes r=4 as a structural bound (Diag.cpp:684-698) — that is 33% conservative for an all-capsule body.
- **[I] So the real staging budget is 256 ÷ 3 ≈ 85 simultaneous capsule-pairs**, not the 64 that Report 12 and the prompt assumed. The pair budget is 42% larger than believed. The hard persistent-manifold cap remains **255** contact IDs (`hkUchar`, `hkpDynamicsCpIdMgr.h:19-21`).

**Is 441 ever reached?** No. 441 requires both 21-child COMs to be near-coincident with *every* child-pair inside the collision tolerance simultaneously — a degenerate ceiling, not an operating count. Only child-pairs whose surfaces fall within the collision tolerance stage any points; a distant AABB-overlapping pair appends **0**.

**But the operating count on the current solid-fill design still breaches budget**, and the driver is a fat tolerance:

- **[V] The collision tolerance is ~7 game units.** `hkpWorldCinfo::m_collisionTolerance` default = 0.1 Havok-m (`hkpWorldCinfo.h:242`) × ~69.9 u/m ≈ **7 units** (PPB Diag.cpp:360-364 reads it live). That is a huge "contact skin" relative to genital anatomy (canal radius ~4-8u, ring-capsule radius ~2-4u). Consequences: a shell's hollow interior is partly back-filled by the skin, and if a canal's radius < 7u an axial probe registers the *whole* ring regardless of topology. **This, not clever geometry, is the dominant driver — which is why keeping child-count LOW beats any topology trick.**

Decomposed into the two physically-distinct interaction zones (this split is fundamental, not cosmetic — the zones have different pair-scaling):

| Zone | Interaction | Realistic simultaneous pairs | Points (×3) |
|---|---|---|---|
| **Canal** (appendage ↔ orifice ring) | near-parallel capsules, tree/arc-limited | 8–20 (20-stave) / 8–10 (10-stave) | 24–60 / 24–30 |
| **Pelvic flesh** (COM bulk ↔ COM bulk), solid fill | O(N²) volumetric overlap | **40–120** | **150–420** |
| **Pelvic flesh**, hollow shell | O(N) surface-curve intersection | 12–40 | 36–170 |

**CANAL IS NEVER THE CLIFF.** The **pelvic flesh press is the cliff.** Realistic two-actor penetration-pose totals:

- **Current solid-fill, no fix:** ~50–140 pairs → 150–420 pts. **Median ~90 pairs ≈ 270 pts → BREACHES the 85-pair/256 budget on the median and hard in the tail.** So on the shipping design the cliff *is* hit in a real scene, not just at the 441 ceiling.
- **Shell flesh + 20-stave ring:** ~20–60 pairs → 60–180 pts. Median ~40 pairs ≈ 120 pts → UNDER budget, but a bad tail (~75 pairs) still flirts with 85.
- **Shell + canal cut to ~8–10 staves + flesh coarsened/convex:** ~6–25 pairs → 18–75 pts. **Deeply safe with margin.**

**Answer:** the 441 is a ceiling never reached, but the honest operating median on the current fat single-list design (~90 pairs / ~270 pts) already sits *at or over* the cliff, with an overflowing tail. A no-LOD fix is therefore genuinely needed, not just prudent.

---

## 3. Each option graded

### Option 1 — MESH / MOPP on the orifice — **VIABLE_WITH_CAVEATS (asymmetric only)** [conf 80]

- **[V] Mesh does NOT escape the 256 funnel.** Every collision agent — capsule, convex-triangle, triangle-triangle — commits into the same `m_contactPoints[256]` via one bump pointer, guarded only by an assert that compiles out of retail (`hkpProcessCollisionOutput.h:37-48`). A mesh gets no separate buffer.
- **[V] Welding snaps NORMALS, it does not cap COUNT.** `hkpWeldingUtility` only snaps collision normals at shared triangle edges to stop internal-edge catching (`hkpWeldingUtility.h:29,43`); its own arrays are 256-sized. The task premise that "mesh contacts get welded/reduced in number" is false.
- **[V] The ONLY count-reduction in narrowphase is per-convex-pair (GSK caps each pair at 4).** There is no global/aggregate contact reducer. So aggregate = Σ over child pairs, funneled into 256.
- **[V] Convex-vs-mesh (one convex probe inside a concave MOPP orifice) IS cheap** — the bvtree/MOPP agent uses the tree to pre-filter to only the triangles overlapping the probe's AABB and dispatches convex-triangle agents for just those (`hkpBvTreeShape.h:41-47`). A probe inside a ~20-triangle canal touches maybe 8–20 triangles → ~8–40 staged points. **This is where mesh wins — and only here.**
- **[V] Mesh-vs-mesh (both COMs meshes, deeply overlapping) is as bad or WORSE** — `hkpMoppAgent` collides all triangles of the smaller MOPP against the bigger (`hkpMoppAgent.h:17-22`), all committed into the same 256 box with no aggregate cap, plus far more CPU. **"Make both COMs meshes" is a dead end.**
- **[V-design] MOPP is architected for STATIC geometry** ("a moving object and a large static geometry, such as a landscape", `hkpBvTreeShape.h:29-30`; the bvtree agent assumes bodyB is the bvtree, `hkpBvTreeAgent.h:21`; every vanilla Skyrim MOPP is `OL_STATIC` landscape). A *moving* keyframed MOPP is a supported case (elevators/platforms; the tree is rigid, the agent recomputes the other body's AABB in MOPP-local space each frame). A **dynamic (mass/force-reactive) MOPP** — which a COM would be, since PLANCK sets active-ragdoll COMs to `MOTION_DYNAMIC` (`planck_src/main.cpp:5018`) — is the *discouraged* configuration and **[U]** in SkyrimVR (unverifiable from the encrypted exe).
- **[V] Bake feasibility: YES.** pynifly ships a complete offline MOPP compiler (`pyn/mopp_compiler.py`), a verifier (`scripts/mopp_verifier.py`), and a benchmark against vanilla Havok-compiled bytecode (`scripts/mopp_benchmark.py`); `nif/collision.py:1063 export_bhkMoppBvTreeShape` bakes the full shape chain. Caveat: pynifly's MOPP child must be a triangle mesh; the Havok-suggested "hkpListShape-of-capsules under a MOPP" variant (which Havok recommends *exactly* for objects other bodies frequently sit "inside", `hkpConvexListShape.h:22-24`) is not a standard Skyrim NIF layout and only helps in *partial* overlap anyway.

**Verdict:** VIABLE but strictly ASYMMETRIC — mesh reduces points *only* when exactly one side of the colliding pair stays convex, and the mesh must ride a **keyframed cavity-sensor follower body**, never the dynamic COM. Never let a mesh face a mesh. As stated in the prompt ("both COMs become meshes"), it is a DEAD END. As "hollow shell MOPP on a keyframed sensor vs a convex appendage capsule," it is the single most detail-faithful realization — but it carries the [U] dynamic-MOPP + Collision-Sentinel-corruption risk and MUST be spike-tested before ship.

### Option 2 — SHELL / PERIMETER wrap + per-child collision filter — **VIABLE_WITH_CAVEATS** [conf 82]

- **[V-geometry] The shell reframe is correct.** A hollow shell converts O(N²) volumetric overlap into O(N) surface-curve intersection: two overlapping shells intersect along a *band*, not a volume, so pairs-within-tolerance ≈ (band length ÷ capsule spacing). It moves the flesh median from *at-the-cliff* (~90 pairs) to *under* (~40 pairs). **This is the correct topology.**
- **CAVEAT (the fat tolerance):** under the ~7u skin a shell's interior is partly back-filled and a sub-7u canal lights the whole ring regardless. So the shell is **necessary but not sufficient alone** — it must be paired with a low child-count and the canal/flesh split. It is not a no-op (buffer-redirect's harsher read), because it genuinely removes the *interior* layers of a stacked volumetric fill; but its benefit is capped by tolerance, so keep N small.
- **Per-child `collisionFilterInfo` — PLAUSIBLE but UNVERIFIED for Skyrim [U].** The DATA exists (`hkpListShape::ChildInfo::m_collisionFilterInfo`, `hkpListShape.h:43`; get/set at :127/:130) and the MACHINERY exists (`hkpListAgent::updateShapeCollectionFilter` → `hkpShapeCollectionFilter` once per child pair). BUT PLANCK's decompiled comparator and every Skyrim RE reference operate on **body-level** collidable filter info (`planck_src/RE/havok.cpp:372-424`), so whether Skyrim's `bhkCollisionFilter` implements the shape-collection overload to honor per-child info (vs returning true) is untested. **Do not depend on it.** Use the two VERIFIED alternatives instead: (i) make the flesh a SEPARATE rigid body with a body-level filter (the proven sensor model), or (ii) `disableChild` the flesh children during scenes (verified bitmask). The 10-min in-game test settles it.

**Verdict:** VIABLE and materially effective as topology; combine with the canal/flesh split + low N. The per-child-filter idea is the weak part — treat as a bonus to be tested, never a dependency.

### Option 3 — REDIRECT / ENLARGE the 256 buffer — **DEAD END** [conf 92, the highest-confidence finding]

- **[V] 256 is a compile-time C-array dimension, not a runtime field.** `enum { HK_MAX_CONTACT_POINT = 256 }` sizes the inline member `hkpProcessCdPoint m_contactPoints[HK_MAX_CONTACT_POINT]` (`hkpProcessCollisionData.h:20,71`, read this session). It is baked into `sizeof()` and every stack frame of the statically-linked, DRM-encrypted, integrity-checked exe. There is no pointer to retarget and no pool to enlarge.
- **[V] Overflow is an unclamped stack write, not a benign clamp.** The retail `commitContactPoints` guard is an `HK_ASSERT2` that compiles out (`hkpProcessCollisionOutput.h:44-48`); point #257 writes past the array into the in-struct TOI info and then the welding-info pointer, which is dereferenced on the next weld → fast CTD within ~2–4 points of overflow.
- **[V] No `hkpWorldCinfo` field enlarges it.** The only contact-adjacent fields (tolerance, restingVelocity, `m_contactPointGeneration`, TOI queue size) are reducers or unrelated; none enlarge the buffer. `reset()` re-pins `m_firstFreeContactPoint` to `&m_contactPoints[0]` every collide, and three sibling 256-arrays plus the SPU DMA schema hard-assume the [256] base+stride.
- **[V] The one runtime-looking lever is a trap.** Writing `hkpSimpleContactConstraintAtom::m_maxNumContactPoints > 256` relocates overflow into other fixed buffers = corruption. And the persistent manifold addresses points with an `hkUchar` cpId (`FREE_VALUE=0xff`) = a hard **255** data-type ceiling regardless of any buffer.
- **[V] No safe pre-manifold hook.** The contact listener is POST-manifold (too late to unstage). The only pre-manifold surface is a narrowphase agent registered globally by SHAPE TYPE in an init-time, full, 18-slot table — a custom kList×kList agent would replace list-vs-list handling for *every* pair in the game and cannot be scoped or hot-swapped safely.

**Verdict:** DEAD END, robust to the one open uncertainty (stack-vs-heap): even if the buffer were heap, (a) the compile-time size, (c) the reset-repin, and (d) the no-safe-hook all still hold. Do not attempt any buffer-side fix.

### Also-evaluated: CONVEX HULL (`bhkConvexVerticesShape`)

- **[V] RULED OUT for the orifice** — a convex hull is by construction convex (`hkpConvexVerticesShape.h:21-23`); a hull of a ring is a solid disc that seals the canal, so a probe hits the sealed hull and never enters.
- **[V] EXCELLENT for the coarse pelvis CORE** — whole-bulk-vs-appendage is one GSK pair welded to ≈4 points. This beats a 7-capsule core (1 convex vs 1 convex ≈ 4 pts vs up to 49) and is a bake-only shape-type change (SAFE per hard rules).

### Also-found: QUERY-SENSOR (from the buffer-redirect arm) — **VIABLE, cliff-immune**

- **[V]** Don't put the fine interaction in the simulation manifold at all. Run on-demand `hkpCollisionDispatcher::getClosestPoints` each frame between a handful of sensor shapes, writing into a **caller-owned** `hkpCdPointCollector` — NOT the 256 staging buffer. This is exactly what HIGGS does (`hand.cpp:211-215`). It carries "which orifice / how deep" with **zero** ragdoll contact points and no cap exposure. It cannot corrupt because it never touches the manifold. Best paired with, or as a replacement for, the follower-body ring where the signal is purely informational (gameplay/SkyrimNet) rather than physical (push-back force).

---

## 4. RECOMMENDED NO-LOD ARCHITECTURE

Full detail on both actors, only verified mechanisms, HARD-RULE compliant. Four layers, in priority order:

1. **Coarse always-on core = ONE convex hull per pelvis** (`bhkConvexVerticesShape`, NIF-bake). The COM×COM bulk pair — the one pair that is *always* present when two actors touch — welds to ≈4 points instead of tens. This is the always-on floor and it is nearly free. (Report-12 Layer-1, upgraded from capsules to convex.)

2. **Orifice ring PARTITIONED across K follower rigid bodies (K≈4), NOT one COM list.** ★ **This is the load-bearing no-LOD move.** Keep all ~20 capsules (or trim to ~10 fat ones — see Layer 4). Split them across K standalone NIF rigid bodies hung off the pelvic-cavity node, each its own rigid body with its own body-level collision filter. Because 256 is per-body-pair, the worst case per cross pair drops ≈K² (441 → ~25 child-pairs at K=4). This is the finger-capsule / `collviz_markers.dll` / baked-R-hand class — **standalone follower bodies OUTSIDE the 18-body ragdoll**, so it is *not* an added ragdoll body (the forbidden operation) but the *allowed* one (our own follower bodies, per the Pitfall Ledger). Detail is not cut — it is spread.

3. **Move the informational signal to a QUERY-SENSOR where physical push-back isn't needed.** For "which orifice / how deep" as a *gameplay/SkyrimNet* signal, prefer per-frame `getClosestPoints` into a caller-owned collector (HIGGS pattern) over simulation contacts. Cliff-immune, zero manifold cost. Reserve real simulation contacts (Layers 1–2) for cases that need felt resistance.

4. **Cross-actor pruning + child-count discipline (verified levers only):**
   - **Reduce N:** cut the ring from 20 to ~8–10 fat capsules. The soft-tissue *feel* is CBPC/SMP, which never touches the 256 buffer; Havok only needs enough capsules to carry the signal. 8 staves × 3 = 24 pts even if the whole ring hugs.
   - **Cross-actor filter:** give the flesh/ring follower bodies a body-level collision filter (VERIFIED) so cross-actor flesh↔flesh can be dropped while hand↔ring still collides — do NOT rely on the unverified per-child list filter.
   - **`disableChild` during scenes:** collapse any remaining fat COM ring children via the already-built, audited bitmask for a scene's duration.

**What is explicitly REJECTED as the primary fix:** mesh-vs-mesh on both COMs (Option 1 naive form), buffer redirect/enlarge (Option 3), and depending on per-child list filtering. The asymmetric keyframed-shell-MOPP (Option 1 refined) and per-child filter remain **optional experiments to validate**, not load-bearing.

**Interplay with soft-tissue:** CBPC/SMP jiggle is immune to the cliff (Report 12 §4d) — it never enters the Havok manifold. The fat-orifice *look and feel* the user wants is carried by that layer plus the (unchanged, full-detail) capsule geometry; nothing here reduces visible or tactile fidelity.

---

## 5. THE DEFINITIVE PER-FRAME-PEAK TEST

Purpose: measure the **true per-body-pair simultaneous peak** in the real COM×COM two-actor worst case, and learn whether overflow rejects gracefully or corrupts — at zero save risk. Report 12's live `4527 [UPPER-BOUND]` was a **monotonic accumulator artifact**, not a peak; this test replaces it with the clean EXACT count.

### 5a. Why the existing instrument almost works
Diag.cpp already contains the exact per-step counter (the `firingCallbacksForFullManifold` branch, ~lines 257-265): on `firstCallbackForFullManifold` it resets `walk=0`, each callback increments, on `lastCallbackForFullManifold` it stores `s->exact = w`. **`s->exact` is already the true simultaneous count of one full-manifold walk.** It never runs today only because every ragdoll body ships `contactPointCallbackDelay = 65535`, so managers never count down to 0 and only the UB `newPoints` accumulator runs. The fix is: **(a) force delay→0 so the existing exact path fires, and (b) report a clean EXACT-only session max.**

Verified enabling facts (this session):
- **[V]** `contactPointCallbackDelay` is `hkUint16` at `hkpEntity+0xFE` (`CommonLibVR-4.14.0/hkpEntity.h:103`), base of `hkpRigidBody`, so `rb->contactPointCallbackDelay = 0;` writes directly. It is a runtime field rebuilt from the NIF at cell load — **NOT serialized into the .ess/co-save** → pitfall-rule-1 safe (int write, existing field, no alloc, no shape-type change).
- **[V]** The full-manifold walk enumerates **all points in that body-pair's manifold** when the *contact manager's* delay hits 0 (`hkpContactListener.h:27-30`) → walk-count = the true per-body-pair simultaneous count.
- **[V] Propagation caveat (critical):** the manager caches its OWN delay (`hkpSimpleConstraintContactMgr.h:127`) copied from the entity at manager creation; the per-step countdown reloads from the manager's copy (`hkpCollisionEvents.inl:54`). **Writing the entity field only affects managers created AFTER the write** → you MUST zero it BEFORE the two pelvises first overlap, or recycle existing managers (disable/re-enable, or pull the actors apart and back).

### 5b. Minimal DLL change (three files; no new console command, no main.cpp change)
Everything is driven by three hot-reloaded `PPB_tuning.txt` knobs plus the existing `perf` arm/disarm. Full patch spec (verified against live source) is in the task record; summary:

- **`Tuning.h`** — add 3 knobs to `struct GrabTune`: `perfExactDelay` (when 1 + `perf` armed, zero `contactPointCallbackDelay` on every driven actor's 18 ragdoll bodies → full-manifold walk fires every step), `killToSingle` + `killAt` (double-gated test backstop). Add `PerfExactDelayEnabled()/KillToSingleEnabled()/KillAtThreshold()` accessors.
- **`Tuning.cpp`** — register all three `PK_NOSNAP` (must never re-dress capsules); define the accessors.
- **`Diag.cpp`** — (1) `PairSlot`: add `maxExact` (clean EXACT-only session peak) + `liveThisStep`; (2) EXACT path: bump `maxExact` and publish `liveThisStep` right after `s->exact.store(w)`; (3) `ApplyExactDelay(actor)` under the world write-lock (idempotent, only writes non-zero delays), called from `OnPreDrive` when armed; (4) `StepSweep()` after the chained `stepDeltaTime` returns (Havok workers joined = safe barrier) to snapshot+reset `liveThisStep`, track `g_maxExactInstant`, and fire the tripwire; (5) `ApplyKillCollapse(actor)` — on first breach of `killAt`, collapse every list body to child-0 via the existing `DisableChildInline`/`AuditList`/`InvalidateBvd` machinery under the world lock (deferred to next `OnPreDrive`, ~1 frame latency); (6) `DumpReport`: log `maxSimultaneousPointsInOnePair [EXACT per-step]` with a GREEN(≤200)/AMBER(≤240)/AMBER(near 255)/RED(>255) gate, keeping the old monotonic line as a cross-check; (7) reset all new counters in `ResetCounters`/`ClearOnLoad`.

`maxExact` is monotonically maxed but each *sample* is already a true per-step instantaneous manifold size (first…last delimited), so its session-max is a genuine peak — fundamentally unlike the old `newPoints` lifetime accumulator. `liveThisStep.exchange(0)` each step ensures a pair that stops colliding reports 0 (no stale kill-switch trips).

**Kill-switch latency caveat (deviates from Report 12's "~240/K-consecutive"):** the collapse can't run mid-step (worker thread, world mid-narrowphase); it defers to the next frame under the world lock = ~1 frame (~11 ms @ 90 Hz). In that frame the count can climb further, so **fire on the FIRST breach** (not after K steps) and default `killAt 200`, not 240, to leave headroom below the 255 cap. Lower it further if the log shows steep single-step jumps.

### 5c. Throwaway-save protocol (never touches a real save; ideally a disposable MO2 profile)
1. **Isolate.** `coc QASmoke` → `tgm` → `tcai` → `set timescale to 0`. Hard-save `PPB_OVERFLOW_TEST`; thereafter only ever `load PPB_OVERFLOW_TEST`.
2. **Edit `D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_tuning.txt`** (hot-reloads ≤1 s): `perfExactDelay 1`, `lodSpike 1`, `killToSingle 1`, `killAt 200`.
3. **Spawn two ragdoll-capable NPCs** (`help "bandit" 4 NPC_` → `player.placeatme <BASE> 1` ×2; race must carry `ActorTypeNPC` so PLANCK arms active ragdoll). Wait until both are driven — `OnPreDrive` per actor auto-zeros their delays.
4. **Arm.** Select NPC #1 in console → `perf`. Confirm the census now shows `cpCallbackDelay=0` on driven bodies before proceeding. **Read `hkpCollisionInput::tolerance` from the filter dump first** (Diag logs it) — if ~7u, many child-pairs count as "in contact" even at visible separation; that is the real saturation driver.
5. **Ordering (propagation caveat).** Force overlap ONLY after step 4 so new COM×COM managers inherit delay 0. If already touching, `disable`→re-`perf`→`enable`, or pull apart and back, to recycle managers.
6. **Force the worst case.** With `tgm`+`tcai`, `moveto`/`setpos` both NPCs so the two COM(21) pelvis bodies deeply interpenetrate; sustain it (add the player's HIGGS hands for the full case). Hold minutes.
7. **Watch `PPB.log`** — live `instantCrossPairPeak`, and on disarm (`perf` again) `maxSimultaneousPointsInOnePair [EXACT per-step]`.

### 5d. Pass/fail interpretation
- **Plateaus ≤255 no matter how hard you press → graceful-reject CONFIRMED (SAFE).** Overflow degrades to contact-starvation (clipping/mushy), not a crash. A single-list ring may even be shippable with margin — but the follower-body partition is still the cleaner default.
- **Climbs past 256 and survives → the buffer is bigger than believed** (Bethesda raised the cap or the path differs). Record the true value, re-tag the gate, still cap child counts conservatively.
- **Climbs toward 256 then CTDs → the corruption path is REAL.** Child counts MUST be hard-capped and the follower-body partition becomes mandatory production architecture. If `killToSingle 1` collapsed everything at 200 and prevented the crash, you'll see `KILL-SWITCH fired` + survival — which independently proves the live-ragdoll collapse path works (Report 12 open-Q #1).
- `load PPB_OVERFLOW_TEST` between trials; a CTD in an empty throwaway save costs nothing.

---

## 6. Confidence + what the in-game test would change

| Claim | Confidence | Basis |
|---|---|---|
| 256 is un-raisable / buffer redirect is a dead end | **96%** | Inline C-array read on disk this session |
| Capsule-pair = 3 pts; budget ≈ 85 pairs (not 64) | **95%** | `m_contactPointId[3]` read this session |
| Mesh helps only asymmetrically (convex-vs-mesh); mesh-vs-mesh no better | **90%** | MOPP/bvtree/welding agent headers |
| Convex core welds to ~4 pts; ruled out for the orifice | **95%** | `hkpConvexVerticesShape` + GSK manifold headers |
| Follower-body partition drops per-pair load ≈K², keeps full detail, HARD-RULE compliant | **90%** | Per-pair ceiling [V] + PPB follower-body precedent |
| Query-sensor is cliff-immune | **90%** | HIGGS `getClosestPoints` pattern [V] |
| Current single-list median (~90 pairs) breaches budget | **~80% (I)** | Geometry + 7u tolerance; no live two-actor measurement yet |
| Overflow rejects gracefully vs corrupts | **~80% reject** | UNPROVEN for our novel multi-child-list × multi-child-list pair |
| DLL peak-test patch correctness/safety | **~92%** | Every field/offset/lock/call-site verified against live source |

**What the CTD test result changes:**
- **If it plateaus ≤255 (graceful reject):** the whole cliff becomes a *quality* problem (mushy contacts), not a *crash* problem. The follower-body partition + convex core is then a robustness/feel upgrade rather than a crash-prevention necessity, and a trimmed single-list ring might even ship. Raises the "current design is survivable" confidence from ~20% toward ~85%.
- **If it climbs then CTDs (corruption):** the follower-body partition becomes **mandatory**, single-fat-COM-ring is permanently banned, and the kill-switch collapse (if it fired) is confirmed as a viable runtime safety net. Turns the ~80% reject assumption into a settled "no."
- **Either way** it converts the two [I]/[U] load-bearing unknowns (real operating median, and reject-vs-corrupt) into measured facts in ~10 minutes at zero save cost — the single highest-value experiment remaining for this subsystem. It also incidentally proves (or disproves) the live-ragdoll `disableChild` collapse path, Report 12's last unproven mechanism.

---

*Supersedes nothing in Report 12; extends §5–§7 with the corrected 85-pair budget, the follower-body-partition framing of the no-LOD fix, the graded option table, and the concrete peak-test DLL patch + protocol.*
