/*
 * LD_PRELOAD shim for testing rpc.nfsd without touching the host's
 * rpcbind (the kernel NFS server's registrations live there).
 * svc_register() registers the dispatcher locally only; the rpcbind
 * set/unset calls become no-ops.
 */
#include <rpc/rpc.h>

bool_t
svc_register(SVCXPRT *xprt, u_long prog, u_long vers,
	     void (*dispatch)(struct svc_req *, SVCXPRT *), int protocol)
{
	(void) protocol;
	return svc_reg(xprt, prog, vers, dispatch, NULL);
}

bool_t rpcb_unset(rpcprog_t p, rpcvers_t v, const struct netconfig *n)
{ (void) p; (void) v; (void) n; return TRUE; }

bool_t pmap_unset(u_long p, u_long v)
{ (void) p; (void) v; return TRUE; }

bool_t pmap_set(u_long p, u_long v, int proto, int port)
{ (void) p; (void) v; (void) proto; (void) port; return TRUE; }
