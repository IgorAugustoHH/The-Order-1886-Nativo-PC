"""BB_ORDER_RESOLUTION=1080: The Order 01.02 picks 1280x720 unless its (developer) resolution
flags select another size (0x44cedc: flags bit 7 = custom size, bit 8 = 1920x1080, bit 9 = 4K).
Both branches are forced to the 1920x1080 case in the loaded image; files stay intact."""
import hashlib
import struct

ELF_SHA256 = '528b8d0e1d185790faf4edba702119e64a98bbceb2a7c0fd66d031b0ac27fbe3'
# (image offset, original bytes, replacement)
WRITES_1080 = [
    (0x44cef3, bytes.fromhex('a8 80 75 41'), bytes.fromhex('a8 80 eb 41')),  # always custom size
    (0x44cf38, bytes.fromhex('f6 c4 01 74 3e'), bytes.fromhex('f6 c4 01 90 90')),  # 1920x1080
]


def resolution_writes(elf, mode):
    if mode != '1080':
        raise ValueError(f'BB_ORDER_RESOLUTION: modo nao suportado: {mode}')
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        raise ValueError('Resolucao: eboot diferente da imagem analisada (CUSA00785 01.02)')
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    entry_size, count = struct.unpack_from('<HH', elf, 0x36)
    out = []
    for target, original, replacement in WRITES_1080:
        for index in range(count):
            kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
                '<IIQQQQQQ', elf, phoff + index * entry_size)
            if kind == 1 and flags & 1 and address <= target and target + len(original) <= address + size:
                start = offset + target - address
                if elf[start:start + len(original)] != original:
                    raise ValueError(f'Resolucao: bytes originais divergem em {target:#x}')
                out.append((target, replacement))
                break
        else:
            raise ValueError(f'Resolucao: {target:#x} fora de segmento executavel')
    print('The Order: resolucao forcada para 1920x1080 (2 escritas)')
    return out
