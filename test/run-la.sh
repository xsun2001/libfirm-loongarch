#!/bin/bash

loongarch64-unknown-linux-gnu-gcc -O0 -g -Wall -march=loongarch64 -o "$1.out" "$1.c"
loongarch64-unknown-linux-gnu-gcc -O0 -S -fverbose-asm -march=loongarch64 -o "$1.asm" "$1.c"
loongarch64-unknown-linux-gnu-objdump -d "$1.out" > "$1.s"
qemu-loongarch64 -L "/data/xcx/opt/x-tools/loongarch64-unknown-linux-gnu/loongarch64-unknown-linux-gnu/sysroot" "$1.out"