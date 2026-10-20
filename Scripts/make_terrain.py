"""Terrain for the Unreal client from Copernicus GLO-30 tiles.

    python Scripts/make_terrain.py <dem-dir> [path/to/toll-sim]

Reads the DEM GeoTIFFs (deflate, floating-point predictor; decoded here with
numpy and zlib, no GDAL), samples them on a 30 m grid in the sim's metric frame
within RADIUS of the road, and writes Content/Data/terrain.bin.

Heights are stored relative to the nearest road point, so the road, the plazas
and the vehicles (all at z = 0) stay put and the land rises and falls around
them. Under the carriageway and around the plazas the ground is flattened, easing
to the real relief; it fades back to road level near RADIUS so the client can
fill everything beyond the chunks with flat ground.

terrain.bin, little-endian:
    char[4] "TTER", int32 version=1, int32 chunkCells, float32 cell (m),
    int32 chunkCount, then per chunk: int32 cx, int32 cy (chunk index;
    origin = cx*chunkCells*cell, cy*chunkCells*cell in sim metres), then
    (chunkCells+1)^2 int16 heights in decimetres, row-major, rows along +y.
"""
import json
import math
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
import pyproj
from scipy.spatial import cKDTree

CELL = 30.0          # metres
CHUNK = 64           # cells per chunk side
RADIUS = 3000.0      # metres from the road
FLAT = 14.0          # metres from a lane centre that stay exactly at road level
BLEND = 40.0         # metres over which the relief returns
PLAZA = 160.0        # metres around a plaza centre kept flat (the apron is wider than the lanes)
PLAZA_BLEND = 120.0
EDGE = 600.0         # the relief fades to road level over the last EDGE metres before RADIUS,
                     # so the flat ground the client lays beyond the chunks meets it without a step


def read_tiff(path):
    """Full-resolution band of a single-band float32 tiled GeoTIFF."""
    b = Path(path).read_bytes()
    bo = "<" if b[:2] == b"II" else ">"
    (ifd,) = struct.unpack(bo + "I", b[4:8])
    (n,) = struct.unpack(bo + "H", b[ifd:ifd + 2])
    tags = {}
    sizes = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 11: 4, 12: 8, 16: 8}
    fmts = {1: "B", 2: "c", 3: "H", 4: "I", 5: "II", 11: "f", 12: "d", 16: "Q"}
    for i in range(n):
        e = ifd + 2 + i * 12
        tag, typ, cnt = struct.unpack(bo + "HHI", b[e:e + 8])
        size = sizes[typ] * cnt
        off = e + 8 if size <= 4 else struct.unpack(bo + "I", b[e + 8:e + 12])[0]
        if typ == 2:
            val = b[off:off + cnt]
        else:
            f = fmts[typ]
            val = struct.unpack(bo + f * cnt, b[off:off + size])
        tags[tag] = val
    W, H = tags[256][0], tags[257][0]
    tw, th = tags[322][0], tags[323][0]
    offs, cnts = tags[324], tags[325]
    comp = tags.get(259, (1,))[0]
    pred = tags.get(317, (1,))[0]
    assert comp == 8, f"compression {comp}"
    assert tags.get(339, (3,))[0] == 3 and tags.get(258, (32,))[0] == 32
    out = np.zeros((H, W), np.float32)
    tx = math.ceil(W / tw)
    for k, (o, c) in enumerate(zip(offs, cnts)):
        raw = np.frombuffer(zlib.decompress(b[o:o + c]), np.uint8)
        if pred == 3:
            rows = raw.reshape(th, tw * 4)
            rows = np.cumsum(rows, axis=1, dtype=np.uint8)
            planes = rows.reshape(th, 4, tw)  # byte planes, most significant first
            tile = np.ascontiguousarray(planes.transpose(0, 2, 1)).view(">f4").reshape(th, tw)
        elif pred == 1:
            tile = raw.view(bo + "f4").reshape(th, tw)
        else:
            raise ValueError(f"predictor {pred}")
        y0, x0 = (k // tx) * th, (k % tx) * tw
        h, w = min(th, H - y0), min(tw, W - x0)
        out[y0:y0 + h, x0:x0 + w] = tile[:h, :w]
    sx, sy = tags[33550][0], tags[33550][1]
    tie = tags[33922]  # i, j, k, x, y, z
    lon0, lat0 = tie[3] - tie[0] * sx, tie[4] + tie[1] * sy
    return out, lon0, lat0, sx, sy


class Dem:
    def __init__(self, paths):
        self.tiles = [read_tiff(p) for p in paths]
        for t, p in zip(self.tiles, paths):
            a, lon0, lat0, sx, sy = t
            print(f"{Path(p).name}: {a.shape[1]}x{a.shape[0]}, lon {lon0:.3f}.., lat {lat0:.3f}.., "
                  f"{a.min():.0f}..{a.max():.0f} m")

    def sample(self, lon, lat):
        out = np.full(lon.shape, np.nan, np.float64)
        for a, lon0, lat0, sx, sy in self.tiles:
            H, W = a.shape
            # pixel-is-area: centres at +0.5
            fx = (lon - lon0) / sx - 0.5
            fy = (lat0 - lat) / sy - 0.5
            m = (fx >= 0) & (fx <= W - 1) & (fy >= 0) & (fy <= H - 1) & np.isnan(out)
            if not m.any():
                continue
            x, y = fx[m], fy[m]
            x0 = np.minimum(np.floor(x).astype(int), W - 2)
            y0 = np.minimum(np.floor(y).astype(int), H - 2)
            dx, dy = x - x0, y - y0
            v = (a[y0, x0] * (1 - dx) * (1 - dy) + a[y0, x0 + 1] * dx * (1 - dy)
                 + a[y0 + 1, x0] * (1 - dx) * dy + a[y0 + 1, x0 + 1] * dx * dy)
            out[m] = v
        return out


def main():
    dem_dir = Path(sys.argv[1])
    here = Path(__file__).resolve().parent.parent
    sim = Path(sys.argv[2]) if len(sys.argv) > 2 else here.parent / "toll-sim"
    road = json.loads((here / "Content/Data/road.json").read_text())
    net = (sim / "sim/net/a1.net.xml").read_text(errors="ignore")[:4000]
    loc = net[net.index("<location"):]
    off = [float(v) for v in loc.split('netOffset="')[1].split('"')[0].split(",")]
    proj = loc.split('projParameter="')[1].split('"')[0]
    to_geo = pyproj.Transformer.from_crs(proj, "EPSG:4326", always_xy=True)

    def geo(x, y):
        return to_geo.transform(x - off[0], y - off[1])

    dem = Dem(sorted(dem_dir.glob("*.tif")))

    # road samples every ~10 m with their DEM height, smoothed along each lane
    pts, heights = [], []
    for lane in road["lanes"]:
        p = np.array(lane["pts"], float)
        seg = []
        for a, b in zip(p[:-1], p[1:]):
            n = max(1, int(np.hypot(*(b - a)) / 10))
            seg.append(a + (b - a) * (np.arange(n)[:, None] / n))
        seg.append(p[-1:])
        q = np.vstack(seg)
        lon, lat = geo(q[:, 0], q[:, 1])
        h = dem.sample(np.asarray(lon), np.asarray(lat))
        if np.isnan(h).any():
            h = np.where(np.isnan(h), np.nanmean(h), h)
        k = 15  # ~150 m running mean: the DEM is a surface model, the road is graded
        if len(h) > k:
            h = np.convolve(np.pad(h, k // 2, mode="edge"), np.ones(k) / k, mode="valid")[:len(q)]
        pts.append(q)
        heights.append(h)
    pts, heights = np.vstack(pts), np.concatenate(heights)
    tree = cKDTree(pts)
    print(f"road: {len(pts)} samples, {heights.min():.0f}..{heights.max():.0f} m")

    plazas = np.array([[p["x"], p["y"]] for p in road.get("plazas", [])], float).reshape(-1, 2)
    ptree = cKDTree(plazas) if len(plazas) else None

    size = CHUNK * CELL
    # chunks the corridor touches
    cand = set()
    for x, y in pts[::20]:
        r = int(math.ceil(RADIUS / size)) + 1
        cx, cy = int(math.floor(x / size)), int(math.floor(y / size))
        for i in range(cx - r, cx + r + 1):
            for j in range(cy - r, cy + r + 1):
                cand.add((i, j))
    chunks = []
    g = np.arange(CHUNK + 1) * CELL
    for cx, cy in sorted(cand):
        X, Y = np.meshgrid(cx * size + g, cy * size + g)
        d, idx = tree.query(np.c_[X.ravel(), Y.ravel()], workers=-1)
        if d.min() > RADIUS:
            continue
        lon, lat = geo(X.ravel(), Y.ravel())
        h = dem.sample(np.asarray(lon), np.asarray(lat))
        rel = h - heights[idx]
        rel = np.where(np.isnan(rel), 0, rel)
        w = np.clip((d - FLAT) / BLEND, 0, 1)
        w = w * w * (3 - 2 * w)
        if len(plazas):
            dp, _ = ptree.query(np.c_[X.ravel(), Y.ravel()])
            wp = np.clip((dp - PLAZA) / PLAZA_BLEND, 0, 1)
            w = np.minimum(w, wp * wp * (3 - 2 * wp))
        we = np.clip((RADIUS - d) / EDGE, 0, 1)
        w = w * we * we * (3 - 2 * we)
        # under and beside the road the ground sits 25 cm below it, so it never shows through
        rel = rel * w - 0.25 * (1 - w)
        chunks.append((cx, cy, np.clip(np.round(rel * 10), -32768, 32767).astype("<i2").reshape(CHUNK + 1, CHUNK + 1)))
    out = here / "Content/Data/terrain.bin"
    with open(out, "wb") as f:
        f.write(b"TTER" + struct.pack("<iifi", 1, CHUNK, CELL, len(chunks)))
        for cx, cy, a in chunks:
            f.write(struct.pack("<ii", cx, cy))
            f.write(a.tobytes())
    allh = np.concatenate([a.ravel() for _, _, a in chunks]) / 10
    print(f"wrote {out}: {len(chunks)} chunks, relief {allh.min():.1f}..{allh.max():.1f} m")


if __name__ == "__main__":
    main()
