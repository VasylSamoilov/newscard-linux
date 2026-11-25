"""Offline test: run probe, info and list through main() against a fake card
(message store from tests/test_store.py)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import test_store as ts   # builds ts.ram with four messages and prints them
import newscard as n

flash = bytearray(b'\xff' * 0x20000)

class Fake(n.NewsCard):
    def __init__(self, path=None):
        pass
    def read(self, off, k):
        if off >= n.FLASH:
            o = off - n.FLASH
            return bytes(flash[o:o + k])
        return bytes(ts.ram[off:off + k])
    def lread(self, off, k):
        return bytes(ts.ram[off:off + k])
    def event(self):
        return False
    def xfer(self, cmd, payload=b'', reply_len=None):
        data = {0x44: [1], 0x81: [0, 3], 0x82: [0, 4], 0x32: [98, 7, 14, 12, 34],
                0x83: list(range(18)) + [0x07, 0x89]}.get(cmd)
        if cmd == 0x11:
            data = [payload[0], payload[1], 0xC0]
        if data is None:
            raise n.CardError("command %02X: card answered NAK, code 1" % cmd)
        return bytes([cmd, 0, len(data) + 1, 6] + data)

n.NewsCard = Fake
for c in ('probe', 'info', 'list'):
    print('=====', c)
    sys.argv = ['newscard.py', c]
    n.main()
