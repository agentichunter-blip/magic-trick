"""Replace the boot intro's opening system-logo scenes with plain text.

The original intro (MagicTrick_2001_No_Oval_Chromatic_160x144.mp4) opens with
the Nintendo and Sega logos. This keeps its timing (fade-ins, holds, cut to
the sparkle at frame 68) but draws the system names as plain text in the
firmware's own 8x8 banner font (components/osd/system/banner_font.c), so the
intro carries no third-party logo artwork. Frames 68+ are copied unchanged.

Usage: make_text_intro.py <original.mp4> <out.mkv>
Writes a lossless FFV1 file at the source frame rate for encode_intro.py.
"""
import re, subprocess, sys
from pathlib import Path
import numpy as np

W, H = 160, 144
FONT_C = Path(__file__).resolve().parents[2] / 'components/osd/system/banner_font.c'
WHITE = np.array([255, 255, 255])

def load_font():
    src = FONT_C.read_text()
    bits = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', src.split('Bits[]={', 1)[1].split('};', 1)[0])]
    dsc = re.findall(r'\.bitmap_index=(\d+),\.adv_w=(\d+)', src)
    glyphs = {}
    for i, (index, adv) in enumerate(dsc):
        index = int(index)
        rows = bits[index:index + 8]
        glyphs[chr(32 + i)] = (np.array([[(r >> (7 - x)) & 1 for x in range(8)] for r in rows], bool), int(adv) // 16)
    return glyphs

GLYPHS = load_font()

def text_mask(text, scale, space=-1):
    """1-bit mask for one line. space=-1 matches the picker's tightened tracking."""
    width = sum(GLYPHS[c][1] + space for c in text) - space - 2  # drop trailing blank columns
    mask = np.zeros((8, max(width, 1)), bool)
    x = 0
    for c in text:
        bitmap, adv = GLYPHS[c]
        w = min(8, mask.shape[1] - x)
        mask[:, x:x + w] |= bitmap[:, :w]
        x += adv + space
    return np.kron(mask, np.ones((scale, scale), bool))

def card(lines, scale, gap):
    """Centre a block of lines on the frame; returns a full-frame mask."""
    masks = [text_mask(t, scale) for t in lines]
    total = sum(m.shape[0] for m in masks) + gap * (len(masks) - 1)
    out = np.zeros((H, W), bool)
    y = (H - total) // 2
    for m in masks:
        x = (W - m.shape[1]) // 2
        out[y:y + m.shape[0], x:x + m.shape[1]] = m
        y += m.shape[0] + gap
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', src, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         check=True, capture_output=True).stdout
    frames = np.frombuffer(raw, np.uint8).reshape(-1, H, W, 3).copy()
    bg = frames[0, 0, 0].astype(float)
    gb = card(['GAME BOY'], 2, 0)
    gbc = card(['GAME BOY', 'COLOR'], 2, 6)
    gg = card(['GAME', 'GEAR'], 2, 6)
    trio = card(['GAME BOY', 'GAME BOY COLOR', 'GAME GEAR'], 1, 10)
    fade = [0.25, 0.5, 0.75, 1.0]
    # (first frame, last frame, mask, fade in?) mirrors the original scene timing.
    scenes = [(3, 18, gb, True), (20, 35, gbc, True), (36, 51, gg, True), (52, 67, trio, False)]
    frames[:68] = bg.astype(np.uint8)
    for f0, f1, mask, fades in scenes:
        for f in range(f0, f1 + 1):
            a = fade[f - f0] if fades and f - f0 < len(fade) else 1.0
            colour = (bg + (WHITE - bg) * a).round().astype(np.uint8)
            frames[f][mask] = colour
    fps = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=r_frame_rate',
                          '-of', 'csv=p=0', src], check=True, capture_output=True, text=True).stdout.strip()
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', fps,
                    '-i', '-', '-c:v', 'ffv1', '-pix_fmt', 'bgr0', dst], input=frames.tobytes(), check=True)

if __name__ == '__main__':
    main()
