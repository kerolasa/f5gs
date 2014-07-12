# This file has variables and functions the actual tests need.

testname="${0##*/}"

declare -A testret
testret[enable]=0
testret[maintenance]=1
testret[disable]=2
testret[unknown]=3

retval=0
tcp_port=19862

if [ "x${abs_top_builddir}" = 'x' ]; then
	stete_tmp_dir=$(readlink -f "$0")
	state_dir="${stete_tmp_dir%/*}"
else
	state_dir="${abs_top_builddir}/tests/"
fi
