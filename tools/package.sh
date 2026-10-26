#!/bin/sh
# Assemble a release folder for one platform and archive it.
#
#   tools/package.sh PLATFORM LAUNCHER_PATH LAUNCHER_NAME ARCHIVE(tar.gz|zip)
#
# Example: tools/package.sh linux-x86_64 build/nullroute nullroute tar.gz
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ $# -ne 4 ]; then
	echo "usage: $0 PLATFORM LAUNCHER_PATH LAUNCHER_NAME ARCHIVE" >&2
	exit 2
fi

PLATFORM=$1
LAUNCHER=$2
LAUNCHER_NAME=$3
ARCHIVE=$4

VERSION=$(sed -n 's/^NR_VERSION="\(.*\)"/\1/p' game/lib/core.sh)
NAME="nullroute-$VERSION-$PLATFORM"
STAGE="dist/$NAME"

rm -rf "dist/$NAME" "dist/$NAME.$ARCHIVE"
mkdir -p "$STAGE"

cp "$LAUNCHER" "$STAGE/$LAUNCHER_NAME"
chmod 755 "$STAGE/$LAUNCHER_NAME"

cp -R game "$STAGE/game"
cp README.md LICENSE "$STAGE/"

cd dist
case $ARCHIVE in
	tar.gz) tar -czf "$NAME.tar.gz" "$NAME" ;;
	zip)    zip -qr "$NAME.zip" "$NAME" ;;
	*)      echo "unknown archive format: $ARCHIVE" >&2; exit 2 ;;
esac
echo "built dist/$NAME.$ARCHIVE"
