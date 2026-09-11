/*
 * Test client for tests/run.sh, run against rpc.nfsd started as
 *   rpc.nfsd -F -P <port> -f <exports> -R <export root>
 * The export root must contain:
 *   esc     -> ../outside          (symlink to a directory outside)
 *   lnkfile -> ../outside/secret   (symlink to a file outside)
 *   big/    with <nfiles> files
 * The caller checks outside/ afterwards (no "planted" file, secret's
 * mode unchanged).
 *
 * usage: nfstest <port> <path of big/ on this host> <nfiles>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rpc/rpc.h>
#include "nfs_prot.h"

#define MAXN	4096
#define NLEN	64

static CLIENT	*cl;
static nfs_fh	pubfh;		/* WebNFS public handle: all zeros */
static int	fails;

static void
check(const char *name, int ok, const char *fmt, int val)
{
	printf("%s  %s", ok ? "PASS" : "FAIL", name);
	if (fmt)
		printf(" (%s%d)", fmt, val);
	putchar('\n');
	if (!ok)
		fails++;
}

static void *
must(void *r, const char *what)
{
	if (r == NULL) {
		clnt_perror(cl, what);
		exit(2);
	}
	return r;
}

static void
noattr(sattr *s)
{
	memset(s, 0xff, sizeof(*s));	/* every field -1 = "don't set" */
}

static nfsstat
lookup(nfs_fh *dir, char *name, nfs_fh *out)
{
	diropargs	a;
	diropres	*r;

	a.dir = *dir;
	a.name = name;
	r = must(nfsproc_lookup_2(&a, cl), "lookup");
	if (r->status == NFS_OK && out)
		*out = r->diropres_u.diropres.file;
	return r->status;
}

static nfsstat
create(nfs_fh *dir, char *name)
{
	createargs	a;
	diropres	*r;

	a.where.dir = *dir;
	a.where.name = name;
	noattr(&a.attributes);
	a.attributes.mode = 0644;
	r = must(nfsproc_create_2(&a, cl), "create");
	return r->status;
}

static nfsstat
setmode(nfs_fh *fh, unsigned int mode)
{
	sattrargs	a;
	attrstat	*r;

	a.file = *fh;
	noattr(&a.attributes);
	a.attributes.mode = mode;
	r = must(nfsproc_setattr_2(&a, cl), "setattr");
	return r->status;
}

static nfsstat
mksymlink(nfs_fh *dir, char *name, char *target, unsigned int mode)
{
	symlinkargs	a;
	nfsstat		*r;

	a.from.dir = *dir;
	a.from.name = name;
	a.to = target;
	noattr(&a.attributes);
	a.attributes.mode = mode;
	r = must(nfsproc_symlink_2(&a, cl), "symlink");
	return *r;
}

/* Read one READDIR page; append names (minus . and ..) to names[]. */
static nfsstat
readdir_page(nfs_fh *dir, char cookie[4], char names[][NLEN], int *n,
	     int *eof)
{
	readdirargs	a;
	readdirres	*r;
	entry		*e;

	a.dir = *dir;
	memcpy(a.cookie, cookie, 4);
	a.count = 1024;		/* small pages: ~25 entries each */
	r = must(nfsproc_readdir_2(&a, cl), "readdir");
	if (r->status != NFS_OK)
		return r->status;
	for (e = r->readdirres_u.reply.entries; e; e = e->nextentry) {
		memcpy(cookie, e->cookie, 4);
		if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
			continue;
		if (*n < MAXN) {
			strncpy(names[*n], e->name, NLEN - 1);
			names[*n][NLEN - 1] = '\0';
			(*n)++;
		}
	}
	*eof = r->readdirres_u.reply.eof;
	return NFS_OK;
}

/* Read pages starting at cookie until EOF; -1 if it never ends. */
static int
readdir_rest(nfs_fh *dir, char cookie[4], char names[][NLEN], int *n)
{
	int	eof = 0, pages = 0;

	while (!eof) {
		if (readdir_page(dir, cookie, names, n, &eof) != NFS_OK)
			return -1;
		if (++pages > 1000)
			return -1;
	}
	return pages;
}

static int
count_of(char names[][NLEN], int n, const char *name)
{
	int	i, c = 0;

	for (i = 0; i < n; i++)
		if (!strcmp(names[i], name))
			c++;
	return c;
}

static char	all[MAXN][NLEN], seen[MAXN][NLEN];

int
main(int argc, char **argv)
{
	struct sockaddr_in sin;
	struct timeval	wait = { 5, 0 };
	int		sock = RPC_ANYSOCK, port, nfiles, n, i, missing,
			dups, pages, eof;
	nfs_fh		fh, bigfh;
	char		cookie[4], path[1024];
	nfsstat		st;
	const char	*bigpath;

	if (argc != 4) {
		fprintf(stderr, "usage: %s port bigdir nfiles\n", argv[0]);
		return 2;
	}
	port = atoi(argv[1]);
	bigpath = argv[2];
	nfiles = atoi(argv[3]);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	/* TCP first: needs the listen() fix to connect at all. */
	cl = clnttcp_create(&sin, NFS_PROGRAM, NFS_VERSION, &sock, 0, 0);
	check("TCP transport accepts a connection", cl != NULL, NULL, 0);
	if (cl) {
		check("NULL call over TCP",
		      nfsproc_null_2(NULL, cl) != NULL, NULL, 0);
		clnt_destroy(cl);
	}

	sock = RPC_ANYSOCK;
	cl = clntudp_create(&sin, NFS_PROGRAM, NFS_VERSION, wait, &sock);
	if (cl == NULL) {
		clnt_pcreateerror("clntudp_create");
		return 2;
	}
	cl->cl_auth = authunix_create("nfstest", 0, 0, 0, NULL);

	/* 1: symlink handle used as a directory */
	st = lookup(&pubfh, "esc", &fh);
	check("LOOKUP esc (the link itself)", st == NFS_OK, "status ", st);
	st = lookup(&fh, "secret", NULL);
	check("LOOKUP through symlink dir is refused",
	      st == NFSERR_NOTDIR, "status ", st);
	st = create(&fh, "planted");
	check("CREATE through symlink dir is refused",
	      st == NFSERR_NOTDIR, "status ", st);

	/* 1b: SETATTR / SYMLINK mode must not reach the target */
	st = lookup(&pubfh, "lnkfile", &fh);
	check("LOOKUP lnkfile", st == NFS_OK, "status ", st);
	st = setmode(&fh, 0666);
	printf("      SETATTR mode on lnkfile returned %d\n", st);
	st = lookup(&pubfh, "sub", &fh);
	check("LOOKUP sub", st == NFS_OK, "status ", st);
	st = mksymlink(&fh, "newlink", "../../outside/secret2", 0666);
	printf("      SYMLINK sub/newlink -> outside/secret2, mode 0666, "
	       "returned %d\n", st);

	/* 6a: full listing of an unchanged directory */
	st = lookup(&pubfh, "big", &bigfh);
	check("LOOKUP big", st == NFS_OK, "status ", st);
	memset(cookie, 0, 4);
	n = 0;
	pages = readdir_rest(&bigfh, cookie, all, &n);
	check("full listing terminates", pages > 0, "pages ", pages);
	for (dups = 0, i = 0; i < n; i++)
		if (count_of(all, n, all[i]) != 1)
			dups++;
	check("full listing returns every file", n == nfiles, "got ", n);
	check("full listing has no duplicates", dups == 0, "dups ", dups);

	/* 6b: delete entries after the first page, then continue */
	memset(cookie, 0, 4);
	n = 0;
	readdir_page(&bigfh, cookie, seen, &n, &eof);
	if (n < 11 || eof) {
		check("first page is a partial listing", 0, "entries ", n);
		return 1;
	}
	for (i = 0; i < 10; i++) {
		snprintf(path, sizeof(path), "%s/%s", bigpath, seen[i]);
		if (unlink(path) < 0)
			perror(path);
	}
	readdir_rest(&bigfh, cookie, seen, &n);
	for (missing = 0, i = 0; i < nfiles; i++)
		if (count_of(seen, n, all[i]) == 0)
			missing++;
	for (dups = 0, i = 0; i < n; i++)
		if (count_of(seen, n, seen[i]) != 1)
			dups++;
	check("no entries skipped after deletes", missing == 0,
	      "missing ", missing);
	check("no duplicates after deletes", dups == 0, "dups ", dups);

	printf("%d failure(s)\n", fails);
	return fails != 0;
}
