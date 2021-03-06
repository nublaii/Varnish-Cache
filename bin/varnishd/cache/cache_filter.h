/*-
 * Copyright (c) 2013-2014 Varnish Software AS
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
 */

struct busyobj;
struct req;
struct vfp_entry;

/* Fetch processors --------------------------------------------------*/

enum vfp_status {
	VFP_ERROR = -1,
	VFP_OK = 0,
	VFP_END = 1,
};

typedef enum vfp_status vfp_init_f(struct busyobj *, struct vfp_entry *);
typedef enum vfp_status
    vfp_pull_f(struct busyobj *, struct vfp_entry *, void *ptr, ssize_t *len);
typedef void vfp_fini_f(struct busyobj *, struct vfp_entry *);

struct vfp {
	const char	*name;
	vfp_init_f	*init;
	vfp_pull_f	*pull;
	vfp_fini_f	*fini;
	const void	*priv1;
	intptr_t	priv2;
};

extern const struct vfp vfp_gunzip;
extern const struct vfp vfp_gzip;
extern const struct vfp vfp_testgunzip;
extern const struct vfp vfp_esi;
extern const struct vfp vfp_esi_gzip;


/* Deliver processors ------------------------------------------------*/

enum vdp_action {
	VDP_NULL,
	VDP_FLUSH,
	VDP_FINISH,
};
typedef int vdp_bytes(struct req *, enum vdp_action, const void *ptr,
    ssize_t len);
