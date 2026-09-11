#!/bin/bash
#
# Runtime tests for rpc.nfsd: symlink escapes from the export, SETATTR
# through symlinks, READDIR cookies (a full listing, and continuing a
# listing after deletes) and the TCP transport.
#
# usage: tests/run.sh [path to rpc.nfsd]	(default ./rpc.nfsd)
#    or: make check
#
# Needs root (nfsd switches fsuid per request), cc, rpcgen and
# libtirpc-dev. It uses neither rpcbind nor mountd: shim.so (preloaded
# into nfsd) registers the NFS service locally and makes the rpcbind
# set/unset calls no-ops, so a kernel NFS server on the same host is
# left alone, and the client starts from the WebNFS public handle
# (nfsd -R).
#
# Environment: PORT (default 12049; UDP and TCP must be free), TMPDIR
# (where the test tree goes), KEEP=1 to keep the test tree.

set -u
here=$(cd "$(dirname "$0")" && pwd)
src=$(dirname "$here")
NFSD=$(realpath "${1:-./rpc.nfsd}")
PORT=${PORT:-12049}
N=300

if [ "$(id -u)" != 0 ]; then
	echo "tests/run.sh: must run as root" >&2
	exit 2
fi
if ss -lnut | grep -q ":$PORT "; then
	echo "tests/run.sh: port $PORT is in use; set PORT" >&2
	exit 2
fi

W=$(mktemp -d "${TMPDIR:-/tmp}/nfstest.XXXXXX") || exit 2
PID=
cleanup() {
	if [ -n "$PID" ]; then
		kill "$PID" 2>/dev/null
		wait "$PID" 2>/dev/null
	fi
	if [ "${KEEP:-0}" = 1 ]; then
		echo "test tree kept in $W"
	else
		rm -rf "$W"
	fi
}
trap cleanup EXIT

# Client stubs from the server's own nfs_prot.x, plus the shim.
mkdir "$W/build"
cp "$src/nfs_prot.x" "$W/build/"
if ! ( cd "$W/build" &&
	rpcgen -h nfs_prot.x -o nfs_prot.h &&
	rpcgen -c nfs_prot.x -o nfs_prot_xdr.c &&
	rpcgen -l nfs_prot.x -o nfs_prot_clnt.c &&
	cc -I/usr/include/tirpc -I. -o nfstest "$here/nfstest.c" \
		nfs_prot_clnt.c nfs_prot_xdr.c -ltirpc &&
	cc -shared -fPIC -I/usr/include/tirpc -o shim.so "$here/shim.c" \
		-ltirpc ) > "$W/build.log" 2>&1; then
	echo "tests/run.sh: building the test client failed:" >&2
	cat "$W/build.log" >&2
	exit 2
fi

# The export, and files outside it that must stay untouched.
R=$W/root
O=$W/outside
mkdir -p "$R/big" "$R/sub" "$O"
for f in secret secret2; do
	echo SECRET > "$O/$f"
	chmod 600 "$O/$f"
done
ln -s ../outside "$R/esc"
ln -s ../outside/secret "$R/lnkfile"
for i in $(seq -w 1 $N); do : > "$R/big/f$i"; done
echo "$R 127.0.0.1(rw,insecure,no_root_squash)" > "$W/exports"

LD_PRELOAD=$W/build/shim.so "$NFSD" -F -P "$PORT" -f "$W/exports" -R "$R" \
	> "$W/nfsd.log" 2>&1 &
PID=$!
for i in $(seq 50); do
	ss -lnu | grep -q ":$PORT " && break
	kill -0 "$PID" 2>/dev/null || break
	sleep 0.1
done

echo "== $NFSD, test tree on $(stat -f -c %T "$W")"
"$W/build/nfstest" "$PORT" "$R/big" "$N"
rc=$?

if [ -e "$O/planted" ]; then
	echo "FAIL  CREATE planted a file outside the export"
	rc=1
else
	echo "PASS  nothing planted outside the export"
fi
for f in secret secret2; do
	m=$(stat -c %a "$O/$f")
	if [ "$m" = 600 ]; then
		echo "PASS  outside/$f kept mode 600"
	else
		echo "FAIL  outside/$f changed to mode $m"
		rc=1
	fi
done
if [ $rc != 0 ]; then
	echo "--- last lines of nfsd output:"
	tail -20 "$W/nfsd.log"
fi
exit $rc
