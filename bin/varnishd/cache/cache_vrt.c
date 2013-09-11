/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2013 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Runtime support for compiled VCL programs
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>

#include "cache.h"

#include "cache_backend.h"
#include "hash/hash_slinger.h"
#include "vav.h"
#include "vcl.h"
#include "vrt.h"
#include "vrt_obj.h"
#include "vtim.h"

const void * const vrt_magic_string_end = &vrt_magic_string_end;
const void * const vrt_magic_string_unset = &vrt_magic_string_unset;

/*--------------------------------------------------------------------*/

void
VRT_error(const struct vrt_ctx *ctx, unsigned code, const char *reason)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	VSLb(ctx->vsl, SLT_Debug, "VCL_error(%u, %s)", code,
	    reason ?  reason : "(null)");
	if (code < 100 || code > 999)
		code = 503;
	ctx->req->err_code = (uint16_t)code;
	ctx->req->err_reason =
	    reason ? reason : http_StatusMessage(ctx->req->err_code);
}

/*--------------------------------------------------------------------*/

void
VRT_count(const struct vrt_ctx *ctx, unsigned u)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->vsl != NULL)
		VSLb(ctx->vsl, SLT_VCL_trace, "%u %u.%u", u,
		    ctx->vcl->ref[u].line, ctx->vcl->ref[u].pos);
}

/*--------------------------------------------------------------------*/

void
VRT_acl_log(const struct vrt_ctx *ctx, const char *msg)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	VSLb(ctx->vsl, SLT_VCL_acl, "%s", msg);
}

/*--------------------------------------------------------------------*/

static struct http *
vrt_selecthttp(const struct vrt_ctx *ctx, enum gethdr_e where)
{
	struct http *hp;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	switch (where) {
	case HDR_REQ:
		hp = ctx->http_req;
		break;
	case HDR_BEREQ:
		hp = ctx->http_bereq;
		break;
	case HDR_BERESP:
		hp = ctx->http_beresp;
		break;
	case HDR_RESP:
		hp = ctx->http_resp;
		break;
	case HDR_OBJ:
		hp = ctx->http_obj;
		break;
	default:
		WRONG("vrt_selecthttp 'where' invalid");
	}
	return (hp);
}

/*--------------------------------------------------------------------*/

char *
VRT_GetHdr(const struct vrt_ctx *ctx, const struct gethdr_s *hs)
{
	char *p;
	struct http *hp;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	hp = vrt_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (!http_GetHdr(hp, hs->what, &p))
		return (NULL);
	return (p);
}

/*--------------------------------------------------------------------
 * Collapse a STRING_LIST in the space provided, or return NULL
 */

char *
VRT_StringList(char *d, unsigned dl, const char *p, va_list ap)
{
	char *b, *e;
	unsigned x;

	b = d;
	e = b + dl;
	while (p != vrt_magic_string_end && b < e) {
		if (p != NULL) {
			x = strlen(p);
			if (b + x < e)
				memcpy(b, p, x);
			b += x;
		}
		p = va_arg(ap, const char *);
	}
	if (b >= e)
		return (NULL);
	*b++ = '\0';
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST into a workspace.
 */

const char *
VRT_String(struct ws *ws, const char *h, const char *p, va_list ap)
{
	char *b, *e;
	unsigned u, x;

	u = WS_Reserve(ws, 0);
	e = b = ws->f;
	e += u;
	if (h != NULL) {
		x = strlen(h);
		if (b + x < e)
			memcpy(b, h, x);
		b += x;
		if (b < e)
			*b = ' ';
		b++;
	}
	b = VRT_StringList(b, e > b ? e - b : 0, p, ap);
	if (b == NULL || b == e) {
		WS_Release(ws, 0);
		return (NULL);
	}
	e = b;
	b = ws->f;
	WS_Release(ws, e - b);
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST on the current workspace
 */

const char *
VRT_CollectString(const struct vrt_ctx *ctx, const char *p, ...)
{
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	va_start(ap, p);
	b = VRT_String(ctx->ws, NULL, p, ap);
	va_end(ap);
	return (b);
}

/*--------------------------------------------------------------------*/

void
VRT_SetHdr(const struct vrt_ctx *ctx , const struct gethdr_s *hs,
    const char *p, ...)
{
	struct http *hp;
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(hs);
	AN(hs->what);
	hp = vrt_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	va_start(ap, p);
	if (p == vrt_magic_string_unset) {
		http_Unset(hp, hs->what);
	} else {
		b = VRT_String(hp->ws, hs->what + 1, p, ap);
		if (b == NULL) {
			VSLb(ctx->vsl, SLT_LostHeader, "%s", hs->what + 1);
		} else {
			http_Unset(hp, hs->what);
			http_SetHeader(hp, b);
		}
	}
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
VRT_handling(const struct vrt_ctx *ctx, unsigned hand)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	assert(hand < VCL_RET_MAX);
	*ctx->handling = hand;
}

/*--------------------------------------------------------------------
 * Feed data into the hash calculation
 */

void
VRT_hashdata(const struct vrt_ctx *ctx, const char *str, ...)
{
	va_list ap;
	const char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	HSH_AddString(ctx->req, str);
	va_start(ap, str);
	while (1) {
		p = va_arg(ap, const char *);
		if (p == vrt_magic_string_end)
			break;
		HSH_AddString(ctx->req, p);
		VSLb(ctx->vsl, SLT_Hash, "%s", str);
	}
	/*
	 * Add a 'field-separator' to make it more difficult to
	 * manipulate the hash.
	 */
	HSH_AddString(ctx->req, NULL);
}

/*--------------------------------------------------------------------*/

double
VRT_r_now(const struct vrt_ctx *ctx)
{

	(void)ctx;
	return (VTIM_real());
}

/*--------------------------------------------------------------------*/

char *
VRT_IP_string(const struct vrt_ctx *ctx, const VCL_IP ip)
{
	char *p;
	const struct sockaddr *sa;
	const struct sockaddr_in *si4;
	const struct sockaddr_in6 *si6;
	const void *addr;
	int len;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	sa = ip;
	switch (sa->sa_family) {
	case AF_INET:
		len = INET_ADDRSTRLEN;
		si4 = ip;
		addr = &(si4->sin_addr);
		break;
	case AF_INET6:
		len = INET6_ADDRSTRLEN;
		si6 = ip;
		addr = &(si6->sin6_addr);
		break;
	default:
		INCOMPL();
	}
	XXXAN(len);
	AN(p = WS_Alloc(ctx->ws, len));
	AN(inet_ntop(sa->sa_family, addr, p, len));
	return (p);
}

char *
VRT_INT_string(const struct vrt_ctx *ctx, long num)
{
	char *p;
	int size;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	size = snprintf(NULL, 0, "%ld", num) + 1;
	AN(p = WS_Alloc(ctx->ws, size));
	assert(snprintf(p, size, "%ld", num) < size);
	return (p);
}

char *
VRT_REAL_string(const struct vrt_ctx *ctx, double num)
{
	char *p;
	int size;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	size = snprintf(NULL, 0, "%.3f", num) + 1;
	AN(p = WS_Alloc(ctx->ws, size));
	assert(snprintf(p, size, "%.3f", num) < size);
	return (p);
}

char *
VRT_TIME_string(const struct vrt_ctx *ctx, double t)
{
	char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	p = WS_Alloc(ctx->ws, VTIM_FORMAT_SIZE);
	if (p != NULL)
		VTIM_format(t, p);
	return (p);
}

const char * __match_proto__()
VRT_BACKEND_string(VCL_BACKEND d)
{
	if (d == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	return (d->vcl_name);
}

const char *
VRT_BOOL_string(unsigned val)
{

	return (val ? "true" : "false");
}

/*--------------------------------------------------------------------*/

void
VRT_Rollback(const struct vrt_ctx *ctx)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	HTTP_Copy(ctx->req->http, ctx->req->http0);
	WS_Reset(ctx->req->ws, ctx->req->ws_req);
}

/*--------------------------------------------------------------------*/

void
VRT_synth_page(const struct vrt_ctx *ctx, unsigned flags, const char *str, ...)
{
	va_list ap;
	const char *p;
	struct vsb *vsb;

	(void)flags;
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req->obj, OBJECT_MAGIC);
	vsb = SMS_Makesynth(ctx->req->obj);
	AN(vsb);

	va_start(ap, str);
	p = str;
	while (p != vrt_magic_string_end) {
		if (p == NULL)
			p = "(null)";
		VSB_cat(vsb, p);
		p = va_arg(ap, const char *);
	}
	va_end(ap);
	SMS_Finish(ctx->req->obj);
	http_Unset(ctx->req->obj->http, H_Content_Length);
}

/*--------------------------------------------------------------------*/

void
VRT_ban_string(const char *str)
{
	char *a1, *a2, *a3;
	char **av;
	struct ban *b;
	int good;
	int i;

	av = VAV_Parse(str, NULL, ARGV_NOESC);
	if (av[0] != NULL) {
		/* XXX: report error how ? */
		VAV_Free(av);
		return;
	}
	b = BAN_New();
	good = 0;
	for (i = 1; ;) {
		a1 = av[i++];
		if (a1 == NULL)
			break;
		good = 0;
		a2 = av[i++];
		if (a2 == NULL)
			break;
		a3 = av[i++];
		if (a3 == NULL)
			break;
		if (BAN_AddTest(NULL, b, a1, a2, a3))
			break;
		good = 1;
		if (av[i] == NULL)
			break;
		good = 0;
		if (strcmp(av[i++], "&&"))
			break;
	}
	if (!good)
		BAN_Free(b);		/* XXX: report error how ? */
	else
		(void)BAN_Insert(b);	/* XXX: report error how ? */
	VAV_Free(av);
}

/*--------------------------------------------------------------------
 *
 */

int
VRT_CacheReqBody(const struct vrt_ctx *ctx, long long maxsize)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	if (ctx->method != VCL_MET_RECV) {
		VSLb(ctx->vsl, SLT_VCL_Error,
		    "req.body can only be cached in vcl_recv{}");
		return (0);
	}
	return (HTTP1_CacheReqBody(ctx->req, maxsize));
}

/*--------------------------------------------------------------------
 * "real" purges
 */

void
VRT_purge(const struct vrt_ctx *ctx, double ttl, double grace)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req->wrk, WORKER_MAGIC);
	if (ctx->method == VCL_MET_HIT)
		HSH_Purge(ctx->req->wrk, ctx->req->obj->objcore->objhead,
		    ttl, grace);
	else if (ctx->method == VCL_MET_MISS)
		HSH_Purge(ctx->req->wrk, ctx->req->objcore->objhead,
		    ttl, grace);
}

/*--------------------------------------------------------------------
 * Simple stuff
 */

int
VRT_strcmp(const char *s1, const char *s2)
{
	if (s1 == NULL || s2 == NULL)
		return(1);
	return (strcmp(s1, s2));
}

void
VRT_memmove(void *dst, const void *src, unsigned len)
{

	(void)memmove(dst, src, len);
}
