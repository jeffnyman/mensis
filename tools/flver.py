# SPDX-License-Identifier: GPL-3.0-or-later
"""FLVER models: read a character's mesh, export it as OBJ with its textures, render a picture.

    python -I tools/flver.py info   <chrbnd|partsbnd|objbnd|flver>
    python -I tools/flver.py obj    <archive> <out dir>      # .obj + .mtl + the textures as PNG
    python -I tools/flver.py render <archive> <out.png> [--size N] [--yaw DEGREES] [--pitch DEGREES]
        # yaw 180 (the default) shows a character from the front
    python -I tools/flver.py all    <base dvdroot> <update dvdroot> <out dir> [--size N]
        # every character model rendered, with an index
    python -I tools/flver.py parts  <base dvdroot> <update dvdroot> <out dir> [--size N]
        # every piece of equipment rendered on its own, named from the item tables
    python -I tools/flver.py hunter  <base dvdroot> <update dvdroot> <out.png> <set> [--sex m|f] [--size N]
        # a whole hunter in one armor set: <set> is its model number or any piece's name
    python -I tools/flver.py hunters <base dvdroot> <update dvdroot> <out dir> [--size N]
        # every armor set as a whole hunter, male and female, with an index

Output belongs under work/ (gitignored): the models are the game's own.

FLVER ("FLVER\\0", "L\\0" little-endian, version 0x20014 here) is FromSoftware's model format.
Layout as read here (all offsets from the start of the file; geometry from the data offset):

    0x00 header (0x80): version, data offset/length, counts of dummies, materials, bones, meshes,
         vertex buffers; bounding box; face counts; vertex index size (0 = per face set);
         unicode flag; counts of face sets, buffer layouts, textures
    then, packed in order:
    dummies       0x40 each   attachment points (not used here)
    materials     0x20 each   name, MTD shader path, texture count + first texture index
    bones         0x80 each   translation, name, rotation (Euler, radians), parent / child /
                              sibling indices, scale, bounding box
    meshes        0x30 each   dynamic flag, material index, default bone, bone count + offset,
                              face set count + offset, vertex buffer count + offset
    face sets     0x20 each   flags (LOD / motion blur), triangle strip?, cull backfaces?,
                              index count, index offset, index length, index size (16 or 32)
    vertex bufs   0x20 each   buffer index, layout index, vertex size, vertex count, length, offset
    buffer layouts 0x10 each  member count + offset to members (0x14 each: struct offset,
                              type, semantic, index)
    textures      0x20 each   path, type ("g_Diffuse", ...), scale

Vertex member types decoded: 0x02 float3, 0x03 float4, 0x10/0x11/0x13/0x2F 4 bytes,
0x15 two shorts (UV), 0x16 four shorts (two UVs), 0x18 four shorts (bone indices),
0x1A four signed shorts. Semantics: 0 position, 1 bone weights, 2 bone indices, 3 normal,
5 UV, 6 tangent, 7 bitangent, 10 color.
"""

from __future__ import annotations

import io
import math
import os
import struct
import sys
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

SEM = {
    0: "position",
    1: "weights",
    2: "bone indices",
    3: "normal",
    5: "uv",
    6: "tangent",
    7: "bitangent",
    10: "color",
}
SIZE = {
    0x00: 4,
    0x01: 8,
    0x02: 12,
    0x03: 16,
    0x10: 4,
    0x11: 4,
    0x12: 4,
    0x13: 4,
    0x15: 4,
    0x16: 8,
    0x18: 8,
    0x1A: 8,
    0x2E: 8,
    0x2F: 4,
}


def _wstr(d: bytes, off: int, unicode: bool) -> str:
    if off <= 0:
        return ""
    if unicode:
        end = off
        while d[end : end + 2] != b"\0\0":
            end += 2
        return d[off:end].decode("utf-16-le")
    return d[off : d.index(b"\0", off)].decode("shift_jis", "replace")


class Flver:
    def __init__(self, d: bytes):
        if d[:6] != b"FLVER\0" or d[6:8] != b"L\0":
            raise ValueError("not a little-endian FLVER")
        self.d = d
        (self.version, self.data_off, _dlen, n_dum, n_mat, n_bone, n_mesh, n_vb) = (
            struct.unpack_from("<8i", d, 8)
        )
        self.bbox = struct.unpack_from("<6f", d, 0x28)
        self.index_size = d[0x48]
        uni = bool(d[0x49])
        n_fs, n_lay, n_tex = struct.unpack_from("<3i", d, 0x50)
        p = 0x80 + n_dum * 0x40
        self.materials: list[dict[str, Any]] = []
        for i in range(n_mat):
            name, mtd, tcount, tfirst, flags = struct.unpack_from(
                "<5i", d, p + i * 0x20
            )
            self.materials.append(
                {
                    "name": _wstr(d, name, uni),
                    "mtd": _wstr(d, mtd, uni),
                    "tex": (tfirst, tcount),
                }
            )
        p += n_mat * 0x20
        self.bones: list[dict[str, Any]] = []
        for i in range(n_bone):
            o = p + i * 0x80
            t = struct.unpack_from("<3f", d, o)
            name = _wstr(d, struct.unpack_from("<i", d, o + 12)[0], uni)
            r = struct.unpack_from("<3f", d, o + 16)
            parent, _child = struct.unpack_from("<hh", d, o + 28)
            s = struct.unpack_from("<3f", d, o + 32)
            self.bones.append({"name": name, "t": t, "r": r, "s": s, "parent": parent})
        p += n_bone * 0x80
        self.meshes: list[dict[str, Any]] = []
        for i in range(n_mesh):
            o = p + i * 0x30
            dyn = d[o]
            mat, _z1, _z2, dbone, nb, _bbo, bo, nfs, fso, nvb, vbo = struct.unpack_from(
                "<11i", d, o + 4
            )
            self.meshes.append(
                {
                    "dynamic": dyn,
                    "material": mat,
                    "default_bone": dbone,
                    "bones": list(struct.unpack_from(f"<{nb}i", d, bo)) if nb else [],
                    "facesets": list(struct.unpack_from(f"<{nfs}i", d, fso)),
                    "vbufs": list(struct.unpack_from(f"<{nvb}i", d, vbo)),
                }
            )
        p += n_mesh * 0x30
        self.facesets: list[dict[str, Any]] = []
        for i in range(n_fs):
            o = p + i * 0x20
            flags, strip, _cull = struct.unpack_from("<IBB", d, o)
            count, ioff, _ilen, _z, isize = struct.unpack_from("<5i", d, o + 8)
            self.facesets.append(
                {
                    "flags": flags,
                    "strip": bool(strip),
                    "count": count,
                    "offset": ioff,
                    "isize": isize or self.index_size,
                }
            )
        p += n_fs * 0x20
        self.vbufs: list[dict[str, Any]] = []
        for i in range(n_vb):
            _bidx, lay, vsize, vcount, _a, _b, _blen, boff = struct.unpack_from(
                "<8i", d, p + i * 0x20
            )
            self.vbufs.append(
                {"layout": lay, "vsize": vsize, "count": vcount, "offset": boff}
            )
        p += n_vb * 0x20
        self.layouts: list[list[dict[str, Any]]] = []
        for i in range(n_lay):
            mcount, _a, _b, moff = struct.unpack_from("<4i", d, p + i * 0x10)
            members = []
            for k in range(mcount):
                _u, soff, typ, sem, idx = struct.unpack_from(
                    "<iiIIi", d, moff + k * 0x14
                )
                members.append({"offset": soff, "type": typ, "sem": sem, "index": idx})
            self.layouts.append(members)
        p += n_lay * 0x10
        self.textures: list[dict[str, Any]] = []
        for i in range(n_tex):
            path, typ = struct.unpack_from("<ii", d, p + i * 0x20)
            self.textures.append(
                {"path": _wstr(d, path, uni), "type": _wstr(d, typ, uni)}
            )

    # -- geometry ---------------------------------------------------------------------------
    def indices(self, fs: dict) -> list[tuple[int, int, int]]:
        """Triangles of a face set (strips unrolled, 0xFFFF/0xFFFFFFFF restarts skipped)."""
        size = 4 if fs["isize"] == 32 else 2
        fmt = f"<{fs['count']}{'I' if size == 4 else 'H'}"
        idx = struct.unpack_from(fmt, self.d, self.data_off + fs["offset"])
        restart = 0xFFFFFFFF if size == 4 else 0xFFFF
        tris = []
        if not fs["strip"]:
            for i in range(0, len(idx) - 2, 3):
                tris.append((idx[i], idx[i + 1], idx[i + 2]))
            return tris
        flip = False
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if restart in (a, b, c):
                flip = False
                continue
            if a != b and b != c and a != c:
                tris.append((a, c, b) if flip else (a, b, c))
            flip = not flip
        return tris

    def vertices(self, mesh: dict) -> dict[str, list]:
        """Positions, normals, first UV and bone indices of a mesh, merged across its buffers."""
        out: dict[str, list] = {}
        for vbi in mesh["vbufs"]:
            vb = self.vbufs[vbi]
            base = self.data_off + vb["offset"]
            for m in self.layouts[vb["layout"]]:
                sem, typ = m["sem"], m["type"]
                key = {0: "pos", 3: "normal", 5: "uv", 2: "bones"}.get(sem)
                if key is None or key in out or (key == "uv" and m["index"] != 0):
                    continue
                vals = []
                for v in range(vb["count"]):
                    o = base + v * vb["vsize"] + m["offset"]
                    vals.append(self._decode(typ, sem, o))
                out[key] = vals
        return out

    def _decode(self, typ: int, sem: int, o: int):
        d = self.d
        if typ == 0x02:
            return struct.unpack_from("<3f", d, o)
        if typ == 0x03:
            return struct.unpack_from("<4f", d, o)[:3]
        if typ in (0x15, 0x16):  # UV(s) as shorts / 2048
            u, v = struct.unpack_from("<2h", d, o)
            return (u / 2048.0, v / 2048.0)
        if typ == 0x18:
            return struct.unpack_from("<4h", d, o)
        if typ == 0x1A:
            return tuple(x / 32767.0 for x in struct.unpack_from("<4h", d, o)[:3])
        if typ in (0x10, 0x11, 0x13, 0x2F):
            b = d[o : o + 4]
            if sem == 2:
                return tuple(b)
            return tuple((x - 127) / 127.0 for x in b[:3])
        raise ValueError(f"vertex member type {typ:#x} (semantic {sem}) not handled")

    def lod0(self, mesh: dict) -> dict:
        """The full-detail face set: the first with no LOD or motion-blur flag."""
        sets = [self.facesets[i] for i in mesh["facesets"]]
        for fs in sets:
            if fs["flags"] & 0xFF000000 == 0:
                return fs
        return sets[0]


def load(path: str) -> tuple[Flver, list]:
    """The FLVER in an archive (or a bare .flver), and the archive's TPF textures."""
    data = souls.load(path)
    if data[:6] == b"FLVER\0":
        return Flver(data), []
    files = souls.bnd4_read(data)
    flv = next((f for f in files if f.data[:6] == b"FLVER\0"), None)
    if flv is None:
        raise ValueError("no FLVER in the archive")
    import glob

    import textures as tex

    texs = [t for f in files if f.data[:4] == b"TPF\0" for t in tex.read_tpf(f.data)]
    # some characters keep more textures beside the archive: chr/c5100_2.tpf.dcx for c5100
    stem = os.path.basename(path).split(".")[0]
    for extra in sorted(
        glob.glob(os.path.join(os.path.dirname(path), stem + "_*.tpf.dcx"))
    ):
        texs += tex.read_tpf(souls.load(extra))
    return Flver(flv.data), texs


class TextureIndex:
    """Texture name -> the archive holding it, across chr/, parts/ and obj/. 43 of the 217
    character models with meshes keep none of their color textures in their own archive: they
    use textures from texture-only archives (c4029, c4059, ...), from equipment (BD_...) or from
    objects. Names are read once; archives are decoded only when a texture is needed."""

    def __init__(self, base: str, kinds=("chr", "parts", "obj", "other")):
        import glob

        import textures as tex

        self.where: dict[
            str, str
        ] = {}  # lower-case name -> archive (materials' case varies)
        self._cache: dict[str, dict] = {}
        pats = {
            "chr": ["c*.chrbnd.dcx", "c*_*.tpf.dcx"],
            "parts": ["*.partsbnd.dcx", "*.tpf.dcx"],
            "obj": ["o*.objbnd.dcx"],
            "other": ["*.tpf.dcx"],
        }
        for k in kinds:
            for pat in pats[k]:
                for path in sorted(glob.glob(os.path.join(base, k, pat))):
                    try:
                        for t in tex.textures_in(path):
                            self.where.setdefault(t.name.lower(), path)
                    except ValueError:
                        pass

    def get(self, name: str):
        path = self.where.get(name.lower())
        if not path:
            return None
        if path not in self._cache:
            import textures as tex

            self._cache = {
                path: {t.name.lower(): t for t in tex.textures_in(path)}
            }  # keep one archive
        return self._cache[path].get(name.lower())


def info(path: str) -> None:
    f, texs = load(path)
    print(f"FLVER {f.version:#x}  bbox {tuple(round(x, 3) for x in f.bbox)}")
    print(
        f"{len(f.bones)} bones, {len(f.materials)} materials, {len(f.meshes)} meshes, "
        f"{len(f.facesets)} face sets, {len(f.vbufs)} vertex buffers, {len(f.layouts)} layouts, "
        f"{len(f.textures)} texture slots; {len(texs)} textures in the archive"
    )
    for i, lay in enumerate(f.layouts):
        print(
            f"  layout {i}: "
            + ", ".join(
                f"{SEM.get(m['sem'], m['sem'])}@{m['offset']}:{m['type']:#x}"
                for m in lay
            )
        )
    for i, m in enumerate(f.meshes):
        mat = f.materials[m["material"]]
        fs = f.lod0(m)
        tslots = f.textures[mat["tex"][0] : mat["tex"][0] + mat["tex"][1]]
        diffuse = next(
            (
                t["path"]
                for t in tslots
                if "Diffuse" in t["type"] or t["type"].endswith("_0")
            ),
            "",
        )
        print(
            f"  mesh {i}: material {mat['name']!r} dynamic {m['dynamic']} bones {len(m['bones'])} "
            f"vbufs {m['vbufs']} lod0 {fs['count']} idx strip={fs['strip']} isize={fs['isize']} "
            f"diffuse {os.path.basename(diffuse)}"
        )


def _diffuse_name(f: Flver, mesh: dict) -> str:
    """The color texture a mesh's material names, as a TPF texture name (path stem)."""
    first, count = f.materials[mesh["material"]]["tex"]
    slots = f.textures[first : first + count]
    pick = next(
        (t for t in slots if "Diffuse" in t["type"]), slots[0] if slots else None
    )
    if not pick or not pick["path"]:
        return ""
    return os.path.splitext(os.path.basename(pick["path"].replace("\\", "/")))[0]


def meshes(f: Flver):
    """[(mesh index, vertices dict, triangles, color texture name)] at full detail."""
    out = []
    for i, m in enumerate(f.meshes):
        v = f.vertices(m)
        if "pos" not in v:
            continue
        out.append((i, v, f.indices(f.lod0(m)), _diffuse_name(f, m)))
    return out


_INDEX: dict = {}


def _texture(name: str, by_name: dict, path: str):
    """A mesh's color texture: from its own archive, else (built on first need) from the
    game-wide TextureIndex of the dvdroot the archive lives in."""
    if not name:
        return None
    if name in by_name:
        return by_name[name]
    for k, t in by_name.items():  # the material's case and the TPF's often differ
        if k.lower() == name.lower():
            return t
    root = os.path.dirname(os.path.dirname(os.path.abspath(path)))
    if root not in _INDEX:
        _INDEX[root] = TextureIndex(root)
    return _INDEX[root].get(name)


def export_obj(path: str, outdir: str) -> str:
    f, texs = load(path)
    stem = os.path.basename(path).split(".")[0]
    os.makedirs(outdir, exist_ok=True)
    by_name = {t.name: t for t in texs}
    obj, mtl, base = [f"mtllib {stem}.mtl"], [], 1
    written = set()
    for i, v, tris, tex in meshes(f):
        mat = f"{stem}_mesh{i}"
        mtl += [f"newmtl {mat}", "Kd 1 1 1"]
        found = _texture(tex, by_name, path)
        if found is not None:
            if tex not in written:
                found.image().save(os.path.join(outdir, tex + ".png"))
                written.add(tex)
            mtl.append(f"map_Kd {tex}.png")
        obj += [f"o mesh{i}", f"usemtl {mat}"]
        obj += [f"v {x:.5f} {y:.5f} {z:.5f}" for x, y, z in v["pos"]]
        uv = v.get("uv") or [(0.0, 0.0)] * len(v["pos"])
        obj += [f"vt {a:.5f} {1 - b:.5f}" for a, b in uv]  # OBJ's v runs bottom to top
        nrm = v.get("normal") or [(0.0, 1.0, 0.0)] * len(v["pos"])
        obj += [f"vn {x:.4f} {y:.4f} {z:.4f}" for x, y, z in nrm]
        obj += [
            "f " + " ".join(f"{base + k}/{base + k}/{base + k}" for k in t)
            for t in tris
        ]
        base += len(v["pos"])
    with open(os.path.join(outdir, stem + ".obj"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(obj) + "\n")
    with open(os.path.join(outdir, stem + ".mtl"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(mtl) + "\n")
    return os.path.join(outdir, stem + ".obj")


def render(
    path: str | list[str],
    out: str,
    size: int = 1024,
    yaw: float = 180.0,
    pitch: float = 0.0,
) -> None:
    """A software render: orthographic, depth-buffered, textured, one light. Y is up.

    `path` may be a list of archives, drawn together into one picture: equipment pieces share
    the human skeleton's rest pose, so head, body, arms and legs assemble a whole figure."""
    import numpy as np
    from PIL import Image

    paths = [path] if isinstance(path, str) else list(path)
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    rot = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ np.eye(3)
    rot = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]]) @ rot
    parts = []
    allp = []
    for src in paths:
        f, texs = load(src)
        by_name = {t.name: t for t in texs}
        parts_of = [(v, tris, tex) for _, v, tris, tex in meshes(f) if tris]
        for v, tris, tex in parts_of:
            p = np.asarray(v["pos"], dtype=np.float64) @ rot.T
            n = (
                np.asarray(v.get("normal") or [(0, 1, 0)] * len(p), dtype=np.float64)
                @ rot.T
            )
            uv = np.asarray(v.get("uv") or [(0, 0)] * len(p), dtype=np.float64)
            img = refl = None
            found = _texture(tex, by_name, src)
            if found is not None:
                img = (
                    np.asarray(found.image().convert("RGBA"), dtype=np.float32) / 255.0
                )
            # the reflectance map beside the color map (x_a -> x_r): where metal's look lives
            if tex and tex[-2:].lower() == "_a":
                rfound = _texture(tex[:-2] + "_r", by_name, src)
                if rfound is not None:
                    refl = (
                        np.asarray(rfound.image().convert("RGB"), dtype=np.float32)
                        / 255.0
                    )
            parts.append((p, n, uv, np.asarray(tris, dtype=np.int64), img, refl))
            allp.append(p)
    if not allp:
        raise ValueError("the model has no meshes of its own")
    pts = np.concatenate(allp)
    lo, hi = pts.min(0), pts.max(0)
    scale = 0.92 * size / max(hi[0] - lo[0], hi[1] - lo[1])
    cx, cyy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    color = np.zeros((size, size, 3), np.float32) + np.array(
        [0.16, 0.17, 0.19], np.float32
    )
    depth = np.full((size, size), np.inf, np.float32)
    light = np.array([0.4, 0.6, 0.7])
    light /= np.linalg.norm(light)
    half = light + np.array(
        [0.0, 0.0, 1.0]
    )  # Blinn-Phong half vector; the viewer is at +Z
    half /= np.linalg.norm(half)
    for p, n, uv, tris, img, refl in parts:
        sx = (p[:, 0] - cx) * scale + size / 2
        sy2 = size / 2 - (p[:, 1] - cyy) * scale
        sz = -p[:, 2]  # nearer = smaller
        for a, b, c in tris:
            xs = (sx[a], sx[b], sx[c])
            ys = (sy2[a], sy2[b], sy2[c])
            x0, x1 = max(int(min(xs)), 0), min(int(max(xs)) + 1, size)
            y0, y1 = max(int(min(ys)), 0), min(int(max(ys)) + 1, size)
            if x0 >= x1 or y0 >= y1:
                continue
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c]
            region = depth[y0:y1, x0:x1]
            vis = inside & (z < region)
            if not vis.any():
                continue
            nn = w0[..., None] * n[a] + w1[..., None] * n[b] + w2[..., None] * n[c]
            nn /= np.linalg.norm(nn, axis=-1, keepdims=True) + 1e-9
            shade = 0.35 + 0.65 * np.clip(np.abs(nn @ light), 0, 1)
            if img is not None:
                u = w0 * uv[a, 0] + w1 * uv[b, 0] + w2 * uv[c, 0]
                vv = w0 * uv[a, 1] + w1 * uv[b, 1] + w2 * uv[c, 1]
                th, tw = img.shape[:2]
                tx = (np.mod(u, 1.0) * tw).astype(np.int64).clip(0, tw - 1)
                ty = (np.mod(vv, 1.0) * th).astype(np.int64).clip(0, th - 1)
                texel = img[ty, tx]
                vis &= texel[..., 3] >= 0.5  # alpha test (hair, cloth edges)
                rgb = texel[..., :3] * shade[..., None]
                if refl is not None:
                    # reflectance: an even sheen plus a highlight, so metal reads as metal
                    rh, rw = refl.shape[:2]
                    rx = (np.mod(u, 1.0) * rw).astype(np.int64).clip(0, rw - 1)
                    ry = (np.mod(vv, 1.0) * rh).astype(np.int64).clip(0, rh - 1)
                    spec = np.clip(np.abs(nn @ half), 0, 1) ** 24
                    rgb = rgb + refl[ry, rx] * (0.45 + 1.1 * spec)[..., None]
            else:
                rgb = np.full(gx.shape + (3,), 0.7, np.float32) * shade[..., None]
            region[vis] = z[vis]
            color[y0:y1, x0:x1][vis] = rgb[vis]
    Image.fromarray((color.clip(0, 1) * 255).astype(np.uint8)).save(out)


def render_all(base: str, update: str, outdir: str, size: int) -> None:
    """Render every chr/cNNNN.chrbnd.dcx; index.md lists each with the names the game shows for
    characters placed with that model (see textures._model_names for why that isn't identity)."""
    import glob

    import items as itemlib
    import textures as tex

    labels = tex._model_names(itemlib.World(base, update))
    os.makedirs(outdir, exist_ok=True)
    lines = [
        "# Character renders\n",
        (
            "Generated by `tools/flver.py all`: each character model in its rest "
            "pose, from the front. *Placed as* lists names the game shows for characters placed with that "
            "model; it is not the model's identity (c1050's bell-ringers carry The One Reborn's name).\n"
        ),
        "| Model | Render | Placed as |",
        "|---|---|---|",
    ]
    ok = 0
    for path in sorted(glob.glob(os.path.join(base, "chr", "c*.chrbnd.dcx"))):
        model = os.path.basename(path).split(".")[0]
        try:
            render(path, os.path.join(outdir, model + ".png"), size)
            cell = f"`{model}.png`"
            ok += 1
        except ValueError as e:
            cell = f"— ({e})"
        lines.append(f"| {model} | {cell} | {', '.join(labels.get(model, []))} |")
        print(model, cell, flush=True)
    with open(os.path.join(outdir, "index.md"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    print(f"{ok} renders -> {outdir}")


SLOTS = (
    ("hd", "headEquip"),
    ("bd", "bodyEquip"),
    ("am", "armEquip"),
    ("lg", "legEquip"),
)


def armor_sets(base: str, update: str) -> dict[int, dict[str, str]]:
    """Armor sets by model: {equipModelId: {slot: item name}}. A set's pieces share one model
    number; each piece's slot comes from its head/body/arm/leg flag."""
    import re

    import items as itemlib

    w = itemlib.World(base, update)
    names = w.names.get("EquipParamProtector", {})
    sets: dict[int, dict[str, str]] = {}
    for rid, r in sorted(w.rows("EquipParamProtector").items()):
        n = (names.get(rid) or "").strip()
        if not n or n == "NOT NEEDED" or re.fullmatch(r"en\d+", n):
            continue
        for slot, flag in SLOTS:
            if r.get(flag) and r["equipModelId"] > 0:
                sets.setdefault(r["equipModelId"], {}).setdefault(slot, n)
    return sets


def hunter_parts(base: str, model: int, sex: str = "m") -> list[str]:
    """The archives for a whole hunter in armor set `model`: the naked body (`fc_m_0000`
    or `fc_f_0000`), each slot's piece for that sex (or the shared `_a_` piece), and the
    default face (`fg_a_0000`), which closed helmets cover. The "bare" pieces (`bd_m_0000`
    and so on) are skeletons with no meshes, so the body is what fills an empty slot."""
    parts = os.path.join(base, "parts")

    def find(*names: str) -> list[str]:
        for n in names:
            path = os.path.join(parts, n + ".partsbnd.dcx")
            if os.path.exists(path):
                return [path]
        return []

    out = find(f"fc_{sex}_0000")
    for slot, _ in SLOTS:
        out += find(f"{slot}_{sex}_{model:04d}", f"{slot}_a_{model:04d}")
    return out + find("fg_a_0000")


def render_hunter(
    base: str, update: str, out: str, which: str, sex: str = "m", size: int = 1024
) -> None:
    """One hunter: `which` is a set's model number or the name of any piece of it."""
    sets = armor_sets(base, update)
    if which.isdigit():
        model = int(which)
    else:
        model = next(
            (
                m
                for m, s in sets.items()
                if any(n.lower() == which.lower() for n in s.values())
            ),
            -1,
        )
    if model not in sets:
        raise ValueError(f"no armor set {which!r}")
    render(hunter_parts(base, model, sex), out, size)


def render_hunters(base: str, update: str, outdir: str, size: int) -> None:
    """Every armor set as a whole hunter, male and female, with an index."""
    import textures as tex

    sets = armor_sets(base, update)
    os.makedirs(outdir, exist_ok=True)
    lines = [
        "# Hunters\n",
        (
            "Generated by `tools/flver.py hunters`: each armor set worn by the human "
            "model, male and female, in its rest pose. The naked body (`fc_m_0000`, `fc_f_0000`) is "
            "drawn under every set; the face is the default `fg_a_0000`.\n"
        ),
        "| Set | Male | Female | Pieces |",
        "|---:|---|---|---|",
    ]
    for model, pieces in sorted(sets.items()):
        label = pieces.get("bd") or next(iter(pieces.values()))
        cells = []
        for sex in "mf":
            fname = tex._safe(f"{model:04d} {sex} {label}") + ".png"
            try:
                render(
                    hunter_parts(base, model, sex), os.path.join(outdir, fname), size
                )
                cells.append(f"`{fname}`")
            except ValueError as e:
                cells.append(f"— ({e})")
        lines.append(
            f"| {model} | {cells[0]} | {cells[1]} | "
            + ", ".join(pieces[s] for s, _ in SLOTS if s in pieces)
            + " |"
        )
        print(model, label, flush=True)
    with open(os.path.join(outdir, "index.md"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    print(f"{len(sets)} sets -> {outdir}")


def render_parts(base: str, update: str, outdir: str, size: int) -> None:
    """Render every piece of player equipment (parts/*.partsbnd.dcx, not the _l versions), named
    from the item tables: weapons by equipModelId (wp_a_<model>), armor by slot, sex and set
    model (hd/bd/am/lg, m/f/a). Weapons are drawn side-on (yaw 0), armor from the front."""
    import glob

    import items as itemlib
    import textures as tex

    labels = tex._model_names(itemlib.World(base, update))
    os.makedirs(outdir, exist_ok=True)
    lines = [
        "# Equipment renders\n",
        (
            "Generated by `tools/flver.py parts`: each equipment model on its own, "
            "named from the item tables (`equipModelId`).\n"
        ),
        "| Part | Render | Items |",
        "|---|---|---|",
    ]
    ok = 0
    for path in sorted(glob.glob(os.path.join(base, "parts", "*.partsbnd.dcx"))):
        stem = os.path.basename(path).split(".")[0]
        if stem.endswith("_l"):
            continue
        names = labels.get(stem, [])
        fname = tex._safe(stem + (f" {names[0]}" if names else "")) + ".png"
        try:
            render(
                path,
                os.path.join(outdir, fname),
                size,
                0.0 if stem.startswith("wp_") else 180.0,
            )
            cell = f"`{fname}`"
            ok += 1
        except ValueError as e:
            cell = f"— ({e})"
        lines.append(f"| {stem} | {cell} | {', '.join(names)} |")
        print(stem, cell, flush=True)
    with open(os.path.join(outdir, "index.md"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    print(f"{ok} renders -> {outdir}")


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) >= 5 and a[1] == "hunters":
        render_hunters(
            a[2], a[3], a[4], int(a[a.index("--size") + 1]) if "--size" in a else 1024
        )
        return 0
    if len(a) >= 6 and a[1] == "hunter":
        render_hunter(
            a[2],
            a[3],
            a[4],
            a[5],
            a[a.index("--sex") + 1] if "--sex" in a else "m",
            int(a[a.index("--size") + 1]) if "--size" in a else 1024,
        )
        print(a[4])
        return 0
    if len(a) >= 5 and a[1] == "parts":
        render_parts(
            a[2], a[3], a[4], int(a[a.index("--size") + 1]) if "--size" in a else 1024
        )
        return 0
    if len(a) >= 5 and a[1] == "all":
        render_all(
            a[2], a[3], a[4], int(a[a.index("--size") + 1]) if "--size" in a else 1024
        )
        return 0
    if len(a) >= 3 and a[1] == "info":
        info(a[2])
        return 0
    if len(a) == 4 and a[1] == "obj":
        print(export_obj(a[2], a[3]))
        return 0
    if len(a) >= 4 and a[1] == "render":
        opt = lambda k, d: float(a[a.index(k) + 1]) if k in a else d
        render(
            a[2],
            a[3],
            int(opt("--size", 1024)),
            opt("--yaw", 180.0),
            opt("--pitch", 0.0),
        )
        print(a[3])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
