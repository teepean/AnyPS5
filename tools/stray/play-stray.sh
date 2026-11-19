#!/bin/sh
[ $# -ge 1 ] || { echo "usage: play-stray.sh <run folder made by setup-stray.sh>" >&2; exit 2; }
cd "$1" || exit 1
[ -x ./eboot ] || { echo "no eboot in $1: run tools/stray/setup-stray.sh first" >&2; exit 1; }
rm -rf download0/hk_project/saved/crashreportclient
rm -f download0/notallowedunattendedbugreports
export KYTY_ASYNC_PIPELINES="${KYTY_ASYNC_PIPELINES-2}"
export __GL_SHADER_DISK_CACHE_SIZE="${__GL_SHADER_DISK_CACHE_SIZE-10737418240}"
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP="${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP-1}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER-x11}"
exec ./eboot
