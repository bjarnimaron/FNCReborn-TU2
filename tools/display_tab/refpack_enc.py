"""RefPack (EA QFS) compressor - greedy LZ with hash chains. Output decodes with refpack.unrefpack.
Needed because the game decompresses Apt members in place (compressed data placed at the end of the
output buffer), so a stored/literal-only stream bigger than the output corrupts the heap."""
from refpack import unrefpack

def refpack(data):
    data = bytes(data); n = len(data)
    out = bytearray(b'\x10\xfb' + n.to_bytes(3, 'big'))
    heads = {}; lit_start = 0; i = 0

    def flush_literals(upto, keep):  # emit literal blocks for data[lit_start:upto-keep]
        nonlocal lit_start
        while upto - lit_start - keep >= 4:
            k = min(112, (upto - lit_start - keep) & ~3)
            out.append(0xE0 | ((k - 4) >> 2)); out.extend(data[lit_start:lit_start + k]); lit_start += k

    while i < n:
        best_len = 0; best_off = 0
        if i + 3 <= n:
            key = data[i:i + 3]
            for p in reversed(heads.get(key, [])[-48:]):
                off = i - p
                if off > 131072: break
                l = 3; m = min(1028, n - i)
                while l < m and data[p + l] == data[i + l]: l += 1
                ok = (l >= 3 and off <= 1024) or (l >= 4 and off <= 16384) or (l >= 5)
                if ok and l > best_len: best_len, best_off = l, off
                if best_len >= 1028: break
        if best_len:
            flush_literals(i, 0)
            lit = i - lit_start; assert 0 <= lit <= 3
            L, off = best_len, best_off - 1
            if L <= 10 and best_off <= 1024:
                out += bytes([((off >> 3) & 0x60) | ((L - 3) << 2) | lit, off & 0xFF])
            elif L <= 67 and best_off <= 16384:
                out += bytes([0x80 | (L - 4), (lit << 6) | (off >> 8), off & 0xFF])
            else:
                L = min(L, 1028)
                out += bytes([0xC0 | ((off >> 12) & 0x10) | (((L - 5) >> 6) & 0x0C) | lit, (off >> 8) & 0xFF, off & 0xFF, (L - 5) & 0xFF])
            out += data[lit_start:i]
            for k in range(i, min(i + L, n - 2)):
                heads.setdefault(data[k:k + 3], []).append(k)
            i += L; lit_start = i
        else:
            if i + 3 <= n: heads.setdefault(data[i:i + 3], []).append(i)
            i += 1
    flush_literals(n, 0)
    rest = n - lit_start; assert rest <= 3
    out.append(0xFC | rest); out += data[lit_start:]
    out = bytes(out)
    assert unrefpack(out) == data, 'refpack roundtrip failed'
    return out
