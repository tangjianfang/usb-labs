# 一次性校验器: 检查 captures/0*.md 中所有 [SYNC] 包字节的 CRC 与 token 正确性
# 规则: [SYNC] 开头的包, 其后续行仅吸收"纯 ASCII + 空白分隔 hex + 可选 ;注释"的行;
#        含中文/省略号/制表装饰的行视为注释结束(省略字节的两包会被跳过并标记)。
import io, re, importlib.util, contextlib, glob, os
spec = importlib.util.spec_from_file_location('m', 'tools/_usbcalc.py')
m = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(m)

CONT = re.compile(r'^\s+((?:[0-9A-F]{2}\s+)+[0-9A-F]{2})\s*(?:;.*)?$')
NONASCII = re.compile(r'[^\x00-\x7F]')

def packets_of(path):
    lines = io.open(path, encoding='utf-8').read().splitlines()
    out, i = [], 0
    while i < len(lines):
        mo = re.match(r'^\s*\[SYNC\] ((?:[0-9A-F]{2} )+[0-9A-F]{2})\s*(?:;.*)?$', lines[i])
        if not mo:
            i += 1
            continue
        parts, j, elided = [mo.group(1)], i + 1, False
        while j < len(lines):
            if lines[j].lstrip().startswith('[SYNC]') or lines[j].startswith('```'):
                break
            c = CONT.match(lines[j])
            if c:
                parts.append(c.group(1))
                j += 1
            elif NONASCII.search(lines[j]):
                if '字节' in lines[j] and ('00' in lines[j] or '略' in lines[j]):
                    elided = True  # 省略字节行(03 扇区/04 音频), 无法机械校验
                break
            else:
                break
        out.append((i + 1, bytes.fromhex(''.join(p.replace(' ', '') for p in parts)), elided))
        i = j
    return out

tok_ok = tok_bad = dat_ok = dat_bad = skipped = 0
for f in sorted(glob.glob('captures/0*.md')):
    all_lines = io.open(f, encoding='utf-8').read().splitlines()
    for ln, bs, elided in packets_of(f):
        pid = bs[0]
        line = all_lines[ln - 1]
        if pid in (0xC3, 0x4B):
            if elided:
                skipped += 1
                print(f'SKIP(elided) {f}:{ln}')
                continue
            if len(bs) < 3:
                continue
            payload, crc = bs[1:-2], bs[-2:]
            calc = m.crc16(payload)
            wire = bytes([m.rev(calc >> 8, 8), m.rev(calc & 0xFF, 8)])
            if wire == crc:
                dat_ok += 1
            else:
                dat_bad += 1
                print(f'DATA MISMATCH {f}:{ln} wire={wire.hex().upper()} file={crc.hex().upper()}')
        elif pid in (0x69, 0xE1, 0x2D, 0xA5):
            if len(bs) != 3:
                continue
            if pid == 0xA5:
                fm = re.search(r'帧(\d+)', line)
                if not fm:
                    continue
                exp = bytes(m.sof_bytes(int(fm.group(1))))
            else:
                am = re.search(r'ADDR=(\d+)[^\d]*ENDP=(\d+)', line)
                if am:
                    addr, endp = int(am.group(1)), int(am.group(2))
                else:
                    am = re.search(r'ADDR=(\d+)', line)
                    if not am:
                        continue
                    addr, endp = int(am.group(1)), 0
                exp = bytes(m.token_bytes(pid, addr, endp))
            if exp == bs:
                tok_ok += 1
            else:
                tok_bad += 1
                print(f'TOKEN MISMATCH {f}:{ln} file={bs.hex().upper()} expect={exp.hex().upper()}')
print(f'FINAL: DATA ok={dat_ok} bad={dat_bad} skipped={skipped} | TOKEN ok={tok_ok} bad={tok_bad}')
