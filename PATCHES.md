# Patches for building and running nfs-user-server (unfsd) on modern Linux

> **Note (merged tree):** this file comes from ihal's fork. In this
> repository fix 3 is a single `svc_getcaller_in()` helper in `system.h`
> rather than per-call-site casts, and fix 6 is unread-attitude's
> entry-index readdir cookie (same idea, without `rewinddir()`).
> Build flags are in commit e71fe90; see `git log` for the rest.

This fork contains fixes needed to build and run the classic NFSv2
user-space server (`unfsd`, originally by Mark Shand et al., 1998) on a
modern Linux distribution (tested on Ubuntu 24.04, kernel 6.8,
libtirpc-based glibc RPC stack).

The upstream kernel dropped in-kernel NFSv2 support around Linux 5.18+,
and Ubuntu 24.04's kernel no longer includes `NFSD_V2` at all. This
project lets you keep serving NFSv2 (e.g. for retro/vintage clients)
entirely in user space, with no kernel patching required.

## Summary of fixes

### 1. Build: libtirpc integration

Modern glibc no longer ships Sun RPC headers (`rpc/rpc.h`, etc.) in the
default include path; they now live under `libtirpc`. Added:

- `-I/usr/include/tirpc` to `CFLAGS`
- `-ltirpc` to `LIBS`

### 2. Build: `xdrproc_t` redefinition conflict

`system.h` re-declared `xdrproc_t` in a way that conflicts with the
declaration in `libtirpc`'s `rpc/xdr.h`. Removed the local
redeclaration.

### 3. Build: `svc_getcaller()` return type change

Under glibc's legacy Sun RPC, `svc_getcaller()` effectively returned a
pointer usable as `struct sockaddr_in *`. Under TI-RPC (libtirpc), the
macro instead exposes the raw transport address buffer, which needs to
be explicitly cast. Every call site that dereferenced
`svc_getcaller(...)->sin_addr` or `->sin_port` (in `nfsd.c`, `rmtab.c`,
`rquotad.c`, `ugidd.c`, `ugid_map.c`, `auth_clnt.c`) now casts the
result to `(struct sockaddr_in *)` first.

### 4. Build: missing `<time.h>` includes

Several files (`logging.c`, `rpcmisc.c`, `fh.c`, `haccess.c`,
`failsafe.c`, `nfs_dispatch.c`) called `time()`/`localtime()` without
including `<time.h>`, which newer glibc/gcc treat as a hard error
(implicit declaration returning `int`, corrupting the `struct tm *`
result). Added the missing include to each.

### 5. Runtime: TCP socket never entered LISTEN state (100% CPU busy-loop)

The most serious runtime bug. Under the legacy Sun RPC library,
`svctcp_create()` implicitly called `listen()` on the socket it was
given. Under libtirpc, it does not. This package never called
`listen()` itself, so the TCP listening socket was left unlistened.
The result: `accept()` was called in a tight loop, permanently failing
with `EINVAL`, pegging `rpc.nfsd` at ~100% CPU and never accepting TCP
connections.

Fixed by adding an explicit `listen()` call in `makesock()` in
`rpcmisc.c`, gated on `proto == IPPROTO_TCP`.

### 6. Runtime: `readdir` cookie truncation causes infinite loop on client

`nfsd_nfsproc_readdir_2()` (in `nfsd.c`) used `telldir()`/`seekdir()`
to implement NFS `readdir` cookies, truncating the (potentially 64-bit)
`off_t` returned by `telldir()` into a 32-bit NFS cookie. On ext4 (and
other htree-based directories), `telldir()` can return large, hashed
offsets that don't survive this truncation, corrupting the cookie to
`0xFFFFFFFF`. Clients would then resend the same broken cookie forever,
causing `ls` (and any other directory listing) to loop indefinitely
sending duplicate `READDIR` requests.

Fixed by replacing the `telldir()`/`seekdir()`-based cookie scheme with
a simple sequential entry-index cookie: the cookie is now "how many
directory entries to skip from the start", implemented via
`rewinddir()` + a skip-loop. This is O(n) per readdir call rather than
O(1), which is a fine trade-off for the small directories this server
is realistically used with, and it completely sidesteps the
64-bit/32-bit truncation problem, since the index space is naturally
tiny.

Note: this cookie scheme assumes the directory's contents doesn't
change between the client's directory-listing calls (entries added or
removed mid-listing can shift indices). This matches the original
`telldir()`-based scheme's assumptions in practice and is fine for
typical read-mostly export use cases.

### 7. Runtime: `/etc/exports` parser silently forces read-only on unknown keywords

The exports-file option parser in `auth_init.c` has a fallback for
unrecognized keywords that sets `all_squash` and (critically)
`read_only` on the export, as a fail-safe. Modern `/etc/exports` files
commonly include options this 1998-era parser doesn't know about
(e.g. `no_subtree_check`, a knfsd-specific option), silently forcing
the export read-only with no obvious error to the user (only a
`Dprintf(L_ERROR, ...)` to syslog).

No code change was made here (the fail-safe behavior is arguably
reasonable), but this is documented as a **gotcha**: keep
`/etc/exports` entries limited to keywords this server understands
(`rw`, `ro`, `secure`, `insecure`, `root_squash`, `no_root_squash`,
`link_relative`, `link_absolute`, `map_daemon`, `map_nis=`,
`map_static=`, `map_identity`, `all_squash`, `no_all_squash`,
`noaccess`, `squash_uids=`, `squash_gids=`, `anonuid=`, `anongid=`,
`async`, `sync`). In particular, avoid `no_subtree_check`,
`fsid=`, `crossmnt`, and other knfsd/NFSv4-only options.

## Tested configuration

- Server: Ubuntu Server 24.04 (kernel 6.8.x), gcc 13, libtirpc-dev
- Client: Solaris/SunOS (`mount -t nfs 192.168.0.1:/export/path /mnt/point`)
- Export: local ext4 filesystem, single flat directory

## Not fixed / out of scope

- `rpc.ugidd` host-access-control via `libwrap`/tcp_wrappers was left
  disabled (`n` at the BUILD prompt) since modern distros no longer
  ship `libwrap.a` as a static archive by default. Host-based access
  control should instead be done via `/etc/exports` client
  restrictions and/or firewall rules.
