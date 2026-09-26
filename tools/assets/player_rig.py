"""Build the skinned player model from the static one the game already ships.

The player model (Counter-Strike-nds/data/obj_PlayerStatic.bin) is a single Nitro Engine
display list: one connected low-poly body holding a pistol, 170 polygons. Cutting it into
rigid parts would tear it open at every joint, so it is skinned instead, the way DS games
skin their characters: every vertex belongs to one bone, and the display list switches the
current matrix to that bone's (MTX_RESTORE from the hardware matrix stack) before sending
the vertex. The geometry engine transforms each vertex when it arrives, so a polygon whose
corners belong to two bones stretches across the joint instead of splitting. Normals are
sent after the restore, so the lighting follows the bone too.

At draw time the game computes one matrix per bone and stores it in the stack slots named
here; the display list does the rest. Nothing is animated on the CPU per vertex.

Bones, in the order character_anim_core.h expects (parents first):

    0 root     no vertices; whole-body lean and falls pivot at the feet
    1 pelvis   belt line and crotch
    2 chest    torso and shoulders
    3 head
    4 arms     both forearms, the hands and the pistol, pivoting at the shoulder line;
               the upper arms stretch between it and the chest
    5 thigh_l  the character's left leg is the forward one in the rest pose (-x)
    6 shin_l
    7 thigh_r
    8 shin_r

Model space: y is up, the character faces -z, its right is +x. One unit is 4096 in the
display list. The joint positions below were read off the mesh (see --preview).

Usage:
    python3 tools/assets/player_rig.py              # write Counter-Strike-nds/data/player_rig.bin
    python3 tools/assets/player_rig.py --preview out.png [--pose]

File layout (little-endian), checked by CharacterRig_Parse():
    u32 magic 'CSRG'  u16 version (1)  u16 bone count  u16 first matrix slot  u16 0
    per bone, 12 bytes: s8 parent, u8 slot (0xFF: no vertices), s16 rest pitch (32768 per
                        turn, as glRotateXi; + swings a hanging limb forward), s16 length
                        to the next joint (1/4096; legs only), s16 pivot x, y, z (1/4096)
    then the display list as glCallList takes it: a word count, then packed GX commands.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "Counter-Strike-nds", "data")
SOURCE = os.path.join(DATA, "obj_PlayerStatic.bin")
OUTPUT = os.path.join(DATA, "player_rig.bin")

MAGIC = 0x47525343  # 'CSRG'
VERSION = 1
# Hardware matrix stack slots 22..29 hold the bone matrices while a character is drawn.
# Nitro Engine only pushes and pops (never deeper than a few entries) and never stores or
# restores by index, so the top of the 31-entry stack is free. Must equal RIG_FIRST_SLOT.
FIRST_SLOT = 22

ROOT_BONE, PELVIS, CHEST, HEAD, ARMS, THIGH_L, SHIN_L, THIGH_R, SHIN_R = range(9)
BONE_NAMES = ["root", "pelvis", "chest", "head", "arms", "thigh_l", "shin_l", "thigh_r", "shin_r"]
PARENTS = [-1, ROOT_BONE, PELVIS, CHEST, CHEST, PELVIS, THIGH_L, PELVIS, THIGH_R]

# Joint positions in model units, read off the mesh.
GROUND = (0.0, -1.45, -0.20)       # between the feet: falls pivot here
WAIST = (0.0, 0.30, -0.10)         # middle of the belt ring
NECK = (0.0, 1.18, -0.02)
SHOULDERS = (0.0, 1.07, -0.12)      # between the two shoulder joints
HIP_L = (-0.17, 0.12, -0.14)
HIP_R = (0.17, 0.12, -0.06)
KNEE_L = (-0.29, -0.565, -0.475)   # centre of the left knee ring
KNEE_R = (0.35, -0.668, 0.086)
ANKLE_L = (-0.35, -1.03, -0.56)
ANKLE_R = (0.43, -1.02, 0.33)
PIVOTS = [GROUND, WAIST, WAIST, NECK, SHOULDERS, HIP_L, KNEE_L, HIP_R, KNEE_R]

# Vertex classification thresholds (model units).
HEAD_MIN_Y = 1.18      # the neck ring and up...
HEAD_HALF_WIDTH = 0.2  # ...but not the shoulder tops beside it...
HEAD_MIN_Z = -0.45     # ...nor the hands or the pistol, which are well forward
ARMS_MAX_Z = -0.55     # forward of the chest (its front is at -0.48): elbows and on
ARMS_MIN_Y = 0.6
CHEST_MIN_Y = 0.36     # above the belt ring
LEG_MAX_Y = 0.20       # below the belt ring, except the two crotch vertices
CROTCH_HALF_WIDTH = 0.05
KNEE_SPLIT = 1.08      # along hip->knee: the knee ring (t ~ 1) stays with the thigh

# GX commands and their parameter word counts.
CMD_PARAMS = {
    0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12,
    0x18: 16, 0x19: 12, 0x1A: 9, 0x1B: 3, 0x1C: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2,
    0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1, 0x30: 1,
    0x31: 1, 0x32: 1, 0x33: 1, 0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3,
    0x71: 2, 0x72: 1,
}
NOP, MTX_RESTORE, COLOR, NORMAL, TEXCOORD, VTX_16, VTX_10 = 0x00, 0x14, 0x20, 0x21, 0x22, 0x23, 0x24
VTX_XY, VTX_XZ, VTX_YZ, VTX_DIFF, BEGIN_VTXS, END_VTXS = 0x25, 0x26, 0x27, 0x28, 0x40, 0x41


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s10(v):
    v &= 0x3FF
    return v - 0x400 if v & 0x200 else v


# ----------------------------------------------------------------------------------------
# Display list decoding
# ----------------------------------------------------------------------------------------

def read_words(path):
    data = open(path, "rb").read()
    if len(data) % 4:
        raise SystemExit("%s: not a whole number of words" % path)
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def decode_commands(words):
    """[(cmd, [params])] for a glCallList-style list (a word count, then packed commands)."""
    count = words[0]
    if count + 1 > len(words):
        raise SystemExit("display list claims %d words, file has %d" % (count, len(words) - 1))
    body = words[1:1 + count]
    out = []
    i = 0
    while i < len(body):
        packed = body[i]
        i += 1
        for shift in (0, 8, 16, 24):
            cmd = (packed >> shift) & 0xFF
            if cmd not in CMD_PARAMS:
                raise SystemExit("unknown GX command %02x" % cmd)
            n = CMD_PARAMS[cmd]
            if i + n > len(body):
                raise SystemExit("GX command %02x runs past the end of the list" % cmd)
            out.append((cmd, body[i:i + n]))
            i += n
    return out


def decode_primitives(words):
    """[{'type': 0 tris / 1 quads, 'verts': [(pos, texcoord word, normal word)]}]

    Positions are resolved to absolute 1/4096 units whatever vertex command sent them.
    Colours are not supported: the player model has none, and they would need carrying.
    """
    prims = []
    current = None
    last = (0, 0, 0)
    texcoord = None
    normal = None
    for cmd, args in decode_commands(words):
        if cmd == BEGIN_VTXS:
            if args[0] not in (0, 1):
                raise SystemExit("only triangle and quad lists are supported, got type %d" % args[0])
            current = {"type": args[0], "verts": []}
            prims.append(current)
            continue
        if cmd == END_VTXS:
            current = None
            continue
        if cmd in (NOP, MTX_RESTORE):  # bone switches only occur in our own output
            continue
        if cmd == TEXCOORD:
            texcoord = args[0]
            continue
        if cmd == NORMAL:
            normal = args[0]
            continue
        a = args[0] if args else 0
        if cmd == VTX_16:
            last = (s16(a), s16(a >> 16), s16(args[1]))
        elif cmd == VTX_10:
            last = (s10(a) << 6, s10(a >> 10) << 6, s10(a >> 20) << 6)
        elif cmd == VTX_XY:
            last = (s16(a), s16(a >> 16), last[2])
        elif cmd == VTX_XZ:
            last = (s16(a), last[1], s16(a >> 16))
        elif cmd == VTX_YZ:
            last = (last[0], s16(a), s16(a >> 16))
        elif cmd == VTX_DIFF:
            raise SystemExit("VTX_DIFF is not supported (the player model does not use it)")
        else:
            raise SystemExit("unexpected GX command %02x in a model" % cmd)
        if current is None:
            raise SystemExit("vertex outside BEGIN_VTXS")
        if texcoord is None or normal is None:
            raise SystemExit("vertex without a texture coordinate and a normal")
        current["verts"].append((last, texcoord, normal))
    for p in prims:
        if len(p["verts"]) % (3 if p["type"] == 0 else 4):
            raise SystemExit("primitive list with a partial polygon")
    return prims


# ----------------------------------------------------------------------------------------
# Skinning
# ----------------------------------------------------------------------------------------

def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def leg_bone(p, hip, knee, thigh, shin):
    axis = sub(knee, hip)
    t = dot(sub(p, hip), axis) / dot(axis, axis)
    return thigh if t < KNEE_SPLIT else shin


def bone_of(pos):
    x, y, z = (c / 4096 for c in pos)
    p = (x, y, z)
    if y > HEAD_MIN_Y and abs(x) < HEAD_HALF_WIDTH and z > HEAD_MIN_Z:
        return HEAD
    if z < ARMS_MAX_Z and y > ARMS_MIN_Y:
        return ARMS
    if y > CHEST_MIN_Y:
        return CHEST
    if y > LEG_MAX_Y or abs(x) < CROTCH_HALF_WIDTH:
        return PELVIS
    if x < 0:
        return leg_bone(p, HIP_L, KNEE_L, THIGH_L, SHIN_L)
    return leg_bone(p, HIP_R, KNEE_R, THIGH_R, SHIN_R)


TURN = 32768  # binary angle units per turn, as libnds' glRotate*i takes them


def rest_pitch(upper, lower):
    """Angle of upper->lower from straight down, + toward the front (-z)."""
    d = sub(lower, upper)
    return int(round(math.atan2(-d[2], -d[1]) * TURN / (2 * math.pi)))


def length(a, b):
    return math.sqrt(dot(sub(a, b), sub(a, b)))


def limb_lengths():
    lengths = [0] * len(BONE_NAMES)
    lengths[THIGH_L] = length(HIP_L, KNEE_L)
    lengths[SHIN_L] = length(KNEE_L, ANKLE_L)
    lengths[THIGH_R] = length(HIP_R, KNEE_R)
    lengths[SHIN_R] = length(KNEE_R, ANKLE_R)
    return lengths


def rest_pitches():
    pitches = [0] * len(BONE_NAMES)
    pitches[THIGH_L] = rest_pitch(HIP_L, KNEE_L)
    pitches[SHIN_L] = rest_pitch(KNEE_L, ANKLE_L)
    pitches[THIGH_R] = rest_pitch(HIP_R, KNEE_R)
    pitches[SHIN_R] = rest_pitch(KNEE_R, ANKLE_R)
    return pitches


def pack(cmds):
    """Packs [(cmd, params)] four commands to a word, like the hardware FIFO expects."""
    words = []
    for i in range(0, len(cmds), 4):
        group = cmds[i:i + 4]
        while len(group) < 4:
            group.append((NOP, []))
        words.append(sum(c << (8 * k) for k, (c, _) in enumerate(group)))
        for _, params in group:
            words.extend(params)
    return [len(words)] + words


def encode(prims, bones):
    cmds = []
    for p in prims:
        cmds.append((BEGIN_VTXS, [p["type"]]))
        current = None
        for pos, texcoord, normal in p["verts"]:
            bone = bones[pos]
            if bone != current:
                cmds.append((MTX_RESTORE, [FIRST_SLOT + slot_of(bone)]))
                current = bone
            cmds.append((TEXCOORD, [texcoord]))
            cmds.append((NORMAL, [normal]))
            x, y, z = pos
            cmds.append((VTX_16, [((y & 0xFFFF) << 16) | (x & 0xFFFF), z & 0xFFFF]))
        cmds.append((END_VTXS, []))
    return pack(cmds)


def slot_of(bone):
    # The root has no vertices and no slot; the rest are numbered in bone order.
    return bone - 1


def check_roundtrip(prims, words, bones):
    """The skinned list must send exactly the original polygons, bone switches aside."""
    again = decode_primitives(words)
    if [(p["type"], p["verts"]) for p in again] != [(p["type"], p["verts"]) for p in prims]:
        raise SystemExit("re-encoded display list does not match the original geometry")
    restores = [args[0] for cmd, args in decode_commands(words) if cmd == MTX_RESTORE]
    used = {FIRST_SLOT + slot_of(b) for b in set(bones.values())}
    if set(restores) != used or max(restores) > 30:
        raise SystemExit("matrix slots out of range")


def build():
    prims = decode_primitives(read_words(SOURCE))
    positions = {v[0] for p in prims for v in p["verts"]}
    bones = {pos: bone_of(pos) for pos in positions}
    words = encode(prims, bones)
    check_roundtrip(prims, words, bones)

    pitches = rest_pitches()
    lengths = limb_lengths()
    used = set(bones.values())
    out = struct.pack("<IHHHH", MAGIC, VERSION, len(BONE_NAMES), FIRST_SLOT, 0)
    for b, name in enumerate(BONE_NAMES):
        slot = slot_of(b) if b in used else 0xFF
        px, py, pz = (int(round(c * 4096)) for c in PIVOTS[b])
        out += struct.pack("<bBhhhhh", PARENTS[b], slot, pitches[b],
                           int(round(lengths[b] * 4096)), px, py, pz)
    out += struct.pack("<%dI" % len(words), *words)
    return prims, bones, out


# ----------------------------------------------------------------------------------------
# Preview
# ----------------------------------------------------------------------------------------

COLORS = [(128, 128, 128), (220, 200, 60), (70, 140, 230), (230, 90, 90), (240, 150, 50),
          (80, 200, 120), (40, 120, 70), (200, 120, 230), (120, 60, 160)]


def rot_x(p, a):
    c, s = math.cos(a), math.sin(a)
    return (p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c)


def rot_y(p, a):
    c, s = math.cos(a), math.sin(a)
    return (p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c)


def posed(pos, bone, pose):
    """Applies a test pose: {bone: (pitch, yaw) radians}, children first, parents after."""
    p = tuple(c / 4096 for c in pos)
    b = bone
    while b >= 0:
        pitch, yaw = pose.get(b, (0.0, 0.0))
        pivot = PIVOTS[b]
        q = sub(p, pivot)
        # + pitch swings a hanging limb forward, toward -z: a rotation about +x
        q = rot_y(rot_x(q, pitch), yaw)
        p = tuple(a + c for a, c in zip(q, pivot))
        b = PARENTS[b]
    return p


def preview(prims, bones, path, pose_test):
    from PIL import Image, ImageDraw

    pose = {}
    if pose_test:
        pitches = rest_pitches()
        turn = 2 * math.pi / TURN
        # stand the legs up straight, then bend: a mid-stride with a lifted knee
        pose = {
            THIGH_L: (-pitches[THIGH_L] * turn - 0.5, 0.0),
            SHIN_L: (-(pitches[SHIN_L] - pitches[THIGH_L]) * turn - 0.9, 0.0),
            THIGH_R: (-pitches[THIGH_R] * turn + 0.45, 0.0),
            SHIN_R: (-(pitches[SHIN_R] - pitches[THIGH_R]) * turn, 0.0),
            HEAD: (0.0, 0.5), CHEST: (0.15, -0.3), ARMS: (-0.5, 0.0),
        }
    size = 320
    views = [((0, 1), 2, -1), ((2, 1), 0, 1), ((0, 2), 1, 1)]
    out = Image.new("RGB", (size * len(views), size), (24, 24, 24))
    for n, (axes, depth, sign) in enumerate(views):
        img = Image.new("RGB", (size, size), (24, 24, 24))
        d = ImageDraw.Draw(img)
        scale = size / 3.4
        faces = []
        for p in prims:
            k = 3 if p["type"] == 0 else 4
            for i in range(0, len(p["verts"]), k):
                corners = [posed(v[0], bones[v[0]], pose) for v in p["verts"][i:i + k]]
                owner = max(set(bones[v[0]] for v in p["verts"][i:i + k]),
                            key=lambda b: sum(bones[v[0]] == b for v in p["verts"][i:i + k]))
                faces.append((corners, owner))
        faces.sort(key=lambda f: sign * sum(c[depth] for c in f[0]))
        for corners, owner in faces:
            pts = [(size / 2 + c[axes[0]] * scale, size / 2 - (c[axes[1]] - 0.1) * scale) for c in corners]
            d.polygon(pts, fill=COLORS[owner], outline=(0, 0, 0))
        out.paste(img, (size * n, 0))
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", help="write a front/side/top view coloured by bone")
    ap.add_argument("--pose", action="store_true", help="bend the joints in the preview")
    ap.add_argument("--output", default=OUTPUT)
    args = ap.parse_args()
    prims, bones, blob = build()
    with open(args.output, "wb") as fh:
        fh.write(blob)
    counts = {name: sum(1 for b in bones.values() if b == i) for i, name in enumerate(BONE_NAMES)}
    print("%s: %d bytes, %d polygons, vertices per bone %s" % (
        os.path.relpath(args.output, ROOT), len(blob),
        sum(len(p["verts"]) // (3 if p["type"] == 0 else 4) for p in prims), counts))
    if args.preview:
        preview(prims, bones, args.preview, args.pose)


if __name__ == "__main__":
    main()
