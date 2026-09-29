#!/usr/bin/env python3
"""
Pre-convert a GameMaker APK's texture pages for YoYo Loader (run on a PC).

Big pixel-art games (GMS 2024.x, e.g. UFO 50) store dozens of 2048x2048 pages
as bzip2+QOI. Decoding those on the Vita takes ~0.5s per page and each page
needs 16MB as RGBA. This tool decodes every page once on the PC and writes it
as an externalized .pvr the loader reads directly:

  * pages with <= 256 colours -> P8 (palette + 1 byte/pixel, lossless, 4MB)
  * other pages               -> raw RGBA8

Sounds set to "decompress on load" are switched to "compressed" so the
music stays as OGG in memory instead of raw PCM.

game.droid gets a tiny 2x1 placeholder PNG in place of each page (the same
scheme the selector's "Externalize" feature uses), and is stored uncompressed
in the new APK so the loader doesn't have to inflate it at boot.

Usage:
  pip install numpy numba pillow
  python gm_textures.py game.apk out_dir

Then copy out_dir/game.apk and out_dir/assets/ to ux0:data/gms/<game id>/.
Only use this on games you own.
"""
import bz2
import io
import os
import struct
import sys
import zipfile

import numpy as np
from PIL import Image

try:
    from numba import njit
except ImportError:  # works without numba, just much slower
    def njit(*a, **k):
        return (lambda f: f) if not (a and callable(a[0])) else a[0]


@njit(cache=True)
def _qoi_decode(data, w, h):
    # GameMaker's QOI variant (older draft of the spec)
    out = np.zeros(w * h * 4, np.uint8)
    index = np.zeros(64 * 4, np.int32)
    r = 0; g = 0; b = 0; a = 255; run = 0; pos = 0; n = len(data)
    for p in range(w * h):
        if run > 0:
            run -= 1
        elif pos < n:
            b1 = data[pos]; pos += 1
            if (b1 & 0xc0) == 0x00:
                i = (b1 & 0x3f) * 4
                r = index[i]; g = index[i + 1]; b = index[i + 2]; a = index[i + 3]
            elif (b1 & 0xe0) == 0x40:
                run = b1 & 0x1f
            elif (b1 & 0xe0) == 0x60:
                b2 = data[pos]; pos += 1
                run = (((b1 & 0x1f) << 8) | b2) + 32
            elif (b1 & 0xc0) == 0x80:
                r += ((b1 >> 4) & 3) - 2
                g += ((b1 >> 2) & 3) - 2
                b += (b1 & 3) - 2
            elif (b1 & 0xe0) == 0xc0:
                b2 = data[pos]; pos += 1
                r += (b1 & 0x1f) - 16
                g += (b2 >> 4) - 8
                b += (b2 & 15) - 8
            elif (b1 & 0xf0) == 0xe0:
                b2 = data[pos]; b3 = data[pos + 1]; pos += 2
                r += (((b1 & 15) << 1) | (b2 >> 7)) - 16
                g += ((b2 & 0x7c) >> 2) - 16
                b += (((b2 & 3) << 3) | (b3 >> 5)) - 16
                a += (b3 & 31) - 16
            else:
                if b1 & 8:
                    r = data[pos]; pos += 1
                if b1 & 4:
                    g = data[pos]; pos += 1
                if b1 & 2:
                    b = data[pos]; pos += 1
                if b1 & 1:
                    a = data[pos]; pos += 1
            r &= 255; g &= 255; b &= 255; a &= 255
            i = ((r ^ g ^ b ^ a) & 63) * 4
            index[i] = r; index[i + 1] = g; index[i + 2] = b; index[i + 3] = a
        out[p * 4] = r; out[p * 4 + 1] = g; out[p * 4 + 2] = b; out[p * 4 + 3] = a
    return out


def decode_page(blob):
    magic = blob[:4]
    if magic in (b'2zoq', b'fioq'):
        w, h = struct.unpack('<HH', blob[4:8])
        if magic == b'2zoq':
            raw = bz2.decompress(blob[12:])
        else:
            raw = blob[12:12 + struct.unpack('<I', blob[8:12])[0]]
        return w, h, _qoi_decode(np.frombuffer(raw, np.uint8), w, h).reshape(h, w, 4)
    if magic == b'\x89PNG':
        img = np.array(Image.open(io.BytesIO(blob)).convert('RGBA'))
        return img.shape[1], img.shape[0], img
    raise ValueError('unknown texture format %r' % magic)


def write_pvr(path, w, h, fmt, payload):
    hdr = struct.pack('<IIQIIIIIIIII', 0x03525650, 0, fmt, 0, 0, h, w, 1, 1, 1, 1, 0)
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(payload)


def convert_page(img):
    """Returns (format, payload): 0x100 = P8, 0x101 = RGBA8."""
    px = np.ascontiguousarray(img).reshape(-1, 4).view(np.uint32).ravel()
    colours, idx = np.unique(px, return_inverse=True)
    if len(colours) <= 256:
        pal = np.zeros(256, np.uint32)
        pal[:len(colours)] = colours
        return 0x100, pal.tobytes() + idx.astype(np.uint8).tobytes(), len(colours)
    return 0x101, np.ascontiguousarray(img).tobytes(), len(colours)


def placeholder_png(i):
    # 2x1 RGBA image the loader recognises: pixel 0 = 0xFFBEADDE, pixel 1 = texture index
    px = struct.pack('<II', 0xFFBEADDE, 0xFF000000 | i)
    buf = io.BytesIO()
    Image.frombytes('RGBA', (2, 1), px).save(buf, 'PNG')
    return buf.getvalue()


def parse_chunks(d):
    assert d[:4] == b'FORM'
    chunks = []
    off = 8
    while off < len(d):
        name = d[off:off + 4].decode()
        size = struct.unpack('<I', d[off + 4:off + 8])[0]
        chunks.append((name, off, size))
        off += 8 + size
    return chunks


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    apk_path, out_dir = sys.argv[1], sys.argv[2]
    assets_dir = os.path.join(out_dir, 'assets')
    os.makedirs(assets_dir, exist_ok=True)

    with zipfile.ZipFile(apk_path) as z:
        d = z.read('assets/game.droid')
    chunks = parse_chunks(d)
    names = [c[0] for c in chunks]
    ti = names.index('TXTR')
    _, t_off, t_size = chunks[ti]
    t_start = t_off + 8
    count = struct.unpack('<I', d[t_start:t_start + 4])[0]
    ptrs = struct.unpack('<%dI' % count, d[t_start + 4:t_start + 4 + 4 * count])
    entry_size = (ptrs[1] - ptrs[0]) if count > 1 else 28
    if entry_size != 28:
        sys.exit('Unsupported TXTR entry layout (%d bytes); this tool targets GMS 2024.x' % entry_size)

    entries = [list(struct.unpack('<7I', d[p:p + 28])) for p in ptrs]
    first_blob = min(e[6] for e in entries)
    new_txtr = bytearray(d[t_start:first_blob])  # count, pointers, entries (+ padding)

    total_p8 = total_rgba = 0
    for i, e in enumerate(entries):
        blob = d[e[6]:e[6] + e[2]]
        w, h, img = decode_page(blob)
        fmt, payload, ncol = convert_page(img)
        write_pvr(os.path.join(assets_dir, '%d.pvr' % i), w, h, fmt, payload)
        if fmt == 0x100:
            total_p8 += 1
        else:
            total_rgba += 1
        print('page %2d: %4dx%-4d %4d colours -> %s' % (i, w, h, ncol, 'P8' if fmt == 0x100 else 'RGBA'))

        # placeholder blob, 128-byte aligned like GameMaker does
        while (t_start + len(new_txtr)) % 128:
            new_txtr.append(0)
        ph = placeholder_png(i)
        blob_ptr = t_start + len(new_txtr)
        new_txtr += ph
        rel = ptrs[i] - t_start
        struct.pack_into('<I', new_txtr, rel + 8, len(ph))
        struct.pack_into('<I', new_txtr, rel + 24, blob_ptr)
    while len(new_txtr) % 4:
        new_txtr.append(0)

    out = bytearray(d[:t_off])

    # Music marked "decompress on load" (flags 0x67) gets unpacked to raw PCM when its
    # audio group loads (~10x its OGG size; UFO 50's menu music alone is ~62MB).
    # Switch it to "compressed" (0x66): the OGG stays in memory and is decoded as it plays.
    if 'SOND' in names:
        _, s_off, _ = chunks[names.index('SOND')]
        assert s_off < t_off
        n = struct.unpack('<I', out[s_off + 8:s_off + 12])[0]
        patched = 0
        for k in range(n):
            e = struct.unpack_from('<I', out, s_off + 12 + 4 * k)[0]
            if struct.unpack_from('<I', out, e + 4)[0] == 0x67:
                struct.pack_into('<I', out, e + 4, 0x66)
                patched += 1
        print('%d sounds switched from decompress-on-load to compressed' % patched)
    out += b'TXTR' + struct.pack('<I', len(new_txtr)) + new_txtr
    delta = (t_off + 8 + len(new_txtr)) - (t_off + 8 + t_size)
    for name, off, size in chunks[ti + 1:]:
        body = bytearray(d[off + 8:off + 8 + size])
        if name == 'AUDO' and size >= 4:  # absolute pointers into this chunk
            n = struct.unpack('<I', body[:4])[0]
            for k in range(n):
                p = struct.unpack_from('<I', body, 4 + 4 * k)[0]
                struct.pack_into('<I', body, 4 + 4 * k, p + delta)
        elif name != 'AUDO' and size > 0:
            sys.exit('Chunk %s after TXTR is not supported' % name)
        out += name.encode() + struct.pack('<I', size) + body
    struct.pack_into('<I', out, 4, len(out) - 8)

    # sanity check: every entry now points at its placeholder
    chk = bytes(out)
    for i, p in enumerate(ptrs):
        e = struct.unpack('<7I', chk[p:p + 28])
        assert chk[e[6]:e[6] + 8] == b'\x89PNG\r\n\x1a\n', 'page %d placeholder check failed' % i

    new_apk = os.path.join(out_dir, 'game.apk')
    with zipfile.ZipFile(apk_path) as src, zipfile.ZipFile(new_apk, 'w') as dst:
        for info in src.infolist():
            if info.filename == 'assets/game.droid':
                zi = zipfile.ZipInfo('assets/game.droid', info.date_time)
                zi.compress_type = zipfile.ZIP_STORED
                dst.writestr(zi, chk)
            else:
                dst.writestr(info, src.read(info.filename))

    print('\n%d pages as P8, %d as RGBA' % (total_p8, total_rgba))
    print('game.droid: %.1f MB -> %.1f MB' % (len(d) / 2**20, len(chk) / 2**20))
    print('Copy %s and the %s folder to ux0:data/gms/<game id>/' % (new_apk, assets_dir))


if __name__ == '__main__':
    main()
