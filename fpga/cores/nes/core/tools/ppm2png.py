"""ppm2png.py IN.ppm [OUT.png] [scale] - tiny P6 -> PNG converter (no dependencies)."""
import sys, zlib, struct
def conv(src, dst, scale=2):
    d = open(src, 'rb').read()
    parts = d.split(b'\n', 1)
    hdr = parts[0].split()
    w, h = int(hdr[1]), int(hdr[2]); px = parts[1]
    rows = []
    for y in range(h):
        row = px[y*w*3:(y+1)*w*3]
        row = b''.join(row[x*3:x*3+3] * scale for x in range(w))
        rows += [b'\x00' + row] * scale
    raw = b''.join(rows)
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w*scale, h*scale, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
if __name__ == '__main__':
    conv(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else sys.argv[1][:-4] + '.png', int(sys.argv[3]) if len(sys.argv) > 3 else 2)
