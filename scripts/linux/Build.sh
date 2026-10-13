#!/bin/sh
set -e
config=$(echo "${1:-Debug}" | tr '[:upper:]' '[:lower:]')
cd "$(dirname "$0")/../.."
[ -f Makefile ] || vendor/premake/bin/premake5 gmake
make -j"$(nproc)" config="$config"
