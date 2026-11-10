"""Encode the 160x144 boot intro into firmware (main/intro_video_data.c).

Format (all little-endian):
  header: 'CMIN', u8 version=2, u8 fps, u16 frames, u8 palette_count, 3 pad
  palette: palette_count x u16 RGB565 (0xF81F, the OSD transparent key, is avoided)
  per frame: ops until 160*144 pixels are covered; each op is 2 bytes:
    byte0 = 0xFF -> skip (pixels unchanged from the previous frame)
    byte0 < palette_count -> fill with that palette index
    byte1 = run length - 1 (1..256)
  Frame 0 is all fills. Video is quantised to one global palette with no dither,
  which removes H.264 grain so unchanged areas compress to skips.
Usage: encode_intro.py <video.mp4> <out.c> [colors] [preview.png]
"""
import subprocess, sys, struct
from pathlib import Path
from PIL import Image
W, H = 160, 144
N = W * H
# (first, last frame, y0, y1, x0, x1) regions painted out from the row below.
# Empty for the plain-text source made by make_text_intro.py.
CLEANUPS = []
SETTLE = 2  # frames a new best match must persist before replacing a held one
HOLD = 3  # RMS per-channel error tolerated to suppress grain (0..255 scale)

def frames(path):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         check=True, capture_output=True).stdout
    fps = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=r_frame_rate',
                          '-of', 'csv=p=0', path], check=True, capture_output=True, text=True).stdout.strip()
    num, den = map(int, fps.split('/'))
    return [Image.frombytes('RGB', (W, H), raw[i:i + N * 3]) for i in range(0, len(raw), N * 3)], round(num / den)

def rgb565(c):
    v = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)
    return 0xF81E if v == 0xF81F else v

def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    colors = int(sys.argv[3]) if len(sys.argv) > 3 else 255
    assert 1 <= colors <= 255
    imgs, fps = frames(src)
    import numpy as np
    arr = np.stack([np.asarray(im, dtype=np.int16) for im in imgs])  # T,H,W,3
    # Optional source clean-up (see CLEANUPS); the old logo source needed one.
    for f0, f1, y0, y1, x0, x1 in CLEANUPS:
        arr[f0:f1 + 1, y0:y1 + 1, x0:x1 + 1] = arr[f0:f1 + 1, y1 + 2:y1 + 3, x0:x1 + 1]
    # 2) Palette: weighted k-means over distinct colours, weight = sqrt(count),
    #    so the large flat background cannot starve faint fade-in colours.
    flat = arr.reshape(-1, 3)
    uniq, counts = np.unique(flat, axis=0, return_counts=True)
    w = np.sqrt(counts.astype(np.float64))
    pts = uniq.astype(np.float64)
    rng = np.random.default_rng(1)
    k = min(colors, len(pts))
    cent = [pts[np.argmax(counts)]]
    for _ in range(1, k):  # k-means++ seeding (weighted)
        d = np.min(((pts[:, None, :] - np.array(cent)[None]) ** 2).sum(2), axis=1)
        cent.append(pts[rng.choice(len(pts), p=(d * w) / (d * w).sum())])
    cent = np.array(cent)
    for _ in range(25):
        lab = np.argmin(((pts[:, None, :] - cent[None]) ** 2).sum(2), axis=1)
        for c in range(k):
            m = lab == c
            if m.any(): cent[c] = (pts[m] * w[m, None]).sum(0) / w[m].sum()
    pal = [int(round(v)) for v in cent.clip(0, 255).reshape(-1)]
    palette = [rgb565(pal[i * 3:i * 3 + 3]) for i in range(k)]
    colors = k
    # Map every frame to its nearest palette entry, then apply temporal
    # hysteresis in palette space: keep the previous entry while it is within
    # HOLD levels (RMS) of the best match for the *current* source pixel. This
    # removes H.264 grain shimmer (so static areas become 'skip' runs) without
    # letting slow fades accumulate error.
    eps = 3 * HOLD * HOLD
    # A held entry is replaced once the best match has been the same different
    # entry for SETTLE consecutive frames (a real, settled change, not grain).
    prev_idx = None
    cand = None
    streak = None
    idx_frames = []
    for fr in arr:
        px = fr.reshape(-1, 3).astype(np.float64)
        d = ((px[:, None, :] - cent[None]) ** 2).sum(2)
        best = np.argmin(d, axis=1)
        if prev_idx is not None:
            r = np.arange(len(px))
            same_cand = best == cand
            streak = np.where(same_cand, streak + 1, 1)
            cand = best
            keep = (d[r, prev_idx] <= d[r, best] + eps) & (streak < SETTLE)
            keep |= best == prev_idx
            best = np.where(keep, prev_idx, best)
        else:
            cand = best.copy()
            streak = np.ones(len(best), dtype=np.int32)
        idx_frames.append([int(v) for v in best])
        prev_idx = best
    final_err = np.sqrt(((arr[-1].reshape(-1, 3) - cent[np.array(idx_frames[-1])]) ** 2).mean(1))
    print(f'last-frame error vs source: mean={final_err.mean():.2f} max={final_err.max():.1f} (RMS levels)')
    data = bytearray(b'CMIN' + struct.pack('<BBHB3x', 2, fps, len(imgs), colors))
    for v in palette: data += struct.pack('<H', v)
    prev = None
    for cur in idx_frames:
        i = 0
        while i < N:
            if prev is not None and palette[cur[i]] == palette[prev[i]]:
                j = i
                while j < N and j - i < 256 and palette[cur[j]] == palette[prev[j]]: j += 1
                data += bytes((0xFF, j - i - 1))
            else:
                j = i
                while j < N and j - i < 256 and cur[j] == cur[i] and (prev is None or palette[cur[j]] != palette[prev[j]]): j += 1
                data += bytes((cur[i], j - i - 1))
            i = j
        prev = cur
    body = ',\n'.join('    ' + ','.join('0x%02x' % b for b in data[k:k + 24]) for k in range(0, len(data), 24))
    out.write_text('/* Generated by tools/intro/encode_intro.py from '
                   + Path(src).name + '. Do not edit. */\n#include "intro_video.h"\n'
                   'const uint8_t IntroVideoData[] = {\n' + body + '\n};\n'
                   'const size_t IntroVideoSize = sizeof(IntroVideoData);\n')
    print(f'frames={len(imgs)} fps={fps} colors={colors} bytes={len(data)}')
    if len(sys.argv) > 4:  # preview: decoded frames 0, 36, 72, 108, 143
        sheet = Image.new('RGB', (W * 5, H))
        for k, f in enumerate([0, 36, 72, 108, len(imgs) - 1]):
            px = [tuple(pal[c * 3:c * 3 + 3]) for c in idx_frames[f]]
            im = Image.new('RGB', (W, H)); im.putdata(px); sheet.paste(im, (k * W, 0))
        sheet.resize((W * 10, H * 2), Image.NEAREST).save(sys.argv[4])

if __name__ == '__main__':
    main()
