#!/usr/bin/env python3
"""The ASTC pack of an Unreal Engine 3 game's BC textures, for GPUs without BC texture formats (an A16 iPhone): every DXT1 / DXT3 / DXT5 level of every Texture2D in the cooked
packages, re-encoded offline by dm/tools/bc_astc/bc2astc and keyed by its content, so the host (dm/gfx/tex/nvn_bcsub.c) finds it when the game uploads that level. The game's files
are not changed: its packages are Oodle-compressed chunks whose sizes it knows, and half its textures are 8-byte-block DXT1, which no ASTC block replaces in place.

    ue3_astc.py <RomFS folder> <out pack> --bc2astc <binary> [--threads N] [--inventory <tsv>] [--limit N]

Written for Rocket League 1.0 (package version 868, licensee 20; PROGRESS.md of that port has the measurements):
- a package is its summary, then the name, import and export tables, AES-256-ECB from the name table to the header size (the summary's +8), then Oodle chunks. The key is the game's,
  given as 64 hex digits in DM_UE3_PKG_KEY (never in this repository);
- after the tables comes the chunk table: count, then {uncompressed offset, uncompressed size, compressed offset, compressed size}; export offsets are uncompressed ones;
- a Texture2D is its tagged properties (Format, CompressionSettings, TextureFileCacheName), 16 bytes, the level count, then per level {flags, element count, size on disk, offset,
  the bytes when stored inline, width, height}; flag 0x1: in <TextureFileCacheName>.tfc at that offset (chunks again), 0x20: no data;
- a level's bytes are Tegra block-linear as a one-level texture of its size (a 16x16 DXT1 level is 512 bytes, one GOB), which the host deswizzles before it uploads; the same code
  (dm/gfx/tex/nvn_deswizzle.c) is built into a small library here.
Chunks are decoded by the game's own decoder in the Mac host (shader_split.oodle: DM_OODLE_HOST, DM_OODLE_MODULES), or by a library with ooz's Kraken_Decompress named in DM_OOZ_LIB
(development on a machine without the host; it lacks the codec of a few tiny blocks, whose textures are then left out and counted)."""
import argparse, collections, ctypes, glob, hashlib, mmap, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = bytes.fromhex('c1832a9e')
KINDS = {'PF_DXT1': (1, 0x42), 'PF_DXT3': (2, 0x43), 'PF_DXT5': (3, 0x44)}   # format name → (bc2astc kind, an NVN format of that block size for the layout)
TEX_CLASSES = ('Texture2D', 'LightMapTexture2D', 'ShadowMapTexture2D')

def red(msg): sys.exit('[RED] ' + msg)

# ---- Oodle ----
_ooz = None; BAD = 0
def oodle(blk, u):
    """one compressed block → u bytes, or None when DM_OOZ_LIB's decoder lacks its codec"""
    global _ooz, BAD
    lib = os.environ.get('DM_OOZ_LIB')
    if not lib:
        sys.path.insert(0, os.path.join(HERE, '..', 'gfx', 'tools'))
        import shader_split
        return shader_split.oodle(blk, u)
    if _ooz is None:
        _ooz = ctypes.CDLL(lib)._Z17Kraken_DecompressPKhmPhm; _ooz.restype = ctypes.c_int
        _ooz.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    out = ctypes.create_string_buffer(u + 64)
    if _ooz(bytes(blk), len(blk), out, u) != u: BAD += 1; return None
    return out.raw[:u]

def chunks(d, i, end):
    """the chunks of d[i:end] decoded and joined; None when a block could not be decoded"""
    out = []
    while i + 16 <= len(d) and i < end and d[i:i + 4] == TAG:
        bs, cs, us = struct.unpack_from('<III', d, i + 4)
        if not bs: red(f'chunk at {i:#x} has block size 0')
        nb = (us + bs - 1) // bs; o = i + 16 + 8 * nb
        for k in range(nb):
            c, u = struct.unpack_from('<II', d, i + 16 + 8 * k); blk = d[o:o + c]
            b = bytes(blk) if c == u else oodle(blk, u)
            if b is None: return None
            out.append(b); o += c
        i = o
    return b''.join(out)

# ---- AES ----
def aes_ecb_decrypt(key, data):
    try:
        from Crypto.Cipher import AES
        return AES.new(key, AES.MODE_ECB).decrypt(data)
    except ImportError: pass
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        d = Cipher(algorithms.AES(key), modes.ECB()).decryptor(); return d.update(data) + d.finalize()
    except ImportError: pass
    r = subprocess.run(['openssl', 'enc', '-d', '-aes-256-ecb', '-nopad', '-K', key.hex()], input=data, capture_output=True)
    if r.returncode or len(r.stdout) != len(data): red('no AES: install pycryptodome or cryptography, or have openssl on PATH')
    return r.stdout

# ---- deswizzle (the host's own code) ----
class Desc(ctypes.Structure): _fields_ = [('target', ctypes.c_int), ('format', ctypes.c_int), ('flags', ctypes.c_int), ('samples', ctypes.c_int), ('width', ctypes.c_uint32), ('height', ctypes.c_uint32), ('depth', ctypes.c_uint32), ('levels', ctypes.c_uint32), ('stride', ctypes.c_uint32), ('pkg_layout', ctypes.c_int)]
class Lay(ctypes.Structure): _fields_ = [('size', ctypes.c_uint64), ('layer_size', ctypes.c_uint64), ('align', ctypes.c_uint32), ('layers', ctypes.c_uint32), ('bpb', ctypes.c_uint32), ('tileH', ctypes.c_uint), ('tileD', ctypes.c_uint)]
_tex = None
def texlib():
    global _tex
    if _tex is None:
        out = os.path.join(tempfile.mkdtemp(prefix='ue3_astc_'), 'libnvntex.' + ('dylib' if sys.platform == 'darwin' else 'so'))
        src = [os.path.join(HERE, '..', 'gfx', 'tex', 'nvn_deswizzle.c'), os.path.join(HERE, '..', 'gfx', 'nvn', 'nvn_tex.c')]
        r = subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-o', out] + src, capture_output=True, text=True)
        if r.returncode: red('could not build the deswizzle library: ' + r.stderr[-2000:])
        _tex = ctypes.CDLL(out)
    return _tex
def deswizzle(data, w, h, fmt):
    """a level stored as a one-level block-linear texture → its linear blocks, or None when the bytes are not that texture's storage"""
    L = texlib(); d = Desc(1, fmt, 0, 0, w, h, 1, 1, 0, 0); lay = Lay(); L.nvn_tex_layout(ctypes.byref(d), ctypes.byref(lay))
    if len(data) < lay.size: return None
    bw, bh = (w + 3) // 4, (h + 3) // 4; out = ctypes.create_string_buffer(bw * lay.bpb * bh)
    if L.nvn_deswizzle(ctypes.byref(d), 0, 0, bytes(data), out, ctypes.c_size_t(bw * lay.bpb)): return None
    return out.raw

# ---- packages ----
def fstr(d, o):
    n = struct.unpack_from('<i', d, o)[0]; o += 4
    if n >= 0: return d[o:o + n].rstrip(b'\0').decode('latin1'), o + n
    return d[o:o - 2 * n].decode('utf-16le', 'replace').rstrip('\0'), o - 2 * n

class Package:
    def __init__(self, path, key):
        with open(path, 'rb') as f: self.raw = raw = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        if raw[:4] != TAG: raise ValueError('not an Unreal package')
        self.ver, self.lic, self.hs = struct.unpack_from('<HHI', raw, 4)
        _, o = fstr(raw, 12)
        _, self.nname, self.oname, self.nexp, self.oexp, self.nimp, self.oimp, self.odep = struct.unpack_from('<8I', raw, o)
        n = (self.hs - self.oname) & ~15
        self.d = d = raw[:self.oname] + aes_ecb_decrypt(key, raw[self.oname:self.oname + n]) + raw[self.oname + n:self.hs]
        self.names = []; p = self.oname
        for _ in range(self.nname):
            s, p = fstr(d, p); p += 8; self.names.append(s)
        if p != self.oimp and self.nname: raise ValueError('the name table does not end at the import table: wrong key (DM_UE3_PKG_KEY)?')
        names = self.names
        self.imports = [names[struct.unpack_from('<i', d, self.oimp + 28 * i + 20)[0]] for i in range(self.nimp)]
        self.exports = []; o = self.oexp
        for _ in range(self.nexp):
            cls, _, _, on, oni, _, _, ssize, soff, _, nnet = struct.unpack_from('<iiiiiiQIIII', d, o); o += 68 + 4 * nnet
            self.exports.append((self.imports[-cls - 1] if cls < 0 else '', names[on] + (f'_{oni - 1}' if oni else ''), soff, ssize))
        n = struct.unpack_from('<I', d, self.odep)[0]
        self.chunks = [struct.unpack_from('<4I', d, self.odep + 4 + 16 * k) for k in range(n)]
        self._dec = {}
    def read(self, off, size):
        """size bytes at an uncompressed offset; None when their chunk could not be decoded"""
        if not self.chunks: return self.raw[off:off + size]
        out = b''
        for k, (uo, us, co, cs) in enumerate(self.chunks):
            if off >= uo + us or off + size <= uo: continue
            if k not in self._dec: self._dec[k] = chunks(self.raw, co, co + cs)
            if self._dec[k] is None: return None
            out += self._dec[k][max(off, uo) - uo:off + size - uo]
        return out if len(out) == size else None

def properties(P, b, o):
    names = P.names; out = {}
    while True:
        n = struct.unpack_from('<i', b, o)[0]; o += 8
        if names[n] == 'None': return out, o
        t, _, size, idx = struct.unpack_from('<iiii', b, o); o += 16; tn = names[t]; val = None
        if tn == 'StructProperty': o += 8
        elif tn == 'ByteProperty':
            o += 8
            val = names[struct.unpack_from('<i', b, o)[0]] if size == 8 else b[o]
        elif tn == 'BoolProperty': val = b[o]; o += 1; size = 0
        elif tn == 'IntProperty': val = struct.unpack_from('<i', b, o)[0]
        elif tn == 'NameProperty': val = names[struct.unpack_from('<i', b, o)[0]]
        o += size
        if not idx: out[names[n]] = val

def texture(P, off, size):
    """→ (properties, [(flags, width, height, inline bytes or None, file offset, size on disk)])"""
    b = P.read(off, size)
    if b is None: return None
    pr, o = properties(P, b, 4)
    o += 16
    n = struct.unpack_from('<I', b, o)[0]; o += 4; levels = []
    for _ in range(n):
        fl, _, sz, at = struct.unpack_from('<IIII', b, o); o += 16; data = None
        if not fl & 0x21: data = b[o:o + sz]; o += sz
        w, h = struct.unpack_from('<II', b, o); o += 8
        levels.append((fl, w, h, data, at, sz))
    return pr, levels

def mode_of(pr, kind):
    cs = pr.get('CompressionSettings') or ''
    if cs == 'TC_NormalmapSwizzle' and kind == 3: return 1
    if cs in ('TC_Normalmap', 'TC_NormalmapAlpha', 'TC_NormalmapUncompressed') and kind == 1: return 2
    return 0

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('romfs'); ap.add_argument('out'); ap.add_argument('--bc2astc', required=True); ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--inventory', help='write one row per texture level (no texture data)'); ap.add_argument('--limit', type=int, default=0, help='stop after this many packages (testing)')
    a = ap.parse_args()
    kh = os.environ.get('DM_UE3_PKG_KEY', '')
    if len(kh) != 64: red('DM_UE3_PKG_KEY must hold the game\'s package key as 64 hex digits')
    key = bytes.fromhex(kh)
    files = sorted(f for f in glob.glob(os.path.join(a.romfs, '**', '*'), recursive=True) if f.lower().endswith(('.xxx', '.upk')))
    if a.limit: files = files[:a.limit]
    tfcs = {os.path.basename(f).lower()[:-4]: f for f in glob.glob(os.path.join(a.romfs, '**', '*'), recursive=True) if f.lower().endswith('.tfc')}
    tfc_map = {}
    def tfc(name):
        if name not in tfc_map:
            with open(tfcs[name], 'rb') as f: tfc_map[name] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        return tfc_map[name]
    enc = subprocess.Popen([a.bc2astc, a.out] + ([str(a.threads)] if a.threads else []), stdin=subprocess.PIPE)
    enc.stdin.write(b'BCJ2')
    inv = open(a.inventory, 'w') if a.inventory else None
    seen = set(); st = collections.Counter(); levels_seen = {}
    for fi, f in enumerate(files):
        try: P = Package(f, key)
        except (ValueError, struct.error, IndexError) as e: st['packages not read'] += 1; print(f'[RED] {os.path.basename(f)}: {e}', file=sys.stderr); continue
        for cls, name, off, size in P.exports:
            if cls not in TEX_CLASSES: continue
            try: t = texture(P, off, size)
            except (struct.error, IndexError): t = None
            if t is None: st['textures not read'] += 1; continue
            pr, levels = t; fmt = pr.get('Format'); st['textures'] += 1
            if fmt not in KINDS: st[f'not BC ({fmt})'] += 1; continue
            kind, nfmt = KINDS[fmt]; mode = mode_of(pr, kind); cache = (pr.get('TextureFileCacheName') or '').lower()
            # A copy of a texture in another package names the same cache levels: its signature comes from the digests kept for them, without decoding them again
            def level(li, fl, w, h, data, at, sz, want_data):
                """→ (digest, linear blocks or None), or None when the level has no data"""
                lk = (cache, at, sz, w, h, kind) if fl & 1 else None
                if lk in levels_seen and not want_data: return levels_seen[lk], None
                if fl & 1:
                    if cache not in tfcs: st['levels in a cache file that is not there'] += 1; return None
                    data = chunks(tfc(cache), at, at + sz)
                    if data is None: st['levels not decoded'] += 1; return None
                lin = deswizzle(data, w, h, nfmt)
                if lin is None: st['levels of another size than their storage'] += 1; return None
                dg = hashlib.sha1(struct.pack('<II', w, h) + lin).digest()
                if lk: levels_seen[lk] = dg
                return dg, lin
            use = [(li,) + lv for li, lv in enumerate(levels) if not lv[0] & 0x20 and lv[1] and lv[2]]
            got = [level(*u, False) for u in use]
            use = [u for u, g in zip(use, got) if g]; got = [g for g in got if g]
            if not got: continue
            sig = hashlib.sha1(bytes([kind, mode]) + b''.join(g[0] for g in got)).digest()
            if sig in seen: st['textures already seen'] += 1; continue
            out = []
            for u, g in zip(use, got):
                lin = g[1] if g[1] is not None else level(*u, True)[1]
                out.append((u[2], u[3], lin))
                if inv: inv.write('\t'.join(map(str, (os.path.basename(f), name, cls, fmt, pr.get('SRGB', 1), pr.get('CompressionSettings') or '', u[0], u[2], u[3], hex(u[1]), len(lin)))) + '\n')
            seen.add(sig); st['textures encoded'] += 1; st['levels encoded'] += len(out); st['bytes of BC blocks'] += sum(len(x[2]) for x in out)
            enc.stdin.write(struct.pack('<BBH', kind, mode, len(out)))
            for w, h, lin in out: enc.stdin.write(struct.pack('<III', w, h, len(lin))); enc.stdin.write(lin)
        if fi % 100 == 0: print(f'{fi}/{len(files)} packages, {st["textures encoded"]} textures', file=sys.stderr, flush=True)
    enc.stdin.close()
    if enc.wait(): red('bc2astc failed')
    if BAD: st['blocks the development decoder could not decode'] = BAD
    for k, v in sorted(st.items()): print(f'{v}\t{k}')

if __name__ == '__main__': main()
