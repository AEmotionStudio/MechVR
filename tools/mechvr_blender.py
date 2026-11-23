"""MechVR shapes for Blender: read and write assets/shapes.txt.

A shape (TORSO, ARM_L, CK_SEAT ...) is an Empty. A shape Empty parented to another shape Empty is
attached to it: where it sits is the joint (ARM_L on TORSO is the shoulder). Small labelled Empties
parented to a shape are its named points (MUZZLE, EYE ...). Its pieces are the mesh objects parented to it:
boxes, cylinders, spheres and capsules that add to or cut from the shape, in name order. Move, rotate
and scale them with the normal tools. Blender shows a close stand-in (cutters are wireframes and are
carved out of the pieces before them); the game draws the exact result. Run the game with
--shapes assets/shapes.txt and it redraws whenever the file is written.

In Blender:   Preferences > Add-ons > Install from Disk > this file. The panel is in the 3D view
              sidebar (N), tab "MechVR".
From a shell: blender -b --python tools/mechvr_blender.py -- import assets/shapes.txt --save assets/mechvr.blend
              blender -b assets/mechvr.blend --python tools/mechvr_blender.py -- export assets/shapes.txt
              python3 tools/mechvr_blender.py check assets/shapes.txt      (no Blender needed)

Axes: the game has +Y up and -Z forward. In Blender the mech stands +Z up and faces -Y (towards the
front view), with its left side on +X.
"""
import math
import os
import re
import sys

try:
    import bmesh
    import bpy
    from bpy.app.handlers import persistent
    from mathutils import Matrix, Quaternion, Vector
except ImportError:  # plain Python: only check / format work
    bpy = None

bl_info = {
    "name": "MechVR shapes",
    "author": "MechVR",
    "version": (1, 1, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Sidebar > MechVR",
    "description": "Edit the MechVR game's shapes and write assets/shapes.txt",
    "category": "Import-Export",
}

KINDS = ["box", "cyl", "sphere", "capsule"]
OPS = ["add", "sub", "smooth", "and"]
MATS = ["armor", "armor2", "frame", "accent", "glass", "glow", "screen", "rock", "hazard", "seat", "glove", "plasma", "cabin"]
MAX_PIECES = 16
COLORS = {  # roughly what the game paints each material (armor and armor2 take the team colours there)
    "armor": (0.44, 0.52, 0.30), "armor2": (0.84, 0.80, 0.66), "frame": (0.19, 0.20, 0.25), "accent": (0.95, 0.44, 0.10),
    "glass": (0.13, 0.36, 0.45), "glow": (1.0, 0.85, 0.35), "screen": (0.10, 0.55, 0.45), "rock": (0.62, 0.42, 0.30),
    "hazard": (0.95, 0.75, 0.10), "seat": (0.30, 0.27, 0.26), "glove": (0.93, 0.87, 0.70), "plasma": (0.35, 0.90, 1.0),
    "cabin": (0.46, 0.50, 0.52),
}
HEADER = """# MechVR shapes. Edit by hand, or in Blender with tools/mechvr_blender.py (it reads and writes this file).
# Units are metres, +Y up, -Z forward, +X right. Up to 16 pieces per shape; later pieces act on what came before.
#
# shape NAME at X Y Z on PARENT   the shape's origin is a joint at X Y Z on PARENT (ARM_L on TORSO: the shoulder).
#                                 Without 'on', 'at' is only where Blender shows the shape; the game ignores it.
# point NAME X Y Z                a named spot on the shape that the game looks up (weapon muzzle, pilot's eye ...)
# kind  centre x y z   half-size x y z   round   pitch yaw roll (degrees)   op k   material   # name
#   kind:      box, cyl (axis Y; half-size = radius, half-height, radius), sphere, capsule (radius, half-length, radius)
#   op k:      add 0, sub 0 (cut away), smooth K (blend over K metres), and 0 (keep only the overlap)
#   material:  armor armor2 frame accent glass glow screen rock hazard seat glove plasma cabin
"""


# ---- the text file ---------------------------------------------------------------------------------

def fmt(v, places=4):
    s = ("%.*f" % (places, v)).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def parse(text):
    """Returns (shapes, problems). A shape is a dict: name, at, on, note, points, pieces.
    A point is a dict: name, p, note. A piece is a dict: kind, c, h, round, rot, op, k, mat, name."""
    shapes, problems, cur = [], [], None
    for no, raw in enumerate(text.split("\n"), 1):
        line, _, note = raw.partition("#")
        note, tok = note.strip(), line.split()
        if not tok:
            continue
        if tok[0] == "shape" and len(tok) >= 2:
            at, on, rest = (0.0, 0.0, 0.0), "", tok[2:]
            if len(rest) >= 4 and rest[0] == "at":
                try:
                    at = tuple(float(x) for x in rest[1:4])
                except ValueError:
                    problems.append(f"line {no}: cannot read the position after 'at'")
                rest = rest[4:]
            if len(rest) == 2 and rest[0] == "on":
                on, rest = rest[1], []
            if rest:
                problems.append(f"line {no}: expected 'shape NAME at X Y Z on PARENT', cannot read '{' '.join(rest)}'")
            if any(s["name"] == tok[1] for s in shapes):
                problems.append(f"line {no}: shape {tok[1]} appears twice")
            cur = {"name": tok[1], "at": at, "on": on, "note": note, "points": [], "pieces": []}
            shapes.append(cur)
            continue
        if tok[0] == "point":
            try:
                pt = {"name": tok[1], "p": [float(x) for x in tok[2:5]], "note": note}
                if len(tok) != 5:
                    raise ValueError
            except (ValueError, IndexError):
                problems.append(f"line {no}: expected 'point NAME X Y Z'")
                continue
            if cur is None:
                problems.append(f"line {no}: point before the first 'shape' line")
            elif any(q["name"] == pt["name"] for q in cur["points"]):
                problems.append(f"line {no}: point {pt['name']} appears twice on {cur['name']}")
            else:
                cur["points"].append(pt)
            continue
        if len(tok) != 14:
            problems.append(f"line {no}: expected 14 values, found {len(tok)}")
            continue
        try:
            v = [float(x) for x in tok[1:11]] + [float(tok[12])]
        except ValueError:
            problems.append(f"line {no}: a number is not a number")
            continue
        bad = [w for w, ok in ((tok[0], KINDS), (tok[11], OPS), (tok[13], MATS)) if w not in ok]
        if bad:
            problems.append(f"line {no}: unknown {bad[0]}")
            continue
        if cur is None:
            problems.append(f"line {no}: piece before the first 'shape' line")
            continue
        if len(cur["pieces"]) == MAX_PIECES:
            problems.append(f"line {no}: {cur['name']} has more than {MAX_PIECES} pieces; the game skips the extra ones")
        if tok[0] in ("box", "cyl") and v[6] > min(v[3:6]) + 1e-6:
            problems.append(f"line {no}: round {fmt(v[6])} is larger than the smallest half-size")
        cur["pieces"].append({"kind": tok[0], "c": v[0:3], "h": v[3:6], "round": v[6], "rot": v[7:10],
                              "op": tok[11], "k": v[10], "mat": tok[13], "name": note})
    names = {s["name"]: s for s in shapes}
    for s in shapes:
        if not s["pieces"]:
            problems.append(f"shape {s['name']} has no pieces")
        if s["on"] and s["on"] not in names:
            problems.append(f"shape {s['name']} is on {s['on']}, which is not in the file")
        seen, up = {s["name"]}, s
        while up["on"] in names:  # a shape cannot be attached to itself, directly or through others
            up = names[up["on"]]
            if up["name"] in seen:
                problems.append(f"shape {s['name']} is attached in a loop")
                break
            seen.add(up["name"])
    return shapes, problems


def write(shapes):
    out = [HEADER.rstrip("\n")]
    for s in shapes:
        out.append("")
        out.append(f"shape {s['name']} at " + " ".join(fmt(x) for x in s["at"]) + (f" on {s['on']}" if s["on"] else "")
                   + (f"   # {s['note']}" if s["note"] else ""))
        for q in s["points"]:
            out.append((f"point {q['name']} " + " ".join(fmt(x) for x in q["p"])).ljust(37) + (f" # {q['note']}" if q["note"] else ""))
            out[-1] = out[-1].rstrip()
        for p in s["pieces"]:
            cols = [p["kind"].ljust(7), " ".join(fmt(x) for x in p["c"]).ljust(22), " ".join(fmt(x) for x in p["h"]).ljust(20),
                    fmt(p["round"]).ljust(6), " ".join(fmt(x, 2) for x in p["rot"]).ljust(10), (p["op"] + " " + fmt(p["k"])).ljust(10),
                    p["mat"].ljust(7)]
            out.append((" ".join(cols) + (" # " + p["name"] if p["name"] else "")).rstrip())
    return "\n".join(out) + "\n"


def save_text(path, text):  # replace the file in one step, so a running game never reads half of it
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


# ---- game <-> Blender axes -------------------------------------------------------------------------
# game (x, y, z) <-> Blender (-x, z, y). The same swap works in both directions.

def swap(v):
    return (-v[0], v[2], v[1])


def euler_to_quat(pitch, yaw, roll):  # game: yaw(Y) * pitch(X) * roll(Z), degrees -> (x, y, z, w)
    def axis(x, y, z, deg):
        a = math.radians(deg) * 0.5
        s = math.sin(a)
        return (x * s, y * s, z * s, math.cos(a))

    def mul(a, b):
        return (a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2])

    return mul(mul(axis(0, 1, 0, yaw), axis(1, 0, 0, pitch)), axis(0, 0, 1, roll))


def quat_to_euler(q):  # the reverse: (x, y, z, w) -> pitch, yaw, roll in degrees
    x, y, z, w = q
    r02, r12, r22 = 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)
    r10, r11 = 2 * (x * y + w * z), 1 - 2 * (x * x + z * z)
    r00, r20 = 1 - 2 * (y * y + z * z), 2 * (x * z - w * y)
    flat = math.hypot(r10, r11)  # cosine of the pitch
    pitch = math.atan2(-r12, flat)
    if flat < 1e-4:  # pointing straight up or down: yaw and roll turn about the same axis
        yaw, roll = math.atan2(-r20, r00), 0.0
    else:
        yaw, roll = math.atan2(r02, r22), math.atan2(r10, r11)
    return [round(math.degrees(a), 2) + 0.0 for a in (pitch, yaw, roll)]


# ---- everything below needs Blender ----------------------------------------------------------------
QUIET = False  # set while the command line saves a .blend, so that save does not also rewrite the shapes file

if bpy:
    PIECE_KINDS = {"BOX": "box", "CYL": "cyl", "SPHERE": "sphere", "CAPSULE": "capsule"}

    def material(name):
        m = bpy.data.materials.get("mv_" + name)
        if not m:
            m = bpy.data.materials.new("mv_" + name)
            m.diffuse_color = (*COLORS[name], 1.0)
        return m

    def lathe(profile, seg=32):
        """Mesh from a profile of (radius, height) points spun about Blender's Z axis."""
        verts, rings, faces = [], [], []
        for r, z in profile:
            if r < 1e-9:
                rings.append([len(verts)])
                verts.append((0.0, 0.0, z))
            else:
                rings.append(list(range(len(verts), len(verts) + seg)))
                verts += [(r * math.cos(2 * math.pi * i / seg), r * math.sin(2 * math.pi * i / seg), z) for i in range(seg)]
        for a, b in zip(rings, rings[1:]):
            for i in range(seg):
                j = (i + 1) % seg
                if len(a) == 1 and len(b) == 1:
                    continue
                if len(a) == 1:
                    faces.append((a[0], b[i], b[j]))
                elif len(b) == 1:
                    faces.append((a[i], a[j], b[0]))
                else:
                    faces.append((a[i], a[j], b[j], b[i]))
        return verts, faces

    def build_mesh(name, kind, h):
        """h is the game half-size (x, y, z); the mesh is built in Blender's axes at its real size."""
        hx, hy, hz = h
        smooth = True
        if kind == "box":
            x, y, z = hx, hz, hy
            verts = [(sx * x, sy * y, sz * z) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
            faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
            smooth = False
        elif kind == "cyl":
            verts, faces = lathe([(0, -hy), (hx, -hy), (hx, hy), (0, hy)])
        else:
            n = 8
            arc = [(hx * math.cos(math.pi / 2 * i / n), hx * math.sin(math.pi / 2 * i / n)) for i in range(n + 1)]
            off = hy if kind == "capsule" else 0.0
            lower = [(r, -z - off) for r, z in reversed(arc)]
            upper = [(r, z + off) for r, z in arc]
            verts, faces = lathe(lower + (upper if off else upper[1:]))
        me = bpy.data.meshes.new(name)
        me.from_pydata(verts, [], faces)
        bm = bmesh.new()
        bm.from_mesh(me)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(me)
        bm.free()
        me.update()
        if smooth:
            me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
            if kind == "cyl":  # keep the rims crisp
                for e in me.edges:
                    a, b = (me.vertices[i].co for i in e.vertices)
                    if abs(a.z - b.z) < 1e-9 and a.xy.length > hx * 0.99 and b.xy.length > hx * 0.99:
                        e.use_edge_sharp = True
        return me

    def refresh_shape(shape):
        """Rebuild the stand-in for one shape: rounding as a bevel, and every cutter carved out of the
        pieces that come before it (as the game does), so Blender shows roughly what the game draws."""
        ps = pieces_of(shape)
        for i, o in enumerate(ps):
            for m in [m for m in o.modifiers if m.name.startswith("mv ")]:
                o.modifiers.remove(m)
            p = o.mechvr
            cutter = p.op in ("SUB", "AND")
            o.display_type = "WIRE" if cutter else "TEXTURED"
            o.hide_render = cutter
            if p.round > 0 and p.kind in ("BOX", "CYL"):
                m = o.modifiers.new("mv round", "BEVEL")
                m.limit_method, m.segments, m.width = "ANGLE", 4, p.round
            if cutter:
                continue
            for c in ps[i + 1:]:
                if c.mechvr.op not in ("SUB", "AND"):
                    continue
                m = o.modifiers.new("mv cut", "BOOLEAN")
                m.operation = "DIFFERENCE" if c.mechvr.op == "SUB" else "INTERSECT"
                m.object = c
                try:  # the cut surface takes the cutter's material, as in the game
                    m.solver = "EXACT"
                    m.material_mode = "TRANSFER"
                except (AttributeError, TypeError):
                    pass

    def refresh(o):
        shape = shape_of(o)
        if shape and o.mechvr.kind in PIECE_KINDS:
            refresh_shape(shape)

    def _update(self, context):
        refresh(self.id_data)

    def _mat_get(self):
        m = self.id_data.active_material
        n = m.name[3:].split(".")[0] if m and m.name.startswith("mv_") else "armor"
        return MATS.index(n) if n in MATS else 0

    def _mat_set(self, value):
        o = self.id_data
        if o.type == "MESH":
            o.data.materials.clear()
            o.data.materials.append(material(MATS[value]))

    class MechVRProps(bpy.types.PropertyGroup):
        kind: bpy.props.EnumProperty(name="Kind", items=[(k, k.title(), "") for k in ["NONE", "SHAPE", "BOX", "CYL", "SPHERE", "CAPSULE", "POINT"]])
        op: bpy.props.EnumProperty(name="Does", update=_update, items=[
            ("ADD", "Add", "Adds to the shape"), ("SUB", "Cut", "Cuts away from what came before"),
            ("SMOOTH", "Blend", "Adds with a soft join"), ("AND", "Overlap", "Keeps only the part that overlaps what came before")])
        k: bpy.props.FloatProperty(name="Blend width", min=0.0, soft_max=2.0, unit="LENGTH")
        round: bpy.props.FloatProperty(name="Round", min=0.0, soft_max=1.0, unit="LENGTH", update=_update,
                                       description="Corner radius. Must not exceed the smallest half-size")
        mat: bpy.props.EnumProperty(name="Material", items=[(m, m, "") for m in MATS], get=_mat_get, set=_mat_set)
        note: bpy.props.StringProperty(name="Note")
        order: bpy.props.IntProperty(name="Order")

    def shape_of(o):
        if o and o.mechvr.kind == "SHAPE":
            return o
        if o and o.parent and o.parent.mechvr.kind == "SHAPE":
            return o.parent
        return None

    def points_of(shape):
        return sorted((c for c in shape.children if c.mechvr.kind == "POINT"), key=lambda c: (c.mechvr.order, c.name))

    def parent_shape(shape):
        return shape.parent if shape.parent and shape.parent.mechvr.kind == "SHAPE" else None

    def pieces_of(shape):
        return sorted((c for c in shape.children if c.mechvr.kind in PIECE_KINDS and c.type == "MESH"), key=lambda c: c.name)

    def root_collection(scene):
        col = bpy.data.collections.get("MechVR")
        if not col:
            col = bpy.data.collections.new("MechVR")
        if col.name not in scene.collection.children:
            scene.collection.children.link(col)
        return col

    def new_shape(scene, name, at, note="", order=0):
        col = bpy.data.collections.new(name)
        root_collection(scene).children.link(col)
        e = bpy.data.objects.new(name, None)
        e.empty_display_type = "PLAIN_AXES"
        e.empty_display_size = 0.3
        e.location = at
        e.mechvr.kind, e.mechvr.note, e.mechvr.order = "SHAPE", note, order
        col.objects.link(e)
        return e

    def new_point(shape, name, at, note="", order=0, size=0.1):
        o = bpy.data.objects.new(f"{shape.name}.{name}"[:63], None)
        o.empty_display_type = "SPHERE"
        o.empty_display_size = size
        o.show_name = o.show_in_front = True
        o.parent = shape
        o.location = at
        o.mechvr.kind, o.mechvr.note, o.mechvr.order = "POINT", note, order
        (shape.users_collection[0] if shape.users_collection else bpy.context.scene.collection).objects.link(o)
        return o

    def new_piece(shape, index, p):
        label = f"{shape.name}.{index:02d}" + (" " + p["name"] if p["name"] else "")
        o = bpy.data.objects.new(label[:63], build_mesh(label[:63], p["kind"], p["h"]))
        o.parent = shape
        o.location = swap(p["c"])
        x, y, z, w = euler_to_quat(*p["rot"])
        o.rotation_euler = Quaternion((w, -x, z, y)).to_euler("XYZ")
        o.data.materials.append(material(p["mat"]))
        m = o.mechvr
        m.kind, m.k = p["kind"].upper(), p["k"]
        m.op = {"add": "ADD", "sub": "SUB", "smooth": "SMOOTH", "and": "AND"}[p["op"]]
        m.round = p["round"]
        (shape.users_collection[0] if shape.users_collection else bpy.context.scene.collection).objects.link(o)
        return o

    def clear_shapes():
        for o in [o for o in bpy.data.objects if o.mechvr.kind != "NONE"]:
            me = o.data if o.type == "MESH" else None
            bpy.data.objects.remove(o)
            if me and not me.users:
                bpy.data.meshes.remove(me)
        root = bpy.data.collections.get("MechVR")
        if root:
            for c in list(root.children):
                if not c.all_objects:
                    bpy.data.collections.remove(c)

    def import_shapes(path, scene, only_new=False):
        """Rebuild the scene's shapes from the file. only_new: leave the scene's shapes alone and add
        the ones it does not have yet."""
        with open(path) as f:
            shapes, problems = parse(f.read())
        made, added = {}, 0
        if only_new:
            made = {o.name: o for o in scene.objects if o.mechvr.kind == "SHAPE"}
        else:
            clear_shapes()
        for n, s in enumerate(shapes):
            if s["name"] in made:
                s["on"] = ""  # already in the scene: keep it as it is
                continue
            added += 1
            e = made[s["name"]] = new_shape(scene, s["name"], swap(s["at"]), s["note"], n)
            for i, p in enumerate(s["pieces"]):
                new_piece(e, i, p)
            big = max((max(p["h"]) for p in s["pieces"]), default=1.0)
            for i, q in enumerate(s["points"]):
                new_point(e, q["name"], swap(q["p"]), q["note"], i, min(max(big * 0.08, 0.008), 0.12))
            refresh_shape(e)
        for s in shapes:  # attach shapes to the shapes they are on; their position is then the joint
            if s["on"] in made and s["on"] != s["name"]:
                made[s["name"]].parent = made[s["on"]]
        return added, problems

    def gather(scene):
        bpy.context.view_layer.update()
        shapes, problems = [], []
        for e in sorted((o for o in scene.objects if o.mechvr.kind == "SHAPE"), key=lambda o: (o.mechvr.order, o.name)):
            up = parent_shape(e)
            at = (up.matrix_world.inverted() @ e.matrix_world).translation if up else e.matrix_world.translation
            s = {"name": e.name, "at": swap(at), "on": up.name if up else "", "note": e.mechvr.note, "points": [], "pieces": []}
            inv = e.matrix_world.inverted()
            for o in points_of(e):
                name = re.sub(r"[^A-Z0-9_]", "_", o.name.split(".", 1)[-1].upper())
                if any(q["name"] == name for q in s["points"]):
                    problems.append(f"{e.name} has two points called {name}; the second is left out")
                    continue
                s["points"].append({"name": name, "p": swap((inv @ o.matrix_world).translation), "note": o.mechvr.note})
            for o in pieces_of(e):
                co = [v.co for v in o.data.vertices]
                if not co:
                    continue
                lo = Vector(tuple(min(c[i] for c in co) for i in range(3)))
                hi = Vector(tuple(max(c[i] for c in co) for i in range(3)))
                rel = inv @ o.matrix_world
                loc, rot, scale = rel.decompose()
                half = [abs((hi[i] - lo[i]) * 0.5 * scale[i]) for i in range(3)]
                centre = rel @ ((lo + hi) * 0.5)
                kind = PIECE_KINDS[o.mechvr.kind]
                if kind == "box":
                    h = [half[0], half[2], half[1]]
                elif kind == "cyl":
                    h = [half[0], half[2], half[0]]
                elif kind == "sphere":
                    h = [half[0]] * 3
                else:
                    h = [half[0], max(half[2] - half[0], 0.0), half[0]]
                m = o.mechvr
                rnd = m.round if kind in ("box", "cyl") else 0.0
                if rnd > min(h) + 1e-6:
                    problems.append(f"{o.name}: round is larger than its smallest half-size; using {fmt(min(h))}")
                    rnd = min(h)
                name = re.sub(r"^\S*\.\d+\s*", "", o.name) if re.match(r"^\S*\.\d+(\s|$)", o.name) else o.name
                s["pieces"].append({"kind": kind, "c": swap(centre), "h": h, "round": rnd,
                                    "rot": quat_to_euler((-rot.x, rot.z, rot.y, rot.w)), "op": m.op.lower(),
                                    "k": m.k if m.op == "SMOOTH" else 0.0, "mat": MATS[_mat_get(m)], "name": name.replace("#", "")})
            if len(s["pieces"]) > MAX_PIECES:
                problems.append(f"{e.name} has {len(s['pieces'])} pieces; the game uses the first {MAX_PIECES}")
            if not s["pieces"]:
                problems.append(f"{e.name} has no pieces")
            shapes.append(s)
        return shapes, problems

    def export_shapes(path, scene):
        """Write the scene's shapes. Shapes that are in the file but not in this scene are kept, so a
        scene holding only some of the shapes never deletes the others."""
        shapes, problems = gather(scene)
        mine = {s["name"]: s for s in shapes}
        try:
            with open(path) as f:
                old = parse(f.read())[0]
        except OSError:
            old = []
        kept = [s["name"] for s in old if s["name"] not in mine]
        merged = [mine.pop(s["name"], s) for s in old] + [s for s in shapes if s["name"] in mine]
        if kept:
            problems.append(f"kept {len(kept)} shapes that are in the file but not in this scene: {', '.join(kept)}")
        if shapes:
            save_text(path, write(merged))
        return len(shapes), problems

    def file_path(scene):
        return bpy.path.abspath(scene.mechvr_file)

    class MECHVR_OT_import(bpy.types.Operator):
        """Replace the MechVR shapes in this scene with the ones in the shapes file"""
        bl_idname = "mechvr.import_shapes"
        bl_label = "Read shapes file"
        bl_options = {"REGISTER", "UNDO"}

        def execute(self, context):
            try:
                n, problems = import_shapes(file_path(context.scene), context.scene)
            except OSError as e:
                self.report({"ERROR"}, str(e))
                return {"CANCELLED"}
            for p in problems:
                self.report({"WARNING"}, p)
            self.report({"INFO"}, f"Read {n} shapes")
            return {"FINISHED"}

        def invoke(self, context, event):
            if any(o.mechvr.kind == "SHAPE" for o in context.scene.objects):
                return context.window_manager.invoke_confirm(self, event)
            return self.execute(context)

    class MECHVR_OT_import_new(bpy.types.Operator):
        """Add the shapes from the shapes file that this scene does not have yet. Nothing already in the scene changes"""
        bl_idname = "mechvr.import_new"
        bl_label = "Read new shapes"
        bl_options = {"REGISTER", "UNDO"}

        def execute(self, context):
            try:
                n, problems = import_shapes(file_path(context.scene), context.scene, only_new=True)
            except OSError as e:
                self.report({"ERROR"}, str(e))
                return {"CANCELLED"}
            for p in problems:
                self.report({"WARNING"}, p)
            self.report({"INFO"}, f"Added {n} new shapes")
            return {"FINISHED"}

    class MECHVR_OT_export(bpy.types.Operator):
        """Write every MechVR shape in this scene to the shapes file"""
        bl_idname = "mechvr.export_shapes"
        bl_label = "Write shapes file"

        def execute(self, context):
            try:
                n, problems = export_shapes(file_path(context.scene), context.scene)
            except OSError as e:
                self.report({"ERROR"}, str(e))
                return {"CANCELLED"}
            for p in problems:
                self.report({"WARNING"}, p)
            self.report({"INFO"}, f"Wrote {n} shapes to {file_path(context.scene)}")
            return {"FINISHED"}

    class MECHVR_OT_add_shape(bpy.types.Operator):
        """Add an empty shape at the 3D cursor. The game only draws names it knows (SHAPE_LIST in src/mech.h)"""
        bl_idname = "mechvr.add_shape"
        bl_label = "New shape"
        bl_options = {"REGISTER", "UNDO"}
        name: bpy.props.StringProperty(name="Name", default="NEW_SHAPE")

        def execute(self, context):
            name = re.sub(r"[^A-Z0-9_]", "_", self.name.upper())
            order = max([o.mechvr.order for o in context.scene.objects if o.mechvr.kind == "SHAPE"], default=-1) + 1
            e = new_shape(context.scene, name, context.scene.cursor.location, "", order)
            context.view_layer.objects.active = e
            return {"FINISHED"}

        def invoke(self, context, event):
            return context.window_manager.invoke_props_dialog(self)

    class MECHVR_OT_add_point(bpy.types.Operator):
        """Add a named point to the active shape, at the 3D cursor. The game looks points up by name (POINT_LIST in src/mech.h)"""
        bl_idname = "mechvr.add_point"
        bl_label = "New point"
        bl_options = {"REGISTER", "UNDO"}
        name: bpy.props.StringProperty(name="Name", default="POINT")

        @classmethod
        def poll(cls, context):
            return shape_of(context.active_object) is not None

        def execute(self, context):
            shape = shape_of(context.active_object)
            order = max([o.mechvr.order for o in points_of(shape)], default=-1) + 1
            at = shape.matrix_world.inverted() @ context.scene.cursor.location
            o = new_point(shape, re.sub(r"[^A-Z0-9_]", "_", self.name.upper()), at, "", order)
            for x in context.selected_objects:
                x.select_set(False)
            o.select_set(True)
            context.view_layer.objects.active = o
            return {"FINISHED"}

        def invoke(self, context, event):
            return context.window_manager.invoke_props_dialog(self)

    class MECHVR_OT_add_piece(bpy.types.Operator):
        """Add a piece to the active shape, at the shape's origin"""
        bl_idname = "mechvr.add_piece"
        bl_label = "Add piece"
        bl_options = {"REGISTER", "UNDO"}
        kind: bpy.props.StringProperty(default="box")
        size: bpy.props.FloatProperty(name="Size", default=0.25, min=0.001, unit="LENGTH")

        @classmethod
        def poll(cls, context):
            return shape_of(context.active_object) is not None

        def execute(self, context):
            shape = shape_of(context.active_object)
            used = [int(m.group(1)) for m in (re.match(r"^\S*\.(\d+)", c.name) for c in pieces_of(shape)) if m]
            s = self.size
            h = {"box": [s, s, s], "cyl": [s, s, s], "sphere": [s, s, s], "capsule": [s * 0.5, s, s * 0.5]}[self.kind]
            o = new_piece(shape, max(used, default=-1) + 1,
                          {"kind": self.kind, "c": (0, 0, 0), "h": h, "round": 0.0, "rot": (0, 0, 0), "op": "add", "k": 0.0, "mat": "armor", "name": self.kind})
            refresh_shape(shape)
            for x in context.selected_objects:
                x.select_set(False)
            o.select_set(True)
            context.view_layer.objects.active = o
            return {"FINISHED"}

    class MECHVR_OT_refresh(bpy.types.Operator):
        """Rebuild Blender's stand-in for the active shape. Use after renaming pieces, which changes their order"""
        bl_idname = "mechvr.refresh"
        bl_label = "Refresh preview"
        bl_options = {"REGISTER", "UNDO"}

        @classmethod
        def poll(cls, context):
            return shape_of(context.active_object) is not None

        def execute(self, context):
            refresh_shape(shape_of(context.active_object))
            return {"FINISHED"}

    class MECHVR_PT_panel(bpy.types.Panel):
        bl_label = "MechVR shapes"
        bl_space_type = "VIEW_3D"
        bl_region_type = "UI"
        bl_category = "MechVR"

        def draw(self, context):
            lay, scene, o = self.layout, context.scene, context.active_object
            lay.prop(scene, "mechvr_file", text="")
            row = lay.row(align=True)
            row.operator("mechvr.import_shapes", text="Read all", icon="IMPORT")
            row.operator("mechvr.export_shapes", text="Write", icon="EXPORT")
            lay.operator("mechvr.import_new", icon="ADD")
            lay.prop(scene, "mechvr_on_save")
            lay.separator()
            shape = shape_of(o)
            lay.operator("mechvr.add_shape", icon="EMPTY_AXIS")
            if not shape:
                lay.label(text="Select a shape or one of its pieces")
                return
            n = len(pieces_of(shape))
            lay.label(text=f"{shape.name}: {n} of {MAX_PIECES} pieces", icon="ERROR" if n > MAX_PIECES else "NONE")
            up = parent_shape(shape)
            lay.label(text=f"Attached to {up.name}: its position is the joint" if up else "Not attached: its position is not used by the game")
            lay.prop(shape.mechvr, "note")
            lay.operator("mechvr.add_point", icon="EMPTY_DATA")
            row = lay.row(align=True)
            for kind, label in (("box", "Box"), ("cyl", "Cylinder"), ("sphere", "Sphere"), ("capsule", "Capsule")):
                row.operator("mechvr.add_piece", text=label).kind = kind
            lay.operator("mechvr.refresh", icon="FILE_REFRESH")
            if o is not shape and o.mechvr.kind == "POINT":
                box = lay.box()
                box.label(text=o.name + " (point)")
                box.prop(o.mechvr, "note")
            if o is shape or o.mechvr.kind not in PIECE_KINDS:
                return
            lay.separator()
            box = lay.box()
            box.label(text=o.name)
            box.prop(o.mechvr, "op")
            if o.mechvr.op == "SMOOTH":
                box.prop(o.mechvr, "k")
            if o.mechvr.kind in ("BOX", "CYL"):
                box.prop(o.mechvr, "round")
            box.prop(o.mechvr, "mat")
            box.label(text="Pieces apply in name order (.00, .01 ...)")

    @persistent
    def _on_save(*_):
        if QUIET:
            return
        for scene in bpy.data.scenes:
            if scene.mechvr_on_save and any(o.mechvr.kind == "SHAPE" for o in scene.objects):
                try:
                    n, problems = export_shapes(file_path(scene), scene)
                    print(f"MechVR: wrote {n} shapes to {file_path(scene)}")
                    for p in problems:
                        print("MechVR:", p)
                except OSError as e:
                    print("MechVR: could not write the shapes file:", e)

    CLASSES = (MechVRProps, MECHVR_OT_import, MECHVR_OT_import_new, MECHVR_OT_export, MECHVR_OT_add_shape, MECHVR_OT_add_point, MECHVR_OT_add_piece, MECHVR_OT_refresh, MECHVR_PT_panel)

    def register():
        for c in CLASSES:
            bpy.utils.register_class(c)
        bpy.types.Object.mechvr = bpy.props.PointerProperty(type=MechVRProps)
        path = dict(name="Shapes file", subtype="FILE_PATH", default="//shapes.txt")
        try:  # newer Blender wants to be told that "//" (next to the .blend) is allowed
            bpy.types.Scene.mechvr_file = bpy.props.StringProperty(options={"PATH_SUPPORTS_BLEND_RELATIVE"}, **path)
        except TypeError:
            bpy.types.Scene.mechvr_file = bpy.props.StringProperty(**path)
        bpy.types.Scene.mechvr_on_save = bpy.props.BoolProperty(
            name="Write when the .blend is saved", default=True,
            description="Saving this .blend also writes the shapes file, so a game started with --shapes updates at once")
        if _on_save not in bpy.app.handlers.save_post:
            bpy.app.handlers.save_post.append(_on_save)

    def unregister():
        if _on_save in bpy.app.handlers.save_post:
            bpy.app.handlers.save_post.remove(_on_save)
        del bpy.types.Scene.mechvr_on_save
        del bpy.types.Scene.mechvr_file
        del bpy.types.Object.mechvr
        for c in reversed(CLASSES):
            bpy.utils.unregister_class(c)


# ---- command line ----------------------------------------------------------------------------------

def main(argv):
    if len(argv) < 2 or argv[0] not in ("check", "format", "import", "export"):
        print(__doc__)
        return 2
    cmd, path = argv[0], argv[1]
    if cmd in ("check", "format"):
        with open(path) as f:
            shapes, problems = parse(f.read())
        for p in problems:
            print(f"{path}: {p}")
        if cmd == "format" and not problems:
            save_text(path, write(shapes))
        print(f"{path}: {len(shapes)} shapes, {sum(len(s['pieces']) for s in shapes)} pieces, "
              f"{sum(len(s['points']) for s in shapes)} points, {len(problems)} problems")
        return 1 if problems else 0
    if not bpy:
        print("import and export run inside Blender: blender -b --python tools/mechvr_blender.py -- " + " ".join(argv))
        return 2
    if cmd == "import":
        bpy.ops.wm.read_factory_settings(use_empty=True)
        register_once()
        scene = bpy.context.scene
        n, problems = import_shapes(path, scene)
        if "--save" in argv:
            blend = os.path.abspath(argv[argv.index("--save") + 1])
            scene.mechvr_file = "//" + os.path.relpath(os.path.abspath(path), os.path.dirname(blend))
            global QUIET
            QUIET = True
            bpy.ops.wm.save_as_mainfile(filepath=blend)
            QUIET = False
    else:
        register_once()
        n, problems = export_shapes(path, bpy.context.scene)
    for p in problems:
        print("MechVR:", p)
    print(f"MechVR: {cmd} {n} shapes, {path}")
    return 0 if n else 1


def register_once():
    if not hasattr(bpy.types.Object, "mechvr"):
        register()


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else ([] if bpy else sys.argv[1:])
    if bpy:
        register_once()
    if args:
        sys.exit(main(args))
