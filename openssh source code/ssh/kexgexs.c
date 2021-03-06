/* $OpenBSD: kexgexs.c,v 1.14 2010/11/10 01:33:07 djm Exp $ */
/*
 * Copyright (c) 2000 Niels Provos.  All rights reserved.
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <openssl/dh.h>

#include "key.h"
#include "cipher.h"
#include "kex.h"
#include "log.h"
#include "packet.h"
#include "dh.h"
#include "ssh2.h"
#include "compat.h"
#ifdef GSSAPI
#include "ssh-gss.h"
#endif
#include "monitor_wrap.h"
#include "dispatch.h"
#include "err.h"
#include "sshbuf.h"

static int input_kex_dh_gex_request(int, u_int32_t, struct ssh *);
static int input_kex_dh_gex_init(int, u_int32_t, struct ssh *);

int
kexgex_server(struct ssh *ssh)
{
	ssh_dispatch_set(ssh, SSH2_MSG_KEX_DH_GEX_REQUEST_OLD,
	    &input_kex_dh_gex_request);
	ssh_dispatch_set(ssh, SSH2_MSG_KEX_DH_GEX_REQUEST,
	    &input_kex_dh_gex_request);
	debug("expecting SSH2_MSG_KEX_DH_GEX_REQUEST");
	return 0;
}

static int
input_kex_dh_gex_request(int type, u_int32_t seq, struct ssh *ssh)
{
	struct kex *kex = ssh->kex;
	int r, min = -1, max = -1, nbits = -1;

	switch (type) {
	case SSH2_MSG_KEX_DH_GEX_REQUEST:
		debug("SSH2_MSG_KEX_DH_GEX_REQUEST received");
		if ((r = sshpkt_get_u32(ssh, &min)) != 0 ||
		    (r = sshpkt_get_u32(ssh, &nbits)) != 0 ||
		    (r = sshpkt_get_u32(ssh, &max)) != 0 ||
		    (r = sshpkt_get_end(ssh)) != 0)
			goto out;
		kex->nbits = nbits;
		kex->min = min;
		kex->max = max;
		min = MAX(DH_GRP_MIN, min);
		max = MIN(DH_GRP_MAX, max);
		nbits = MAX(DH_GRP_MIN, nbits);
		nbits = MIN(DH_GRP_MAX, nbits);
		break;
	case SSH2_MSG_KEX_DH_GEX_REQUEST_OLD:
		debug("SSH2_MSG_KEX_DH_GEX_REQUEST_OLD received");
		if ((r = sshpkt_get_u32(ssh, &nbits)) != 0 ||
		    (r = sshpkt_get_end(ssh)) != 0)
			goto out;
		kex->nbits = nbits;
		/* unused for old GEX */
		kex->min = min = DH_GRP_MIN;
		kex->max = max = DH_GRP_MAX;
		break;
	default:
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}

	if (kex->max < kex->min || kex->nbits < kex->min ||
	    kex->max < kex->nbits) {
		r = SSH_ERR_DH_GEX_OUT_OF_RANGE;
		goto out;
	}

	/* Contact privileged parent */
	kex->dh = PRIVSEP(choose_dh(min, nbits, max));
	if (kex->dh == NULL) {
		sshpkt_disconnect(ssh, "no matching DH grp found");
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	debug("SSH2_MSG_KEX_DH_GEX_GROUP sent");
	if ((r = sshpkt_start(ssh, SSH2_MSG_KEX_DH_GEX_GROUP)) != 0 ||
	    (r = sshpkt_put_bignum2(ssh, kex->dh->p)) != 0 ||
	    (r = sshpkt_put_bignum2(ssh, kex->dh->g)) != 0 ||
	    (r = sshpkt_send(ssh)) != 0)
		goto out;

	/* Compute our exchange value in parallel with the client */
	if ((r = dh_gen_key(kex->dh, kex->we_need * 8)) != 0)
		goto out;

	/* old KEX does not use min/max in kexgex_hash() */
	if (type == SSH2_MSG_KEX_DH_GEX_REQUEST_OLD)
		kex->min = kex->max = -1;

	debug("expecting SSH2_MSG_KEX_DH_GEX_INIT");
	ssh_dispatch_set(ssh, SSH2_MSG_KEX_DH_GEX_INIT, &input_kex_dh_gex_init);
	r = 0;
 out:
	return r;
}

static int
input_kex_dh_gex_init(int type, u_int32_t seq, struct ssh *ssh)
{
	struct kex *kex = ssh->kex;
	BIGNUM *shared_secret = NULL, *dh_client_pub = NULL;
	struct sshkey *server_host_public, *server_host_private;
	u_char *kbuf = NULL, *signature = NULL, *server_host_key_blob = NULL;
	u_char *hash;
	size_t sbloblen, slen;
	size_t klen = 0, hashlen;
	int kout, r;

	if (kex->load_host_public_key == NULL ||
	    kex->load_host_private_key == NULL) {
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	if ((server_host_public = kex->load_host_public_key(kex->hostkey_type,
	    ssh)) == NULL ||
	    (server_host_private = kex->load_host_private_key(kex->hostkey_type,
	    ssh)) == NULL) {
		r = SSH_ERR_NO_HOSTKEY_LOADED;
		goto out;
	}

	/* key, cert */
	if ((dh_client_pub = BN_new()) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if ((r = sshpkt_get_bignum2(ssh, dh_client_pub)) != 0 ||
	    (r = sshpkt_get_end(ssh)) != 0)
		goto out;

#ifdef DEBUG_KEXDH
	fprintf(stderr, "dh_client_pub= ");
	BN_print_fp(stderr, dh_client_pub);
	fprintf(stderr, "\n");
	debug("bits %d", BN_num_bits(dh_client_pub));
#endif

#ifdef DEBUG_KEXDH
	DHparams_print_fp(stderr, kex->dh);
	fprintf(stderr, "pub= ");
	BN_print_fp(stderr, kex->dh->pub_key);
	fprintf(stderr, "\n");
#endif
	if (!dh_pub_is_valid(kex->dh, dh_client_pub)) {
		sshpkt_disconnect(ssh, "bad client public DH value");
		r = SSH_ERR_MESSAGE_INCOMPLETE;
		goto out;
	}

	klen = DH_size(kex->dh);
	if ((kbuf = malloc(klen)) == NULL ||
	    (shared_secret = BN_new()) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if ((kout = DH_compute_key(kbuf, dh_client_pub, kex->dh)) < 0 ||
	    BN_bin2bn(kbuf, kout, shared_secret) == NULL) {
		r = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}
#ifdef DEBUG_KEXDH
	dump_digest("shared secret", kbuf, kout);
#endif
	if ((r = sshkey_to_blob(server_host_public, &server_host_key_blob,
	    &sbloblen)) != 0)
		goto out;
	/* calc H */
	if ((r = kexgex_hash(
	    kex->evp_md,
	    kex->client_version_string,
	    kex->server_version_string,
	    sshbuf_ptr(kex->peer), sshbuf_len(kex->peer),
	    sshbuf_ptr(kex->my), sshbuf_len(kex->my),
	    server_host_key_blob, sbloblen,
	    kex->min, kex->nbits, kex->max,
	    kex->dh->p, kex->dh->g,
	    dh_client_pub,
	    kex->dh->pub_key,
	    shared_secret,
	    &hash, &hashlen)) != 0)
		goto out;

	/* save session id := H */
	if (kex->session_id == NULL) {
		kex->session_id_len = hashlen;
		kex->session_id = malloc(kex->session_id_len);
		if (kex->session_id == NULL) {
			r = SSH_ERR_ALLOC_FAIL;
			goto out;
		}
		memcpy(kex->session_id, hash, kex->session_id_len);
	}

	/* sign H */
	if ((r = PRIVSEP(sshkey_sign(server_host_private, &signature, &slen,
	    hash, hashlen, ssh->compat))) < 0)
		goto out;

	/* destroy_sensitive_data(); */

	/* send server hostkey, DH pubkey 'f' and singed H */
	if ((r = sshpkt_start(ssh, SSH2_MSG_KEX_DH_GEX_REPLY)) != 0 ||
	    (r = sshpkt_put_string(ssh, server_host_key_blob, sbloblen)) != 0 ||
	    (r = sshpkt_put_bignum2(ssh, kex->dh->pub_key)) != 0 ||     /* f */
	    (r = sshpkt_put_string(ssh, signature, slen)) != 0 ||
	    (r = sshpkt_send(ssh)) != 0)
		goto out;

	if ((r = kex_derive_keys(ssh, hash, hashlen, shared_secret)) == 0)
		r = kex_send_newkeys(ssh);
 out:
	DH_free(kex->dh);
	kex->dh = NULL;
	if (dh_client_pub)
		BN_clear_free(dh_client_pub);
	if (kbuf) {
		bzero(kbuf, klen);
		free(kbuf);
	}
	if (shared_secret)
		BN_clear_free(shared_secret);
	if (server_host_key_blob)
		free(server_host_key_blob);
	if (signature)
		free(signature);
	return r;
}
