#!/usr/bin/env python3
"""Headless STL -> PNG preview (Agg software backend; no GL/X needed).
Usage: stl_preview.py OUT.png "TITLE" ELEV AZIM  path,dz,#rrggbb  [more parts...]"""
import sys, struct, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

def load_stl(path):
    data = open(path, "rb").read()
    if data[:5] == b"solid" and b"facet" in data[:2000]:            # ASCII
        tris, cur = [], []
        for ln in data.decode("ascii", "ignore").splitlines():
            s = ln.split()
            if len(s) >= 4 and s[0] == "vertex":
                cur.append([float(s[1]), float(s[2]), float(s[3])])
                if len(cur) == 3: tris.append(cur); cur = []
        return np.array(tris, float)
    n = struct.unpack("<I", data[80:84])[0]; off = 84; tris = []    # binary
    for _ in range(n):
        v = struct.unpack("<12f", data[off+12:off+48]); off += 50
        tris.append([v[0:3], v[3:6], v[6:9]])
    return np.array(tris, float)

def add(ax, tris, color, light=np.array([0.35, 0.5, 0.8])):
    nrm = np.cross(tris[:,1]-tris[:,0], tris[:,2]-tris[:,0])
    L = np.linalg.norm(nrm, axis=1, keepdims=True); L[L==0] = 1; nrm /= L
    light = light/np.linalg.norm(light)
    shade = 0.4 + 0.6*np.clip(nrm @ light, 0, 1)
    fc = np.clip(shade[:,None]*np.array(color)[None,:], 0, 1)
    ax.add_collection3d(Poly3DCollection(tris, facecolors=fc,
                        edgecolors=(0,0,0,0.10), linewidths=0.2))

def main():
    out, title, elev, azim = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    fig = plt.figure(figsize=(8,6), dpi=130); ax = fig.add_subplot(111, projection="3d")
    pts = []
    for spec in sys.argv[5:]:
        path, dz, hexc = spec.rsplit(",", 2)
        rgb = [int(hexc.lstrip("#")[i:i+2],16)/255 for i in (0,2,4)]
        t = load_stl(path).copy(); t[:,:,2] += float(dz)
        add(ax, t, rgb); pts.append(t.reshape(-1,3))
    P = np.vstack(pts); mn, mx = P.min(0), P.max(0); c = (mn+mx)/2; r = (mx-mn).max()/2*1.05
    ax.set_xlim(c[0]-r,c[0]+r); ax.set_ylim(c[1]-r,c[1]+r); ax.set_zlim(c[2]-r,c[2]+r)
    ax.set_box_aspect((1,1,1)); ax.view_init(elev=elev, azim=azim); ax.set_axis_off()
    ax.set_title(title, fontsize=11)
    fig.tight_layout(); fig.savefig(out, facecolor="white"); print("wrote", out)

main()
