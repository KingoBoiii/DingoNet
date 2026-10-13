#!/bin/sh
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

[ $# -gt 0 ] || set -- Debug Release Distribution
base=$(head -n 1 VERSION | tr -d '[:space:]')
DN_VERSION=${DN_VERSION:-$base.0}
build=${DN_BUILD_NUMBER:-$(git rev-list --count HEAD)}
ver="$DN_VERSION.$build"
IFS=. read -r v_major v_minor v_patch <<EOF
$DN_VERSION
EOF
[ -n "$v_patch" ] || { echo "DN_VERSION must be MAJOR.MINOR.PATCH." >&2; exit 1; }

mkdir -p build/package
cp include/DingoNet/BuildInfo.h build/package/BuildInfo.h.bak
trap 'cp "$root/build/package/BuildInfo.h.bak" "$root/include/DingoNet/BuildInfo.h"; rm -f "$root/build/package/BuildInfo.h.bak"' EXIT
printf '%s\n' "#pragma once" "" \
	"#define DN_VERSION_MAJOR $v_major" \
	"#define DN_VERSION_MINOR $v_minor" \
	"#define DN_VERSION_PATCH $v_patch" \
	"#define DN_VERSION_BUILD $build" > include/DingoNet/BuildInfo.h

for arg in "$@"; do
	case "$(echo "$arg" | tr '[:upper:]' '[:lower:]')" in
		debug) config=Debug ;;
		release) config=Release ;;
		distribution) config=Distribution ;;
		*) echo "Unknown configuration: $arg" >&2; exit 1 ;;
	esac
	"$root/scripts/linux/Build.sh" "$config"

	stage="build/package/stage-$config-linux"
	zip_path="$root/build/package/DingoNet-$ver-$config-linux-x86_64.zip"
	rm -rf "$stage" "$zip_path"
	mkdir -p "$stage/lib/$config"
	cp -r include "$stage/include"
	cp -r steam "$stage/steam"
	cp "build/bin/$config-linux-x86_64/DingoNet/libDingoNet.a" "$stage/lib/$config/"
	cp LICENSE.txt "$stage/"
	echo "$ver" > "$stage/VERSION.txt"
	if command -v zip >/dev/null 2>&1; then
		(cd "$stage" && zip -q -r "$zip_path" .)
	else
		(cd "$stage" && python3 -m zipfile -c "$zip_path" *)
	fi
	rm -rf "$stage"
	echo "Packaged $zip_path"
done
