#!/bin/bash

CROSS_PREFIX="loongarch64-unknown-linux-gnu"
CROSS_SYSROOT="/data/xcx/opt/x-tools/loongarch64-unknown-linux-gnu/loongarch64-unknown-linux-gnu/sysroot"

cmake --build ../../build

rm -rf $1
mkdir $1
cd $1

if [[ -z "$CROSS_PREFIX" ]]; then
    ../../../build/cparser -mdump=all -O0 -S ../$1.c -o $1.s
    ../../../build/cparser -O0 ../$1.c -o $1.o
    objdump -d $1.o > $1.asm.s
    ./$1.o
else
    ../../../build/cparser --target=$CROSS_PREFIX -mdump=all -O0 -S ../$1.c -o $1.s
    ../../../build/cparser --target=$CROSS_PREFIX -O0 ../$1.c -o $1.o
    $CROSS_PREFIX-objdump -d $1.o > $1.asm.s
    qemu-loongarch64 -L $CROSS_SYSROOT $1.o
fi

echo $?
cd ..