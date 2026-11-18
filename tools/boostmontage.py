"""Sesion 7: montaje de BMP aclarados (xK) para ver escenas oscuras. python tools/boostmontage.py salida.png factor columnas a.bmp ..."""
import struct, zlib, sys
import numpy as np
out, k, cols, files = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
def load(f):
    d = open(f, "rb").read(); off = struct.unpack_from("<I", d, 10)[0]; w, h = struct.unpack_from("<ii", d, 18); hh = abs(h)
    a = np.frombuffer(d[off:off + w * hh * 4], np.uint8).reshape(hh, w, 4)
    if h > 0: a = a[::-1]
    return np.clip(a[:, :, 2::-1].astype(np.float32) * k, 0, 255).astype(np.uint8)[::3, ::3]
imgs = [load(f) for f in files]
H, W = imgs[0].shape[:2]; rows = (len(imgs) + cols - 1) // cols
canvas = np.zeros((rows * H, cols * W, 3), np.uint8)
for i, im in enumerate(imgs): canvas[(i // cols) * H:(i // cols) * H + im.shape[0], (i % cols) * W:(i % cols) * W + im.shape[1]] = im
raw = b"".join(b"\0" + canvas[y].tobytes() for y in range(canvas.shape[0]))
ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", canvas.shape[1], canvas.shape[0], 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b""))
