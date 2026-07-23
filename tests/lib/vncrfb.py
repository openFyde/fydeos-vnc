#!/usr/bin/env python3
"""Minimal RFB 3.8 test client for FydeOS VNC.

Supports the security paths implemented by fydeos-vncd:
  - VeNCrypt (19) with sub-types X509None (260), X509Plain (262), Plain (256)
  - SecTypeNone (1)            [loopback, no password]
  - VncAuth (2)                [insecure mode, DES challenge-response]

Stdlib only. Subcommands:
  sectypes                       print advertised security types / subtypes
  fingerprint                    print SHA-256 fingerprint of the TLS cert
  probe                          full handshake, print server name + geometry
  screenshot OUT.png             grab one full frame (Raw), write PNG + stats
  key <keysym> [down|up|press]   send KeyEvent(s)
  type <text>                    type ASCII text as KeyEvents
  pointer <x> <y> [mask]         send PointerEvent with raw button mask
  click <x> <y> [1|2|3]          press+release button at position
  wheel <x> <y> <up|down> [n]    scroll n notches at position
  cuttext <text>                 send legacy ClientCutText (latin-1)
  watch <seconds>                stream incremental updates, report fps/bytes
  selftest                       run DES + PNG self-tests (no network)

Examples:
  vncrfb.py --host 192.168.1.50 sectypes
  vncrfb.py --host 192.168.1.50 --password s3cret screenshot /tmp/x.png
  vncrfb.py --host 127.0.0.1 --port 15900 --security none probe
"""

import argparse
import os
import socket
import ssl
import struct
import sys
import time
import zlib

# ---------------------------------------------------------------- DES (VncAuth)
# Compact single-purpose DES (encrypt only), used for the RFB VncAuth
# challenge-response, where the key is the password with each byte bit-reversed.

_IP = [58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,
       62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
       57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,
       61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7]
_FP = [40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,
       38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
       36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,
       34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25]
_E  = [32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
       16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1]
_P  = [16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,
       2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25]
_PC1 = [57,49,41,33,25,17,9,1,58,50,42,34,26,18,
        10,2,59,51,43,35,27,19,11,3,60,52,44,36,
        63,55,47,39,31,23,15,7,62,54,46,38,30,22,
        14,6,61,53,45,37,29,21,13,5,28,20,12,4]
_PC2 = [14,17,11,24,1,5,3,28,15,6,21,10,
        23,19,12,4,26,8,16,7,27,20,13,2,
        41,52,31,37,47,55,30,40,51,45,33,48,
        44,49,39,56,34,53,46,42,50,36,29,32]
_SHIFTS = [1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1]
_SBOX = [
 [14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,
  0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
  4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,
  15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13],
 [15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,
  3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
  0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,
  13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9],
 [10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,
  13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
  13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,
  1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12],
 [7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,
  13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
  10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,
  3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14],
 [2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,
  14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
  4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,
  11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3],
 [12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,
  10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
  9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,
  4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13],
 [4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,
  13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
  1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,
  6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12],
 [13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,
  1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
  7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,
  2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11],
]


def _bits(data, n):
    return [(data >> (n - 1 - i)) & 1 for i in range(n)]


def _num(bits):
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v


def _des_subkeys(key8):
    k = _bits(int.from_bytes(key8, 'big'), 64)
    cd = [k[i - 1] for i in _PC1]
    c, d = cd[:28], cd[28:]
    out = []
    for s in _SHIFTS:
        c = c[s:] + c[:s]
        d = d[s:] + d[:s]
        cd = c + d
        out.append([cd[i - 1] for i in _PC2])
    return out


def _des_block(block8, subkeys):
    b = _bits(int.from_bytes(block8, 'big'), 64)
    b = [b[i - 1] for i in _IP]
    l, r = b[:32], b[32:]
    for sk in subkeys:
        e = [r[i - 1] for i in _E]
        x = [a ^ c for a, c in zip(e, sk)]
        o = []
        for i in range(8):
            six = x[i * 6:i * 6 + 6]
            row = (six[0] << 1) | six[5]
            col = _num(six[1:5])
            o += _bits(_SBOX[i][row * 16 + col], 4)
        f = [o[i - 1] for i in _P]
        l, r = r, [a ^ c for a, c in zip(l, f)]
    b = r + l
    b = [b[i - 1] for i in _FP]
    return _num(b).to_bytes(8, 'big')


def vnc_des_response(password, challenge):
    """RFB 3.8 sec. 7.2.2: DES-encrypt the 16-byte challenge with the
    password (truncated/zero-padded to 8 bytes, each byte bit-reversed)."""
    key = password.encode('latin-1')[:8].ljust(8, b'\0')
    key = bytes(int(f'{b:08b}'[::-1], 2) for b in key)
    sk = _des_subkeys(key)
    return _des_block(challenge[:8], sk) + _des_block(challenge[8:16], sk)


# ------------------------------------------------------------------ PNG writer

def write_png(path, w, h, rgb):
    def chunk(t, d):
        return (struct.pack('>I', len(d)) + t + d
                + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff))
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


# ------------------------------------------------------------------ RFB client

SEC_NONE, SEC_VNCAUTH, SEC_VENCRYPT = 1, 2, 19
VC_PLAIN, VC_X509NONE, VC_X509PLAIN = 256, 260, 262
SEC_NAMES = {1: 'None', 2: 'VncAuth', 19: 'VeNCrypt'}
VC_NAMES = {256: 'Plain', 257: 'TLSNone', 258: 'TLSVnc', 259: 'TLSPlain',
            260: 'X509None', 261: 'X509Vnc', 262: 'X509Plain'}

KEYSYMS = {
    'Return': 0xff0d, 'Enter': 0xff0d, 'Escape': 0xff1b, 'Tab': 0xff09,
    'BackSpace': 0xff08, 'Delete': 0xffff, 'Home': 0xff50, 'End': 0xff57,
    'Left': 0xff51, 'Up': 0xff52, 'Right': 0xff53, 'Down': 0xff54,
    'Shift_L': 0xffe1, 'Control_L': 0xffe3, 'Alt_L': 0xffe9,
    'Super_L': 0xffeb, 'Search': 0xffeb, 'space': 0x20,
    **{f'F{i}': 0xffbd + i for i in range(1, 13)},
}


class RfbError(Exception):
    pass


class RfbClient:
    def __init__(self, host, port, password='', security='auto',
                 timeout=15.0, tls_max=None):
        self.host, self.port = host, port
        self.password = password
        self.security = security
        self.timeout = timeout
        self.tls_max = tls_max
        self.sock = None
        self.width = self.height = 0
        self.name = ''
        self.pixfmt = None
        self.sectypes = []
        self.subtypes = []
        self.tls_fingerprint = ''

    # low-level ------------------------------------------------------------
    def _recv(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RfbError('connection closed by server')
            buf += chunk
        return buf

    def _send(self, data):
        self.sock.sendall(data)

    # handshake ------------------------------------------------------------
    def connect(self, negotiate_only=False):
        self.sock = socket.create_connection((self.host, self.port),
                                             self.timeout)
        self.sock.settimeout(self.timeout)
        ver = self._recv(12)
        if not ver.startswith(b'RFB '):
            raise RfbError(f'not an RFB server: {ver!r}')
        self._send(b'RFB 003.008\n')
        n = self._recv(1)[0]
        if n == 0:
            rlen = struct.unpack('>I', self._recv(4))[0]
            raise RfbError('server refused: '
                           + self._recv(rlen).decode('latin-1', 'replace'))
        self.sectypes = list(self._recv(n))
        choice = self._pick_sectype()
        if negotiate_only and choice == SEC_VENCRYPT:
            # continue far enough to learn the sub-types, then bail out
            self._send(bytes([SEC_VENCRYPT]))
            self._vencrypt_enumerate()
            return
        if negotiate_only:
            return
        self._send(bytes([choice]))
        if choice == SEC_NONE:
            self._security_result()
        elif choice == SEC_VNCAUTH:
            challenge = self._recv(16)
            self._send(vnc_des_response(self.password, challenge))
            self._security_result()
        elif choice == SEC_VENCRYPT:
            self._vencrypt()
        else:
            raise RfbError(f'unsupported security type {choice}')
        self._client_server_init()

    def _pick_sectype(self):
        pref = {'auto': [SEC_VENCRYPT, SEC_NONE, SEC_VNCAUTH],
                'vencrypt': [SEC_VENCRYPT],
                'none': [SEC_NONE],
                'vncauth': [SEC_VNCAUTH]}[self.security]
        for p in pref:
            if p in self.sectypes:
                return p
        raise RfbError(f'no acceptable security type in {self.sectypes}')

    def _vencrypt_enumerate(self):
        maj, mnr = self._recv(2)
        self._send(bytes([0, 2]))
        if self._recv(1)[0] != 0:
            raise RfbError('VeNCrypt version rejected')
        n = self._recv(1)[0]
        self.subtypes = [struct.unpack('>I', self._recv(4))[0]
                         for _ in range(n)]

    def _vencrypt(self):
        self._vencrypt_enumerate()
        if self.password and VC_X509PLAIN in self.subtypes:
            sub = VC_X509PLAIN
        elif not self.password and VC_X509NONE in self.subtypes:
            sub = VC_X509NONE
        elif VC_PLAIN in self.subtypes:
            sub = VC_PLAIN
        elif self.password and VC_X509NONE in self.subtypes:
            sub = VC_X509NONE   # password set locally but server has none
        else:
            raise RfbError(f'no usable VeNCrypt subtype in {self.subtypes}')
        self._send(struct.pack('>I', sub))
        # FydeOS quirk (matches noVNC): the server sends the 1-byte
        # accept only for the X509 sub-types, not for loopback Plain.
        if sub in (VC_X509NONE, VC_X509PLAIN):
            if self._recv(1)[0] != 1:
                raise RfbError(f'VeNCrypt subtype {sub} rejected')
            self._start_tls()
        if sub in (VC_PLAIN, VC_X509PLAIN):
            user = b''
            pw = self.password.encode('utf-8')
            self._send(struct.pack('>II', len(user), len(pw)) + user + pw)
        self._security_result()

    def _start_tls(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        if self.tls_max:
            ctx.maximum_version = {
                '1.0': ssl.TLSVersion.TLSv1,
                '1.1': ssl.TLSVersion.TLSv1_1,
                '1.2': ssl.TLSVersion.TLSv1_2,
                '1.3': ssl.TLSVersion.TLSv1_3}[self.tls_max]
            try:
                ctx.minimum_version = ctx.maximum_version
            except (ValueError, ssl.SSLError):
                pass
        self.sock = ctx.wrap_socket(self.sock)
        der = self.sock.getpeercert(binary_form=True)
        import hashlib
        fp = hashlib.sha256(der).hexdigest().upper()
        self.tls_fingerprint = ':'.join(fp[i:i + 2] for i in range(0, 64, 2))

    def _security_result(self):
        res = struct.unpack('>I', self._recv(4))[0]
        if res != 0:
            try:
                rlen = struct.unpack('>I', self._recv(4))[0]
                reason = self._recv(rlen).decode('latin-1', 'replace')
            except Exception:
                reason = ''
            raise RfbError(f'authentication failed {res} {reason}'.strip())

    def _client_server_init(self):
        self._send(b'\x01')                       # shared
        head = self._recv(24)
        self.width, self.height = struct.unpack('>HH', head[:4])
        self.pixfmt = self._parse_pixfmt(head[4:20])
        nlen = struct.unpack('>I', head[20:24])[0]
        self.name = self._recv(nlen).decode('utf-8', 'replace')

    @staticmethod
    def _parse_pixfmt(b):
        (bpp, depth, bigend, truecol, rmax, gmax, bmax,
         rsh, gsh, bsh) = struct.unpack('>BBBBHHHBBB', b[:13])
        return dict(bpp=bpp, depth=depth, bigend=bigend, truecol=truecol,
                    rmax=rmax, gmax=gmax, bmax=bmax,
                    rsh=rsh, gsh=gsh, bsh=bsh)

    # client messages ------------------------------------------------------
    def set_encodings(self, encs):
        self._send(struct.pack('>BxH', 2, len(encs))
                   + b''.join(struct.pack('>i', e) for e in encs))

    def fbur(self, incremental, x=0, y=0, w=None, h=None):
        self._send(struct.pack('>BBHHHH', 3, 1 if incremental else 0,
                               x, y, w or self.width, h or self.height))

    def key_event(self, keysym, down):
        self._send(struct.pack('>BBxxI', 4, 1 if down else 0, keysym))

    def pointer_event(self, x, y, mask):
        self._send(struct.pack('>BBHH', 5, mask & 0xff, x, y))

    def cut_text(self, text):
        data = text.encode('latin-1', 'replace')
        self._send(struct.pack('>BxxxI', 6, len(data)) + data)

    # server messages ------------------------------------------------------
    def read_update(self, fb=None):
        """Read one server message. Returns ('update', nrects, bytes) for a
        FramebufferUpdate (rects painted into fb if given), or a tuple for
        other message types."""
        t = self._recv(1)[0]
        if t == 0:                                # FramebufferUpdate
            self._recv(1)
            nrects = struct.unpack('>H', self._recv(2))[0]
            total = 0
            for _ in range(nrects):
                x, y, w, h = struct.unpack('>HHHH', self._recv(8))
                enc = struct.unpack('>i', self._recv(4))[0]
                if enc == 0:                      # Raw
                    need = w * h * (self.pixfmt['bpp'] // 8)
                    data = self._recv(need)
                    total += need
                    if fb is not None:
                        self._paint(fb, x, y, w, h, data)
                elif enc == -239 or enc == -240:  # cursor pseudo — skip
                    pass
                else:
                    raise RfbError(f'unexpected encoding {enc} '
                                   '(client only requests Raw)')
            return ('update', nrects, total)
        if t == 1:                                # SetColourMapEntries
            self._recv(1)
            first, n = struct.unpack('>HH', self._recv(4))
            self._recv(n * 6)
            return ('colormap', n)
        if t == 2:                                # Bell
            return ('bell',)
        if t == 3:                                # ServerCutText
            self._recv(3)
            ln = struct.unpack('>i', self._recv(4))[0]
            data = self._recv(abs(ln))
            return ('cuttext', ln, data)
        raise RfbError(f'unknown server message type {t}')

    def _paint(self, fb, x, y, w, h, data):
        f = self.pixfmt
        step = f['bpp'] // 8
        endian = 'big' if f['bigend'] else 'little'
        for row in range(h):
            for col in range(w):
                off = (row * w + col) * step
                v = int.from_bytes(data[off:off + step], endian)
                r = (v >> f['rsh']) & f['rmax']
                g = (v >> f['gsh']) & f['gmax']
                b = (v >> f['bsh']) & f['bmax']
                if f['rmax'] and f['rmax'] != 255:
                    r = r * 255 // f['rmax']
                    g = g * 255 // f['gmax']
                    b = b * 255 // f['bmax']
                i = ((y + row) * self.width + (x + col)) * 3
                fb[i:i + 3] = bytes((r, g, b))

    def grab_frame(self):
        self.set_encodings([0])                   # Raw only
        fb = bytearray(self.width * self.height * 3)
        self.fbur(False)
        covered = 0
        deadline = time.time() + self.timeout
        while covered == 0 and time.time() < deadline:
            msg = self.read_update(fb)
            if msg[0] == 'update' and msg[1] > 0:
                covered = 1
        return fb


def resolve_keysym(s):
    if s in KEYSYMS:
        return KEYSYMS[s]
    if s.startswith('0x'):
        return int(s, 16)
    if s.isdigit() and len(s) > 1:
        return int(s)
    if len(s) == 1:
        return ord(s)
    raise SystemExit(f'unknown keysym: {s}')


def frame_stats(fb):
    colors = set()
    for i in range(0, len(fb), 3):
        colors.add(bytes(fb[i:i + 3]))
        if len(colors) > 4096:
            break
    return len(colors)


def selftest():
    # Classic DES vector (worked example): K=133457799BBCDFF1,
    # P=0123456789ABCDEF -> C=85E813540F0AB405
    sk = _des_subkeys(bytes.fromhex('133457799BBCDFF1'))
    c = _des_block(bytes.fromhex('0123456789ABCDEF'), sk)
    assert c == bytes.fromhex('85E813540F0AB405'), c.hex()
    # VncAuth reference: password "test", challenge of zeros must be stable
    r = vnc_des_response('test', b'\x00' * 16)
    assert len(r) == 16 and r[:8] == r[8:], r.hex()
    # PNG writer round-trips a header
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.png') as f:
        write_png(f.name, 2, 2, bytes(range(12)))
        head = open(f.name, 'rb').read(8)
        assert head == b'\x89PNG\r\n\x1a\n'
    print('selftest ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=5900)
    ap.add_argument('--password', default=os.environ.get('VNC_PASSWORD', ''))
    ap.add_argument('--security', default='auto',
                    choices=['auto', 'vencrypt', 'none', 'vncauth'])
    ap.add_argument('--timeout', type=float, default=15.0)
    ap.add_argument('--tls-max', choices=['1.0', '1.1', '1.2', '1.3'],
                    help='cap TLS version (for min-version tests)')
    ap.add_argument('cmd', nargs='+')
    a = ap.parse_args()
    cmd, args = a.cmd[0], a.cmd[1:]

    if cmd == 'selftest':
        selftest()
        return

    c = RfbClient(a.host, a.port, a.password, a.security,
                  a.timeout, a.tls_max)

    if cmd == 'sectypes':
        c.connect(negotiate_only=True)
        types = ','.join(f'{t}({SEC_NAMES.get(t, "?")})' for t in c.sectypes)
        print(f'sectypes={types}')
        if c.subtypes:
            subs = ','.join(f'{t}({VC_NAMES.get(t, "?")})' for t in c.subtypes)
            print(f'vencrypt_subtypes={subs}')
        return

    c.connect()

    if cmd == 'fingerprint':
        if not c.tls_fingerprint:
            raise SystemExit('no TLS negotiated (non-TLS security path)')
        print(c.tls_fingerprint)
    elif cmd == 'probe':
        print(f'name={c.name}')
        print(f'geometry={c.width}x{c.height}')
        print(f'pixfmt=bpp{c.pixfmt["bpp"]} depth{c.pixfmt["depth"]} '
              f'shifts r{c.pixfmt["rsh"]} g{c.pixfmt["gsh"]} b{c.pixfmt["bsh"]}')
        if c.tls_fingerprint:
            print(f'tls_fingerprint={c.tls_fingerprint}')
    elif cmd == 'screenshot':
        out = args[0] if args else 'screenshot.png'
        fb = c.grab_frame()
        write_png(out, c.width, c.height, bytes(fb))
        print(f'file={out}')
        print(f'geometry={c.width}x{c.height}')
        print(f'unique_colors={frame_stats(fb)}')
    elif cmd == 'key':
        ks = resolve_keysym(args[0])
        mode = args[1] if len(args) > 1 else 'press'
        if mode in ('down', 'press'):
            c.key_event(ks, True)
        if mode in ('up', 'press'):
            time.sleep(0.05)
            c.key_event(ks, False)
        time.sleep(0.1)
        print(f'sent key 0x{ks:x} {mode}')
    elif cmd == 'type':
        for ch in ' '.join(args):
            c.key_event(ord(ch), True)
            time.sleep(0.02)
            c.key_event(ord(ch), False)
            time.sleep(0.02)
        print('typed')
    elif cmd == 'pointer':
        x, y = int(args[0]), int(args[1])
        mask = int(args[2], 0) if len(args) > 2 else 0
        c.pointer_event(x, y, mask)
        time.sleep(0.1)
        print(f'pointer {x},{y} mask={mask:#x}')
    elif cmd == 'click':
        x, y = int(args[0]), int(args[1])
        btn = int(args[2]) if len(args) > 2 else 1
        mask = {1: 1, 2: 2, 3: 4}[btn]
        c.pointer_event(x, y, 0)
        time.sleep(0.05)
        c.pointer_event(x, y, mask)
        time.sleep(0.05)
        c.pointer_event(x, y, 0)
        time.sleep(0.1)
        print(f'clicked {x},{y} button={btn}')
    elif cmd == 'wheel':
        x, y = int(args[0]), int(args[1])
        mask = 8 if args[2] == 'up' else 16
        n = int(args[3]) if len(args) > 3 else 3
        for _ in range(n):
            c.pointer_event(x, y, mask)
            time.sleep(0.03)
            c.pointer_event(x, y, 0)
            time.sleep(0.03)
        print(f'wheel {args[2]} x{n}')
    elif cmd == 'cuttext':
        c.cut_text(' '.join(args))
        time.sleep(0.2)
        print('cuttext sent')
    elif cmd == 'watch':
        secs = float(args[0]) if args else 10.0
        c.set_encodings([0])
        fb = bytearray(c.width * c.height * 3)
        end = time.time() + secs
        updates = rects = total = 0
        c.fbur(False)
        c.sock.settimeout(1.0)
        while time.time() < end:
            try:
                msg = c.read_update(fb)
            except (socket.timeout, TimeoutError):
                continue
            if msg[0] == 'update':
                updates += 1
                rects += msg[1]
                total += msg[2]
                c.fbur(True)
        print(f'updates={updates} rects={rects} bytes={total} '
              f'fps={updates / secs:.1f}')
    else:
        raise SystemExit(f'unknown command: {cmd}')


if __name__ == '__main__':
    try:
        main()
    except RfbError as e:
        print(f'RFB-ERROR: {e}', file=sys.stderr)
        sys.exit(2)
