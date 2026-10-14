"""BB_ORDER_RESOLUTION=1080: The Order 01.02 picks 1280x720 unless its (developer) resolution
flags select another size (0x44cedc: flags bit 7 = custom size, bit 8 = 1920x1080, bit 9 = 4K).
Both branches are forced to the 1920x1080 case in the loaded image; files stay intact.
BB_ORDER_SCENE_RECT=WxH (experiment): keeps the 1920x1080 size and shrinks only the rectangle
(x, y, width, height at -0x430..-0x424) the same branch writes."""
import hashlib
import os
import struct

ELF_SHA256 = '528b8d0e1d185790faf4edba702119e64a98bbceb2a7c0fd66d031b0ac27fbe3'
# (image offset, original bytes, replacement)
WRITES_1080 = [
    (0x44cef3, bytes.fromhex('a8 80 75 41'), bytes.fromhex('a8 80 eb 41')),  # always custom size
    (0x44cf38, bytes.fromhex('f6 c4 01 74 3e'), bytes.fromhex('f6 c4 01 90 90')),  # 1920x1080
]
# movl $imm32, -0x428(%rbp) / -0x424(%rbp): the rectangle's width and height
RECT_WIDTH = (0x44cf65, bytes.fromhex('c7 85 d8 fb ff ff 80 07 00 00'))
RECT_HEIGHT = (0x44cf6f, bytes.fromhex('c7 85 dc fb ff ff 38 04 00 00'))
RECT_Y = (0x44cf5b, bytes.fromhex('c7 85 d4 fb ff ff 00 00 00 00'))  # -0x42c(%rbp)


def elf_offset(elf, target, length):
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    entry_size, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + index * entry_size)
        if kind == 1 and flags & 1 and address <= target and target + length <= address + size:
            return offset + target - address
    raise ValueError(f'Resolucao: {target:#x} fora de segmento executavel')


def resolution_writes(elf, mode):
    if mode != '1080':
        raise ValueError(f'BB_ORDER_RESOLUTION: modo nao suportado: {mode}')
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        raise ValueError('Resolucao: eboot diferente da imagem analisada (CUSA00785 01.02)')
    out = []
    for target, original, replacement in WRITES_1080:
        start = elf_offset(elf, target, len(original))
        if elf[start:start + len(original)] != original:
            raise ValueError(f'Resolucao: bytes originais divergem em {target:#x}')
        out.append((target, replacement))
    print('The Order: resolucao forcada para 1920x1080 (2 escritas)')
    rect = os.environ.get('BB_ORDER_SCENE_RECT')
    if rect:
        # WxH or WxH+Y (vertical offset, e.g. 1920x800+140 for the PS4's 2.40:1 letterbox).
        size, _, y = rect.lower().partition('+')
        w, h = (int(v) for v in size.split('x'))
        fields = [(RECT_WIDTH, w), (RECT_HEIGHT, h)]
        if y:
            fields.append((RECT_Y, int(y)))
        for (target, original), value in fields:
            start = elf_offset(elf, target, len(original))
            if elf[start:start + len(original)] != original:
                raise ValueError(f'Resolucao: instrucao do retangulo diverge em {target:#x}')
            out.append((target + 6, struct.pack('<I', value)))
        print(f'The Order: EXPERIMENTO retangulo {w}x{h} dentro de 1920x1080')
    return out
