#!/bin/bash
set -eu
usage() {
    echo "usage: setup-stray.sh <game app0 folder> <new run folder> [AnyKyty build folder, default ./build]" >&2
    exit 2
}
[ $# -ge 2 ] || usage
game=$(realpath "$1")
run=$2
build=$(realpath "${3:-build}")
relinker="$build/core/relinker/relinker"
libs="$build/core/libs/libs"
[ -f "$game/eboot.bin" ] || { echo "no eboot.bin in $game" >&2; exit 1; }
[ -d "$game/sce_module" ] || { echo "no sce_module folder in $game" >&2; exit 1; }
[ -x "$relinker" ] || { echo "no relinker at $relinker: build the kyty_gpu, libs and relinker targets first" >&2; exit 1; }
[ -d "$libs" ] || { echo "no libraries at $libs" >&2; exit 1; }
[ ! -e "$run" ] || { echo "$run already exists" >&2; exit 1; }

mkdir -p "$run/source/sce_module" "$run/app0/sce_module" "$run/download0"
run=$(realpath "$run")
cp --reflink=auto "$game/eboot.bin" "$run/source/input.elf"
for module in "$game"/sce_module/*.prx; do
    case "$module" in *.guest.prx) continue ;; esac
    cp --reflink=auto "$module" "$run/source/sce_module/"
    ln -s "$module" "$run/app0/sce_module/$(basename "$module")"
done
for entry in "$game"/* "$game"/.[!.]*; do
    [ -e "$entry" ] || continue
    [ "$(basename "$entry")" = sce_module ] && continue
    ln -s "$entry" "$run/app0/$(basename "$entry")"
done
(cd "$run" && "$relinker" source/input.elf eboot > relink.log 2>&1) || { echo "relinking failed, see $run/relink.log" >&2; exit 1; }
ln -s "$libs" "$run/libs"
echo "ready: $run (start it with tools/stray/play-stray.sh $run)"
