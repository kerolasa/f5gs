#!/bin/bash

set -e
trap 'echo "autogen.sh: exit on error"; exit 1' ERR

if [ ! -f ./f5gs.c ]; then
	cd ${0%/*}
fi
mkdir -p m4
autoreconf --force --install --symlink
exit $?
