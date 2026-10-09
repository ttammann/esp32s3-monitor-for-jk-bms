#!/bin/sh
# Host-side tests. No ESP-IDF, no hardware needed.
#
# Built with the address and undefined-behaviour sanitizers: the decoders read
# at fixed offsets into caller-supplied buffers and index arrays with values
# taken straight off the wire, so an out-of-bounds read is exactly the failure
# mode worth catching here. Set NOSAN=1 to skip them.
set -e
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/jk_host_test"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
[ -n "$NOSAN" ] && SAN=""

WARN="-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes"

JKBMS=../components/jkbms

build_and_run() {
    name="$1"; shift
    # shellcheck disable=SC2086
    cc -std=c11 -g -O1 $WARN $SAN \
       -I . -I "$JKBMS/include" -I ../components/util/include \
       "$@" -o "$OUT-$name"
    "$OUT-$name"
}

rc=0
build_and_run req   "$JKBMS/jk_crc.c" "$JKBMS/jk_req.c" \
                    host_test_req.c || rc=1
build_and_run jk55  "$JKBMS/jk55.c" "$JKBMS/jk_crc.c" \
                    host_test_jk55.c || rc=1
build_and_run util  "$JKBMS/jk_demux.c" "$JKBMS/jk55.c" "$JKBMS/jk_crc.c" \
                    "$JKBMS/jk_req.c" "$JKBMS/jk_busdiag.c" "$JKBMS/jk_fmt.c" \
                    host_test_util.c || rc=1

if [ "$rc" -eq 0 ]; then
    echo "ALL PASS"
else
    echo "FAILURES" >&2
fi
exit "$rc"
