"""Append capsule children to the SHIPPED PPB skeletons (2026-08-17).

  COM  (NPC COM [COM ])  32 -> 34   on ALL FOUR skeletons
        +2 for the DRESS GATE: two capsules that will be raised/lowered by the
        clothed-state signal so the pelvic orifices are not reachable while dressed.
  HEAD (NPC Head [Head]) 23 -> 25   on HUMAN + DRAENEI only (beast heads are 17 and
        are not involved; ApplyListSlot loops the actor's OWN child count so they are
        unaffected by the raised knob bound).
        +2 to receive AURI'S ANTLERS, freeing head 15/16 — which currently carry them
        via npcCap — for the chin seal on her too. The npcCap lines move to 23/24.

SEED SPEC (ledger rule): a tiny buried rod, NEVER a verbatim clone of a real child --
a clone would duplicate visible geometry on every actor until a knob overwrote it.
  point1 = (0,0,0)  point2 = (0,0,2u)  radius = 0.5u   (never A==B: degenerate = invisible)
Material is copied from child[1] so the impact-sound lookup still resolves.

Safety: per-file backup, work on a temp copy, verify by RELOAD (target count, every child
still a capsule, body count unchanged, and EVERY other body's shape+childcount unchanged).
Any mismatch -> temp discarded, original untouched.
"""
import sys, os, shutil
sys.path.insert(0, r"G:\Claude Workspace\tools\pynifly\extracted\io_scene_nifly")
from pyn.pynifly import NifFile
from pyn.nifdefs import bhkCapsuleShapeProps, VECTOR3

BASE = (r"D:/Games/My Skyrim/mods/Precision Physic Bodies/meshes/actors/character/"
        r"character assets female/PPB")
HAVOK = 0.0142875
SEED_LEN_M = 2.0 * HAVOK
SEED_R_M   = 0.5 * HAVOK
TAG = "bak_pre_com2head2"

# file -> list of (slotNode, expectedBefore, add)
JOBS = {
    "skeleton_female.nif":            [("NPC COM [COM ]", 32, 2), ("NPC Head [Head]", 23, 2)],
    "skeleton_female_draenei.nif":    [("NPC COM [COM ]", 32, 2), ("NPC Head [Head]", 23, 2)],
    "skeletonbeast_female.nif":       [("NPC COM [COM ]", 32, 2)],
    "skeletonbeast_female_khajiit.nif":[("NPC COM [COM ]", 32, 2)],
}


def child_counts(nif):
    out = {}
    for nm, node in nif.nodes.items():
        co = node.collision_object
        if co is None or co.body is None:
            continue
        sh = co.body.shape
        tn = type(sh).__name__
        out[nm] = (tn, len(sh.children) if tn == "bhkListShape" else 0)
    return out


def seed_from(ref):
    rp = ref.properties
    p = bhkCapsuleShapeProps()
    p.bhkMaterial = rp.bhkMaterial
    p.bhkRadius = SEED_R_M
    p.radius1 = SEED_R_M
    p.radius2 = SEED_R_M
    p.point1 = VECTOR3(0.0, 0.0, 0.0)
    p.point2 = VECTOR3(0.0, 0.0, SEED_LEN_M)
    return p


rc = 0
for fname, jobs in JOBS.items():
    path = os.path.join(BASE, fname).replace("/", os.sep)
    print("=" * 72)
    print(fname)
    if not os.path.isfile(path):
        print("  !! MISSING - skipped"); rc = 1; continue
    bak = path + "." + TAG
    tmp = path + ".work_" + TAG
    if not os.path.exists(bak):
        shutil.copy(path, bak); print("  backup ->", os.path.basename(bak))
    else:
        print("  backup exists (not overwriting)")
    shutil.copy(path, tmp)
    nif = NifFile(tmp)
    before = child_counts(nif)
    targets = {}
    abort = None
    for slot, expect, add in jobs:
        if slot not in nif.nodes:
            abort = "node %r absent" % slot; break
        lst = nif.nodes[slot].collision_object.body.shape
        if type(lst).__name__ != "bhkListShape":
            abort = "%s is %s, expected bhkListShape" % (slot, type(lst).__name__); break
        n0 = len(lst.children)
        if n0 == expect + add:
            abort = "%s already %d children - already extended?" % (slot, n0); break
        if n0 != expect:
            abort = "%s has %d children, expected %d" % (slot, n0, expect); break
        ref = lst.children[1]
        if type(ref).__name__ != "bhkCapsuleShape":
            abort = "%s child[1] is %s" % (slot, type(ref).__name__); break
        for _ in range(add):
            lst.add_shape(seed_from(ref))
        targets[slot] = expect + add
        print("  %-18s %d -> %d" % (slot, n0, len(lst.children)))
    if abort:
        os.remove(tmp); print("  ABORT:", abort); rc = 1; continue
    nif.save()

    v = NifFile(tmp)
    after = child_counts(v)
    ok = True
    for slot, want in targets.items():
        sh = v.nodes[slot].collision_object.body.shape
        if type(sh).__name__ != "bhkListShape" or len(sh.children) != want:
            ok = False; print("  !! %s verify FAILED (%s/%d)" % (slot, type(sh).__name__, len(sh.children)))
            continue
        for i, ch in enumerate(sh.children):
            if type(ch).__name__ != "bhkCapsuleShape":
                ok = False; print("  !! %s child[%d] is %s" % (slot, i, type(ch).__name__))
        for i in range(want - dict((s, a) for s, _e, a in jobs)[slot], want):
            p = sh.children[i].properties
            print("     new[%d] p1=%s p2=%s r=%.3fu" % (
                i, tuple(round(x / HAVOK, 2) for x in p.point1),
                tuple(round(x / HAVOK, 2) for x in p.point2), p.bhkRadius / HAVOK))
    if len(after) != len(before):
        ok = False; print("  !! body count changed %d -> %d" % (len(before), len(after)))
    for nm, val in before.items():
        if nm in targets:
            continue
        if after.get(nm) != val:
            ok = False; print("  !! collateral change on %r: %s -> %s" % (nm, val, after.get(nm)))
    if not ok:
        os.remove(tmp); print("  VERIFY FAILED - original untouched"); rc = 1; continue
    os.replace(tmp, path)
    print("  VERIFY PASS - deployed (%d bytes)" % os.path.getsize(path))

sys.exit(rc)
