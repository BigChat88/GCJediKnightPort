#!/bin/bash
# Build apploader.img (header + code) for make_gcm.py. Needs devkitPPC (run it in
# the openjkdf2-gamecube Docker image). Usage: build.sh [output.img]
set -e
cd "$(dirname "$0")"
OUT="${1:-apploader.img}"
BIN="${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin"

"$BIN/powerpc-eabi-gcc" -mcpu=750 -meabi -mhard-float -msdata=none -G 0 -O2 \
    -ffreestanding -fno-builtin -nostdlib -Wall \
    -Wl,-T,apploader.ld -Wl,--gc-sections -o apploader.elf apploader.c
"$BIN/powerpc-eabi-objcopy" -O binary apploader.elf apploader.bin

ENTRY=$("$BIN/powerpc-eabi-nm" apploader.elf | awk '$3 == "apploaderEntry" { print $1 }')
python3 - "$ENTRY" apploader.bin "$OUT" <<'EOF'
import struct, sys
entry = int(sys.argv[1], 16)
code = open(sys.argv[2], "rb").read()
code += b"\0" * (-len(code) % 32)
hdr = b"2026/10/01".ljust(16, b"\0") + struct.pack(">III", entry, len(code), 0) + b"\0" * 4
open(sys.argv[3], "wb").write(hdr + code)
print(f"{sys.argv[3]}: entry 0x{entry:08x}, {len(code)} bytes of code")
EOF
rm -f apploader.bin
