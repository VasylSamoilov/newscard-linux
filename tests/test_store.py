import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import newscard as n
ram = bytearray(b'\xff' * 0x20000); top = [0x1C00]
def p32(o, v): ram[o:o+4] = struct.pack('>I', v)
def add(bucket, seq, t, text, read=False):
    rec = top[0]; top[0] += 64; ram[rec:rec+64] = bytes(64)
    L = len(text); nb = max(1, (L + 53)//54); first = top[0]; last = first + (nb-1)*64
    for b in range(nb):
        blk = top[0]; top[0] += 64; ram[blk:blk+64] = bytes(64)
        p32(blk, blk+64 if b+1 < nb else 0xFFFFFFFF); p32(blk+4, last if b == 0 else 0)
        ch = text[b*54:(b+1)*54]; ram[blk+10:blk+10+len(ch)] = ch
    end = last + 10 + (L - (nb-1)*54) - 1
    head = 0x1400 + bucket*0x80
    p32(rec, struct.unpack('>I', ram[head:head+4])[0]); p32(head, rec)
    ram[rec+9] = seq; p32(rec+20, first); p32(rec+24, end); ram[rec+38] = 4 if read else 0
    ram[rec+40:rec+42] = struct.pack('>H', nb); ram[rec+45:rec+50] = bytes(t)
add(0, 1, [98,7,13,9,5], b'Hello from the emulated NewsCard.', True)
add(0, 2, [98,7,14,8,30], b'This is a longer POCSAG message that spans more than one 54 byte text block, to test the block chain.')
add(5, 3, [98,7,14,11,59], b'5551234')
add(1, 4, [98,7,14,12,0], b'X'*54)   # exactly one full block
class Fake(n.NewsCard):
    def __init__(self): pass
    def lread(self, off, k): return bytes(ram[off:off+k])
c = Fake()
for r in c.records(): print(r['seq'], n.fmt_time(r['time']), r['read'], c.text(r))
