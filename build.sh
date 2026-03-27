#!/bin/sh
# Build script for toon CLI - SAS/C 6.58 via vamos
set -e

SC="${SC:-/Users/brie/sasc658}"
SRC="$(cd "$(dirname "$0")" && pwd)"
CFG="${SRC}/vamos_build.cfg"

compile() {
    local f="$1"
    echo "Compiling $f..."
    rm -rf ~/.vamos/volumes/ram
    vamos -c "${CFG}" \
        -V sc:"${SC}" \
        -V src:"${SRC}" \
        sc:c/sc "src:src/$f" \
        DATA=FARONLY NOSTACKCHECK NOCHKABORT NOICONS \
        IDIR=src:src IDIR=sc:include IDLEN=80 \
        OBJNAME=src:src/ 2>&1 | grep -v "WARNING:"
}

link() {
    echo "Linking toon..."
    rm -rf ~/.vamos/volumes/ram
    vamos -c "${CFG}" \
        -V sc:"${SC}" \
        -V src:"${SRC}" \
        sc:c/slink \
        sc:lib/c.o \
        src:src/main.o \
        src:src/toon_decode.o \
        src:src/toon_encode.o \
        src:src/json_parse.o \
        src:src/json_emit.o \
        src:src/toon_util.o \
        TO src:src/toon \
        LIB sc:lib/scnb.lib sc:lib/scmnb.lib sc:lib/amiga.lib \
        NOICONS 2>&1 | grep -v "WARNING:"
}

# Compile all source files
for f in toon_util.c json_parse.c json_emit.c toon_decode.c toon_encode.c main.c; do
    compile "$f"
done

link

echo ""
echo "Build complete: src/toon"
echo ""
echo "Run with:"
echo "  vamos -S -C 68020 -m 8192 -H emu -s 512 \\"
echo "    -V sc:${SC} -V src:${SRC} \\"
echo "    -- src:src/toon [command] [args...]"
