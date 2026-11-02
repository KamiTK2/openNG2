"""Build openNG2's shader precompile manifest from a playthrough's shader cache (findings #58).

Usage: python -I tools/build_shader_manifest.py <cache shareable dir> <TU2 image dir> <out dir>
  cache dir: %USERPROFILE%\\Documents\\ng2\\cache\\shaders\\shareable (544307D5.xsh +
             544307D5.rov.d3d12.xpso), image dir: ghidra-db/image-tu2 (decrypted TU2 image).

Writes <out dir>/shader_manifest.bin and <out dir>/shader_pipelines.xpso. Neither holds game
code: the manifest lists, per shader, its XXH3 hash, type, dword count and address in the game
executable (the shaders are compiled into the XEX), and the pipeline file is the SDK's pipeline
storage (shader hashes + render state). At startup openNG2 reads each shader from the player's
own game image, checks the hash and writes the shader cache, then builds all pipelines.

shader_manifest.bin: 'NG2S', u32 version 1, u32 count, then per shader (little-endian)
  u64 hash, u32 guest address, u32 dword count, u32 type (0 vertex, 1 pixel), u32 byteswap.
"""
import glob
import os
import shutil
import struct
import sys


def load_xsh(path):
    data = open(path, "rb").read()
    off, shaders = 8, []
    while off + 12 <= len(data):
        h, cw = struct.unpack_from("<QI", data, off)
        off += 12
        n = cw & 0x7FFFFFFF
        shaders.append((h, cw >> 31, data[off:off + n * 4]))
        off += n * 4
    return shaders


def main():
    cache_dir, image_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    shaders = load_xsh(os.path.join(cache_dir, "544307D5.xsh"))
    blocks = [(int(os.path.basename(p).split("_")[0], 16), open(p, "rb").read())
              for p in glob.glob(os.path.join(image_dir, "*.bin"))]
    entries, missing = [], 0
    for h, typ, ucode in shaders:
        swapped = b"".join(ucode[i:i + 4][::-1] for i in range(0, len(ucode), 4))
        hit = None
        for base, data in blocks:
            for flag, needle in ((0, ucode), (1, swapped)):
                i = data.find(needle)
                if i != -1:
                    hit = (base + i, flag)
                    break
            if hit:
                break
        if not hit:
            missing += 1
            continue
        entries.append((h, hit[0], len(ucode) // 4, typ, hit[1]))
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "shader_manifest.bin"), "wb") as f:
        f.write(b"NG2S" + struct.pack("<II", 1, len(entries)))
        for e in entries:
            f.write(struct.pack("<QIIII", *e))
    shutil.copyfile(os.path.join(cache_dir, "544307D5.rov.d3d12.xpso"),
                    os.path.join(out_dir, "shader_pipelines.xpso"))
    print(f"{len(entries)} shaders in the manifest, {missing} not found in the image "
          f"(created at runtime; they stay on-demand)")


if __name__ == "__main__":
    main()
