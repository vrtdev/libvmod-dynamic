/*-
 * Copyright 2019 UPLEX - Nils Goroll Systemoptimierung
 * All rights reserved.
 *
 * Authors: Nils Goroll <nils.goroll@uplex.de>
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
 */

#include "config.h"

#include <string.h>
//#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <cache/cache.h>
#include <vsa.h>

#include "vcc_dynamic_if.h"

#include "dyn_resolver.h"
#include "dyn_getdns.h"

/* ------------------------------------------------------------
 * getdns resolver
 */

struct dyn_getdns_addr_state {
	struct VPFX(dynamic_resolver_context)	*context;
	getdns_dict				*response;
	getdns_list				*replies;
	getdns_list				*answers;
	size_t					n_replies;
	size_t					n_answers;
	size_t					reply;  // next to return
	size_t					answer; // next to return
	uint16_t				port;
};

#define errchk(ret) if (ret != GETDNS_RETURN_GOOD) goto out

#ifdef DUMP_GETDNS
#include <unistd.h>
#define dbg_dump_getdns(r) do {					\
		char *dbg = getdns_pretty_print_dict(r);		\
		write(2, dbg, strlen(dbg));				\
		free(dbg);						\
	} while (0)
#else
#define dbg_dump_getdns(r) (void)0
#endif

static int
getdns_lookup(struct VPFX(dynamic_resolver) *r,
    const char *node, const char *service, void **priv)
{
	struct VPFX(dynamic_resolver_context) *c = NULL;
	struct dyn_getdns_addr_state *state;
	getdns_return_t ret = GETDNS_RETURN_GENERIC_ERROR;

	char		buf[1024];
	struct servent	servent_buf[1];
	struct servent	*servent;

	getdns_dict	*reply;
	uint32_t	status;

	AN(r);
	AN(priv);
	AZ(*priv);

	state = malloc(sizeof *state);
	AN(state);
	memset(state, 0, sizeof *state);

	// XXX tcp hardcoded ok?
	state->port = atoi(service);
	if (state->port != 0) {
		state->port = htons(state->port);
	} else if (getservbyname_r(service, "tcp", servent_buf,
	    buf, sizeof(buf), &servent) != 0) {
		ret = GETDNS_RETURN_NO_SERVBYNAME;
		goto out;
	} else {
		state->port = servent->s_port;
	}

	c = dyn_getdns_get_context(r);
	AN(c);
	AN(c->context);
	state->context = c;

	ret = getdns_address_sync(c->context, node, NULL, &state->response);
	errchk(ret);

	dbg_dump_getdns(state->response);

	ret = getdns_dict_get_int(state->response, "/status", &status);
	errchk(ret);

	if (status != GETDNS_RESPSTATUS_GOOD) {
		ret = status;
		goto out;
	}

	ret = getdns_dict_get_list(state->response,
	    "/replies_tree", &state->replies);
	errchk(ret);

	ret = getdns_list_get_length(state->replies,
	    &state->n_replies);
	errchk(ret);

	if (state->n_replies == 0) {
		ret = GETDNS_RETURN_NO_ANSWERS;
		goto out;
	}

	do {
		ret = getdns_list_get_dict(state->replies,
		    state->reply++, &reply);
		errchk(ret);

		ret = getdns_dict_get_list(reply,
		    "/answer", &state->answers);
		errchk(ret);

		state->answer = 0;

		ret = getdns_list_get_length(state->answers,
		    &state->n_answers);
		errchk(ret);
	} while (state->n_answers == 0 && state->reply < state->n_replies);

	if (state->n_answers == 0)
		ret = GETDNS_RETURN_NO_ANSWERS;

  out:
	*priv = state;
	return (ret);
}

void *getdns_last = &getdns_last;

static struct res_info *
getdns_result(struct res_info *info, void *priv, void **answerp)
{
	struct dyn_getdns_addr_state *state;
	getdns_dict *rr;
	getdns_bindata *addr;
	getdns_return_t ret;
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	getdns_dict	*reply;

	AN(info);
	AN(priv);
	AN(answerp);

	if (*answerp == getdns_last)
		return (NULL);

	state = priv;
	if (state->answer >= state->n_answers &&
	    state->reply >= state->n_replies) {
		*answerp = getdns_last;
		return (NULL);
	} else if (*answerp == NULL) {
		*answerp = &state->answer;
	}

	assert(*answerp == &state->answer);

	do {
		// advace to next reply when out of answers
		if (state->answer >= state->n_answers) {
			ret = getdns_list_get_dict(state->replies,
			    state->reply++, &reply);
			if (ret != 0)
				break;

			ret = getdns_dict_get_list(reply,
			    "/answer", &state->answers);
			if (ret != 0)
				break;

			state->answer = 0;

			ret = getdns_list_get_length(state->answers,
			    &state->n_answers);
			if (ret != 0)
				break;
		}

		ret = getdns_list_get_dict(state->answers,
		    state->answer++, &rr);
		AZ(ret);
		ret = getdns_dict_get_bindata(rr, "/rdata/ipv6_address", &addr);
		if (ret == 0)
			break;
		ret = getdns_dict_get_bindata(rr, "/rdata/ipv4_address", &addr);
		if (ret == 0)
			break;
	} while (state->answer < state->n_answers ||
	    state->reply < state->n_replies);

	if (ret != 0) {
		*answerp = getdns_last;
		return (NULL);
	}

	(void) getdns_dict_get_int(rr, "/ttl", &info->ttl);

	/* why dont the getdns folks provide with an interface to
	 * return a sockaddr ?
	 */

	switch (addr->size) {
	case 4:
		assert(sizeof sa4.sin_addr == 4);
		memset(&sa4, 0, sizeof sa4);
		sa4.sin_family = AF_INET;
		sa4.sin_port = state->port;
		memcpy(&sa4.sin_addr, addr->data, addr->size);
		info->sa = VSA_Build(info->suckbuf, &sa4, sizeof sa4);
		break;
	case 16:
		assert(sizeof sa6.sin6_addr == 16);
		memset(&sa6, 0, sizeof sa6);
		sa6.sin6_family = AF_INET6;
		sa6.sin6_port = state->port;
		memcpy(&sa6.sin6_addr, addr->data, addr->size);
		info->sa = VSA_Build(info->suckbuf, &sa6, sizeof sa6);
		break;
	default:
		INCOMPL();
	}
	return (info->sa != NULL ? info : NULL);
}

static void
getdns_fini(void **priv)
{
	struct dyn_getdns_addr_state *state;

	AN(priv);
	state = *priv;
	*priv = NULL;
	AN(state);

	AN(state->context);
	AN(state->response);

	getdns_dict_destroy(state->response);
	dyn_getdns_rel_context(&state->context);
	free(state);
}

/* ------------------------------------------------------------
 * srv
 */

struct dyn_getdns_srv_state {
	struct VPFX(dynamic_resolver_context)	*context;
	getdns_dict				*response;
	getdns_list				*replies;
	getdns_list				*answers;
	size_t					n_replies;
	size_t					n_answers;
	size_t					reply;  // next to return
	size_t					answer; // next to return
};

static int
getdns_srv_lookup(struct VPFX(dynamic_resolver) *r,
    const char *service, void **priv)
{
	struct VPFX(dynamic_resolver_context) *c = NULL;
	struct dyn_getdns_srv_state *state;
	getdns_return_t ret = GETDNS_RETURN_GENERIC_ERROR;

	getdns_dict	*reply;
	uint32_t	status;

	AN(r);
	AN(service);
	AN(priv);
	AZ(*priv);

	state = malloc(sizeof *state);
	AN(state);
	memset(state, 0, sizeof *state);

	c = dyn_getdns_get_context(r);
	AN(c);
	AN(c->context);
	state->context = c;

	ret = getdns_service_sync(c->context, service, NULL, &state->response);
	errchk(ret);

	dbg_dump_getdns(state->response);

	ret = getdns_dict_get_int(state->response, "/status", &status);
	errchk(ret);

	if (status != GETDNS_RESPSTATUS_GOOD) {
		ret = status;
		goto out;
	}

	ret = getdns_dict_get_list(state->response,
	    "/replies_tree", &state->replies);
	errchk(ret);

	ret = getdns_list_get_length(state->replies,
	    &state->n_replies);
	errchk(ret);

	if (state->n_replies == 0) {
		ret = GETDNS_RETURN_NO_ANSWERS;
		goto out;
	}

	do {
		ret = getdns_list_get_dict(state->replies,
		    state->reply++, &reply);
		errchk(ret);

		ret = getdns_dict_get_list(reply,
		    "/answer", &state->answers);
		errchk(ret);

		state->answer = 0;

		ret = getdns_list_get_length(state->answers,
		    &state->n_answers);
		errchk(ret);
	} while (state->n_answers == 0 && state->reply < state->n_replies);

	if (state->n_answers == 0)
		ret = GETDNS_RETURN_NO_ANSWERS;

  out:
	*priv = state;
	return (ret);
}

static struct srv_info *
getdns_srv_result(struct srv_info *info, void *priv, void **answerp)
{
	struct dyn_getdns_srv_state *state;
	getdns_dict *rr;
	getdns_bindata *target;
	uint32_t rrtype;
	getdns_return_t ret;
	getdns_dict	*reply;

	AN(info);
	AN(priv);
	AN(answerp);

	AZ(info->target);
	memset(info, 0, sizeof *info);

	if (*answerp == getdns_last)
		return (NULL);

	state = priv;
	if (state->answer >= state->n_answers &&
	    state->reply >= state->n_replies) {
		*answerp = getdns_last;
		return (NULL);
	} else if (*answerp == NULL) {
		*answerp = &state->answer;
	}

	assert(*answerp == &state->answer);

	do {
		// advace to next reply when out of answers
		if (state->answer >= state->n_answers) {
			ret = getdns_list_get_dict(state->replies,
			    state->reply++, &reply);
			if (ret != 0)
				break;

			ret = getdns_dict_get_list(reply,
			    "/answer", &state->answers);
			if (ret != 0)
				break;

			state->answer = 0;

			ret = getdns_list_get_length(state->answers,
			    &state->n_answers);
			if (ret != 0)
				break;
		}

		ret = getdns_list_get_dict(state->answers,
		    state->answer++, &rr);
		AZ(ret);

		ret = getdns_dict_get_int(rr, "type", &rrtype);
		if (ret != 0)
			continue;

		if (rrtype != GETDNS_RRTYPE_SRV)
			continue;

		// at least target and port must be present
		ret = getdns_dict_get_bindata(rr, "/rdata/target", &target);
		if (ret != 0)
			continue;
		ret = getdns_dict_get_int(rr, "/rdata/port", &info->port);
		if (ret != 0)
			continue;

		AZ(getdns_convert_dns_name_to_fqdn(target, &info->target));
		(void) getdns_dict_get_int(rr, "/rdata/priority",
		    &info->priority);
		(void) getdns_dict_get_int(rr, "/rdata/weight",
		    &info->weight);
		(void) getdns_dict_get_int(rr, "/ttl", &info->ttl);

		return (info);
	} while (state->answer < state->n_answers ||
	    state->reply < state->n_replies);

	*answerp = getdns_last;
	return (NULL);
}

static void
getdns_srv_fini(void **priv)
{
	struct dyn_getdns_srv_state *state;

	AN(priv);
	state = *priv;
	*priv = NULL;
	AN(state);

	AN(state->context);
	AN(state->response);

	getdns_dict_destroy(state->response);
	dyn_getdns_rel_context(&state->context);
	free(state);
}

static char *
getdns_details(void *priv)
{
	struct dyn_getdns_srv_state *state = priv;

	if (state == NULL || state->response == NULL)
		return (NULL);

	return (getdns_pretty_print_dict(state->response));
}

struct res_cb res_getdns = {
	.name = "getdns",

	.lookup = getdns_lookup,
	.result = getdns_result,
	.fini = getdns_fini,

	.srv_lookup = getdns_srv_lookup,
	.srv_result = getdns_srv_result,
	.srv_fini = getdns_srv_fini,

	.strerror = dyn_getdns_strerror,
	.details = getdns_details
};
