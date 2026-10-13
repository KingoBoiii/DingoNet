#!/bin/sh
set -e
config=${1:-Debug}
case "$config" in
	debug) config=Debug ;;
	release) config=Release ;;
	distribution) config=Distribution ;;
esac
root="$(cd "$(dirname "$0")/../.." && pwd)"
"$root/scripts/linux/Build.sh" "$config"
exec "$root/build/bin/$config-linux-x86_64/DingoNet-Tests/DingoNet-Tests"
