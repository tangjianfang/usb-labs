# -*- coding: utf-8 -*-
# USB2.0 spec 8.3.5: shift register seeded all-ones, data bits fed in wire order,
# remainder inverted, sent MSb first. Residual: CRC5=0b01100, CRC16=0x800D.
# Temporary generator for teaching-sample hex; verified against spec residuals
# and catalog CRC-16/USB check value 0xB4C8 ("123456789").

def rev(b, n):
    r = 0
    for i in range(n):
        r = (r << 1) | ((b >> i) & 1)
    return r

def crc5_bits(bits):
    crc = 0x1F
    for b in bits:
        fb = ((crc >> 4) & 1) ^ b
        crc = (crc << 1) & 0x1F
        if fb: crc ^= 0x05
    return (~crc) & 0x1F

def crc5_token_bits(addr, endp):
    bits = [(addr >> i) & 1 for i in range(7)] + [(endp >> i) & 1 for i in range(4)]
    c = crc5_bits(bits)
    wire = bits + [(c >> i) & 1 for i in range(4, -1, -1)]  # CRC MSb first
    chk = 0x1F
    for b in wire:
        fb = ((chk >> 4) & 1) ^ b
        chk = (chk << 1) & 0x1F
        if fb: chk ^= 0x05
    assert chk == 0x0C, hex(chk)
    return wire  # 16 bits

def token_bytes(pid, addr, endp):
    # wire bits after PID: ADDR bit0..6, ENDP bit0..3, then CRC5 MSb first;
    # byte k bit i = wire bit (8k+i)  -> plain LSB-first packing, no reversal
    w = crc5_token_bits(addr, endp)
    return [pid, sum(b << i for i, b in enumerate(w[0:8])),
            sum(b << i for i, b in enumerate(w[8:16]))]

def sof_bytes(frame):
    bits = [(frame >> i) & 1 for i in range(11)]
    c = crc5_bits(bits)
    wire = bits + [(c >> i) & 1 for i in range(4, -1, -1)]
    return [0xA5, sum(b << i for i, b in enumerate(wire[0:8])),
            sum(b << i for i, b in enumerate(wire[8:16]))]

def crc16_ref(data):  # catalog CRC-16/USB cross-check
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF

def crc16(data):
    crc = 0xFFFF
    bits = []
    for b in data:
        for i in range(8):
            x = (b >> i) & 1
            bits.append(x)
            fb = ((crc >> 15) & 1) ^ x
            crc = (crc << 1) & 0xFFFF
            if fb: crc ^= 0x8005
    c = (~crc) & 0xFFFF
    wire = bits + [(c >> (15 - i)) & 1 for i in range(16)]  # MSb first
    chk = 0xFFFF
    for b in wire:
        fb = ((chk >> 15) & 1) ^ b
        chk = (chk << 1) & 0xFFFF
        if fb: chk ^= 0x8005
    assert chk == 0x800D, hex(chk)
    return c

def data_bytes(pid, payload):
    c = crc16(payload)
    b0, b1 = rev(c >> 8, 8), rev(c & 0xFF, 8)
    r = crc16_ref(payload)
    assert b0 == (r & 0xFF) and b1 == (r >> 8), (hex(c), hex(r))
    return [pid] + list(payload) + [b0, b1]

def h(bs): return ' '.join(f'{b:02X}' for b in bs)

if crc16_ref(b'123456789') == 0xB4C8: print("catalog check 0xB4C8 OK")
print("SOF f100 :", h(sof_bytes(100)), " f101:", h(sof_bytes(101)), " f107:", h(sof_bytes(107)), " f108:", h(sof_bytes(108)), " f116:", h(sof_bytes(116)))
print("IN  a0e0 :", h(token_bytes(0x69, 0, 0)), " SETUP a0e0:", h(token_bytes(0x2D, 0, 0)), " OUT a0e0:", h(token_bytes(0xE1, 0, 0)))
print("IN  a5e0 :", h(token_bytes(0x69, 5, 0)), " OUT a5e0:", h(token_bytes(0xE1, 5, 0)), " IN a5e1:", h(token_bytes(0x69, 5, 1)), " OUT a5e1:", h(token_bytes(0xE1, 5, 1)), " IN a5e2:", h(token_bytes(0x69, 5, 2)))

dev_desc = bytes([0x12,0x01,0x00,0x02,0x00,0x00,0x00,0x40,0x41,0x23,0x01,0x00,0x00,0x01,0x01,0x02,0x00,0x01])
cfg = bytes([0x09,0x02,0x22,0x00,0x01,0x01,0x00,0xA0,0x32,
             0x09,0x04,0x00,0x00,0x01,0x03,0x01,0x02,0x00,
             0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x5B,0x00,
             0x07,0x05,0x81,0x03,0x08,0x00,0x01])
rep = bytes([0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,
             0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,0x95,0x05,0x81,0x03,
             0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,0x81,0x06,
             0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x01,0x81,0x06,0x05,0x0C,0x0A,0x38,0x02,
             0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x01,0x81,0x06,0x05,0x09,0x19,0x04,0x29,0x05,
             0x75,0x01,0x95,0x02,0x81,0x02,0x95,0x06,0x81,0x03,0xC0,0xC0])
assert len(rep) == 0x5B, len(rep)
assert len(cfg) == 0x22, len(cfg)
langid = bytes([0x04,0x03,0x09,0x04])
s1 = 'USBLabs'.encode('utf-16-le'); str1 = bytes([len(s1)+2,0x03]) + s1
s2 = 'Lab Mouse'.encode('utf-16-le'); str2 = bytes([len(s2)+2,0x03]) + s2

reqs = {
 'getdev64':  bytes([0x80,0x06,0x00,0x01,0x00,0x00,0x40,0x00]),
 'setaddr5':  bytes([0x00,0x05,0x05,0x00,0x00,0x00,0x00,0x00]),
 'getdev18':  bytes([0x80,0x06,0x00,0x01,0x00,0x00,0x12,0x00]),
 'getcfg9':   bytes([0x80,0x06,0x00,0x02,0x00,0x00,0x09,0x00]),
 'getcfgfull':bytes([0x80,0x06,0x00,0x02,0x00,0x00,0x22,0x00]),
 'getlangid': bytes([0x80,0x06,0x00,0x03,0x00,0x00,0x04,0x00]),
 'getstr1':   bytes([0x80,0x06,0x01,0x03,0x09,0x04,0x10,0x00]),
 'getstr2':   bytes([0x80,0x06,0x02,0x03,0x09,0x04,0x14,0x00]),
 'setcfg1':   bytes([0x00,0x09,0x01,0x00,0x00,0x00,0x00,0x00]),
 'getstatus': bytes([0x80,0x00,0x00,0x00,0x00,0x00,0x02,0x00]),
 'getrep':    bytes([0x81,0x06,0x00,0x22,0x00,0x00,0x5B,0x00]),
 'clearhalt_in1': bytes([0x02,0x01,0x00,0x00,0x81,0x00,0x00,0x00]),
}
for k, v in reqs.items():
    print(f"SETUP {k:12s} DATA0 {h(data_bytes(0xC3, v))}")

for k, v in [('dev18', dev_desc), ('cfg9', cfg[:9]), ('cfg34', cfg), ('rep91', rep),
             ('langid4', langid), ('str1', str1), ('str2', str2)]:
    print(f"DATA1 {k:8s} len={len(v):3d} {h(data_bytes(0x4B, v))}")
print("str1 bytes:", h(str1), " str2:", h(str2))

for k, v in [('r1', [0x01,0x7F,0x00,0x00,0x00,0x00]), ('r2', [0x01,0x49,0x00,0x00,0x00,0x00]),
             ('r3', [0x00,0x00,0x00,0x00,0x00,0x00]), ('wheel', [0x00,0x00,0x00,0xFF,0x00,0x00])]:
    print(f"DATA0 {k:6s} {h(data_bytes(0xC3, bytes(v)))}")
    print(f"DATA1 {k:6s} {h(data_bytes(0x4B, bytes(v)))}")

def cbw(tag, dlen, flags, lun, cdb):
    return bytes([0x55,0x53,0x42,0x43]) + tag.to_bytes(4,'little') + dlen.to_bytes(4,'little') \
        + bytes([flags, lun, len(cdb)]) + cdb.ljust(16, b'\x00')

read10 = cbw(1, 512, 0x80, 0, bytes([0x28,0x00,0x00,0x00,0x03,0xE8,0x00,0x00,0x01,0x00]))
print("CBW READ10    DATA0", h(data_bytes(0xC3, read10)))
read10_bad = cbw(1, 512, 0x80, 0, bytes([0x28,0x00,0x0F,0xFF,0xFF,0xFF,0x00,0x00,0x01,0x00]))
print("CBW READ10BAD DATA0", h(data_bytes(0xC3, read10_bad)))
req_sense = cbw(2, 18, 0x80, 0, bytes([0x03,0,0,0,18,0]))
print("CBW REQSENSE  DATA0", h(data_bytes(0xC3, req_sense)))
sense = bytearray(18); sense[0]=0x70; sense[2]=0x05; sense[7]=0x0A; sense[12]=0x21; sense[13]=0x00
print("SENSE18(SPC)  DATA0", h(data_bytes(0xC3, bytes(sense))))
sense_ufi = bytearray(18); sense_ufi[0]=0x70; sense_ufi[2]=0x05; sense_ufi[8]=0x0A; sense_ufi[11]=0x21; sense_ufi[12]=0x00
print("SENSE18(UFI)  DATA0", h(data_bytes(0xC3, bytes(sense_ufi))))
def csw(tag, res, st): return bytes([0x55,0x53,0x42,0x53]) + tag.to_bytes(4,'little') + res.to_bytes(4,'little') + bytes([st])
print("CSW ok tag1   DATA1", h(data_bytes(0x4B, csw(1, 0, 0))))
print("CSW fail tag1 DATA1", h(data_bytes(0x4B, csw(1, 0, 1))))
print("CSW ok tag2   DATA1", h(data_bytes(0x4B, csw(2, 0, 0))))
sector = bytearray(512)
sector[:16] = bytes([0xEB,0x52,0x90,0x4D,0x53,0x44,0x4F,0x53,0x35,0x2E,0x30,0x00,0x02,0x08,0x20,0x00])
print("SECTOR512     DATA0", h(data_bytes(0xC3, bytes(sector))))

audio = bytearray(192)
audio[:8] = bytes([0x00,0x10,0x00,0xF0,0x00,0x10,0x00,0xF0])
print("AUDIO192      DATA0", h(data_bytes(0xC3, bytes(audio))), "<- crc tail 2 bytes")
fb = lambda v: data_bytes(0xC3, v.to_bytes(3,'little'))  # iso: DATA0, no toggle
print("FB 0x0C0000 DATA0", h(fb(0x0C0000)), " 0x0C004E:", h(fb(0x0C004E)), " 0xB0666:", h(fb(0xB0666)), " 0xB0667:", h(fb(0xB0667)))
print("10.14: 48.0 ->", hex(48<<14), " 44.1*2^14 =", 44.1*(1<<14), hex(int(44.1*(1<<14))), " 48*1.0001*2^14 =", 48.0048*(1<<14), hex(int(48.0048*(1<<14))))

def hdr(ndo, mid, prole, rev, drole, mtype, ext=0):
    return (ext<<15)|(ndo<<12)|(mid<<9)|(prole<<8)|(rev<<6)|(drole<<5)|mtype
H = {
 'src_cap':  hdr(5,0,1,2,1,1), 'gc_snk0': hdr(0,0,0,2,0,1), 'request': hdr(1,0,0,2,0,2),
 'gc_src0':  hdr(0,0,1,2,1,1), 'accept':   hdr(0,1,1,2,1,3), 'gc_snk1': hdr(0,1,0,2,0,1),
 'psrdy':    hdr(0,2,1,2,1,6), 'gc_src2':  hdr(0,2,1,2,1,1), 'gc_snk2': hdr(0,2,0,2,0,1),
}
for k,v in H.items(): print(f"PD {k:8s} = 0x{v:04X}  {v:016b}")
def fixed_pdo(mv, ma, drp=0, susp=0, uncon=0, comm=0, drd=0, unch=0, peak=0):
    return (0<<30)|(drp<<29)|(susp<<28)|(uncon<<27)|(comm<<26)|(drd<<25)|(unch<<24)|(peak<<20)|((mv//50)<<10)|(ma//10)
pdos = [fixed_pdo(5000,3000,uncon=1,comm=1,drd=1,unch=1), fixed_pdo(9000,3000), fixed_pdo(12000,3000), fixed_pdo(15000,3000), fixed_pdo(20000,3250)]
for i,p in enumerate(pdos): print(f"PDO{i} = 0x{p:08X}  {p:032b}")
rdo = (5<<28)|(0<<27)|(0<<25)|(1<<24)|(1<<23)|(0<<22)|(250<<10)|325
print(f"RDO  = 0x{rdo:08X}  {rdo:032b}")
