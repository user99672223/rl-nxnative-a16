#!/usr/bin/env python3
"""An IPA update without the game data: the app's large files (the game's file system image, the texture pack, the shader libraries) are the same from build to build,
so a build also publishes everything else (the executable and the small files, about 40 MB) with a list of what the large ones must be.

  ipa_delta.py make  <new.ipa> <delta.zip>            CI: the small entries and manifest.json (name, size, CRC of every entry)
  ipa_delta.py apply <old.ipa> <delta.zip> <new.ipa>  laptop: the new IPA from the one already here; fails if a large file differs (then fetch the whole IPA)

Large = 256 MiB or more. Large entries are copied as they are packed (no recompression), so applying takes about as long as copying the file."""
import json, shutil, struct, sys, zipfile

BIG = 256 << 20

def make(ipa, out):
    z = zipfile.ZipFile(ipa); man = []
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as o:
        for i in z.infolist():
            big = i.file_size >= BIG
            man.append({'name': i.filename, 'size': i.file_size, 'crc': i.CRC, 'big': big})
            if not big:
                with z.open(i) as f: o.writestr(i, f.read())
        o.writestr('manifest.json', json.dumps(man))
    print(f'{sum(1 for m in man if not m["big"])} entries in the update, {sum(1 for m in man if m["big"])} large ones left to the IPA already installed')

def raw_copy(src, info, dst_zip):
    """One entry of src, bytes as packed, into dst_zip (zipfile has no public way; this writes the local header through a ZipInfo and copies the packed bytes)"""
    with open(src.filename, 'rb') as f:
        f.seek(info.header_offset); h = f.read(30)
        if h[:4] != b'PK\x03\x04': raise SystemExit(f'bad local header for {info.filename}')
        n, m = struct.unpack('<HH', h[26:30]); f.seek(info.header_offset + 30 + n + m)
        zi = zipfile.ZipInfo(info.filename, info.date_time)
        zi.compress_type = info.compress_type; zi.external_attr = info.external_attr; zi.create_system = info.create_system
        zi.CRC = info.CRC; zi.compress_size = info.compress_size; zi.file_size = info.file_size; zi.flag_bits = info.flag_bits & 0x800
        fp = dst_zip.fp; zi.header_offset = fp.tell()
        fp.write(zi.FileHeader(zip64=info.file_size > 0xffffffff or info.compress_size > 0xffffffff))
        left = info.compress_size
        while left:
            b = f.read(min(left, 8 << 20))
            if not b: raise SystemExit(f'{info.filename}: the old IPA is cut short')
            fp.write(b); left -= len(b)
        dst_zip.filelist.append(zi); dst_zip.NameToInfo[zi.filename] = zi; dst_zip.start_dir = fp.tell(); dst_zip._didModify = True

def apply(old, delta, new):
    zo, zd = zipfile.ZipFile(old), zipfile.ZipFile(delta)
    man = json.loads(zd.read('manifest.json')); have = {i.filename: i for i in zo.infolist()}
    for m in man:
        if m['big']:
            i = have.get(m['name'])
            if not i or i.file_size != m['size'] or i.CRC != m['crc']:
                raise SystemExit(f'{m["name"]} changed in this build: the update cannot be applied, fetch the whole IPA (scripts/fetch_ipa.sh <run>)')
    with zipfile.ZipFile(new, 'w', allowZip64=True) as o:
        for m in man:
            if m['big']: raw_copy(zo, have[m['name']], o)
            else:
                i = zd.getinfo(m['name'])
                with zd.open(i) as f: o.writestr(i, f.read(), compress_type=zipfile.ZIP_DEFLATED if i.file_size else zipfile.ZIP_STORED)
    # the result is checked entry by entry against the build's list (sizes and CRCs from the new directory; the large ones were checked above and copied byte for byte)
    got = {i.filename: (i.file_size, i.CRC) for i in zipfile.ZipFile(new).infolist()}
    bad = [m['name'] for m in man if got.get(m['name']) != (m['size'], m['crc'])]
    if bad or len(got) != len(man): raise SystemExit(f'the assembled IPA does not match the build: {bad[:5]}')
    print(f'{new}: {len(man)} entries, matches the build')

if len(sys.argv) >= 4 and sys.argv[1] == 'make': make(sys.argv[2], sys.argv[3])
elif len(sys.argv) >= 5 and sys.argv[1] == 'apply': apply(sys.argv[2], sys.argv[3], sys.argv[4])
else: sys.exit(__doc__)
