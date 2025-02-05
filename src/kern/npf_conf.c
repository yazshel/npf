/*-
 * Copyright (c) 2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NPF config loading mechanism.
 *
 * There are few main operations on the config:
 * 1) Read access which is primarily from the npf_packet_handler() et al.
 * 2) Write access on particular set, mainly rule or table updates.
 * 3) Deletion of the config, which is done during the reload operation.
 *
 * Synchronisation
 *
 *	For (1) case, passive serialisation is used to allow concurrent
 *	access to the configuration set (ruleset, etc).  It guarantees
 *	that the config will not be destroyed while accessing it.
 *
 *	Writers, i.e. cases (2) and (3) use mutual exclusion and when
 *	necessary writer-side barrier of the passive serialisation.
 */

#ifdef _KERNEL
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: npf_conf.c,v 1.13 2019/07/23 00:52:01 rmind Exp $");

#include <sys/param.h>
#include <sys/types.h>

#include <sys/atomic.h>
#include <sys/kmem.h>
#include <sys/pserialize.h>
#include <sys/mutex.h>
#endif

#include "npf_impl.h"
#include "npf_conn.h"

struct npf_config {
	npf_ruleset_t *		n_rules;
	npf_tableset_t *	n_tables;
	npf_ruleset_t *		n_nat_rules;
	npf_rprocset_t *	n_rprocs;
	bool			n_default_pass;
};

void
npf_config_init(npf_t *npf)
{
	npf_ruleset_t *rlset, *nset;
	npf_rprocset_t *rpset;
	npf_tableset_t *tset;

	mutex_init(&npf->config_lock, MUTEX_DEFAULT, IPL_SOFTNET);

	/* Load the empty configuration. */
	tset = npf_tableset_create(0);
	rpset = npf_rprocset_create();
	rlset = npf_ruleset_create(0);
	nset = npf_ruleset_create(0);
	npf_config_load(npf, rlset, tset, nset, rpset, NULL, true);
	KASSERT(npf->config != NULL);
}

static void
npf_config_destroy(npf_config_t *nc)
{
	npf_ruleset_destroy(nc->n_rules);
	npf_ruleset_destroy(nc->n_nat_rules);
	npf_rprocset_destroy(nc->n_rprocs);
	npf_tableset_destroy(nc->n_tables);
	kmem_free(nc, sizeof(npf_config_t));
}

void
npf_config_fini(npf_t *npf)
{
	npf_conndb_t *cd = npf_conndb_create();

	/* Flush the connections. */
	mutex_enter(&npf->config_lock);
	npf_conn_tracking(npf, false);
	pserialize_perform(npf->qsbr);
	npf_conn_load(npf, cd, false);
	npf_ifmap_flush(npf);
	mutex_exit(&npf->config_lock);

	npf_config_destroy(npf->config);
	mutex_destroy(&npf->config_lock);
}

/*
 * npf_config_load: the main routine performing configuration load.
 * Performs the necessary synchronisation and destroys the old config.
 */
void
npf_config_load(npf_t *npf, npf_ruleset_t *rset, npf_tableset_t *tset,
    npf_ruleset_t *nset, npf_rprocset_t *rpset,
    npf_conndb_t *conns, bool flush)
{
	const bool load = conns != NULL;
	npf_config_t *nc, *onc;

	nc = kmem_zalloc(sizeof(npf_config_t), KM_SLEEP);
	nc->n_rules = rset;
	nc->n_tables = tset;
	nc->n_nat_rules = nset;
	nc->n_rprocs = rpset;
	nc->n_default_pass = flush;

	/*
	 * Acquire the lock and perform the first phase:
	 * - Scan and use existing dynamic tables, reload only static.
	 * - Scan and use matching NAT policies to preserve the connections.
	 */
	mutex_enter(&npf->config_lock);
	if ((onc = npf->config) != NULL) {
		npf_ruleset_reload(npf, rset, onc->n_rules, load);
		npf_tableset_reload(npf, tset, onc->n_tables);
		npf_ruleset_reload(npf, nset, onc->n_nat_rules, load);
	}

	/*
	 * Set the new config and release the lock.
	 */
	membar_sync();
	npf->config = nc;
	if (onc == NULL) {
		/* Initial load, done. */
		npf_ifmap_flush(npf);
		npf_conn_load(npf, conns, !flush);
		mutex_exit(&npf->config_lock);
		goto done;
	}

	/*
	 * If we are going to flush the connections or load the new ones,
	 * then disable the connection tracking for the grace period.
	 */
	if (flush || conns) {
		npf_conn_tracking(npf, false);
	}

	/* Synchronise: drain all references. */
	pserialize_perform(npf->qsbr);
	if (flush) {
		npf_portmap_flush(npf->portmap);
		npf_ifmap_flush(npf);
	}

	/*
	 * G/C the existing connections and, if passed, load the new ones.
	 * If not flushing - enable the connection tracking.
	 */
	npf_conn_load(npf, conns, !flush);
	mutex_exit(&npf->config_lock);

	/* Finally, it is safe to destroy the old config. */
	npf_config_destroy(onc);
done:
	/* Sync all interface address tables (can be done asynchronously). */
	npf_ifaddr_syncall(npf);
}

/*
 * Writer-side exclusive locking.
 */

void
npf_config_enter(npf_t *npf)
{
	mutex_enter(&npf->config_lock);
}

void
npf_config_exit(npf_t *npf)
{
	mutex_exit(&npf->config_lock);
}

bool
npf_config_locked_p(npf_t *npf)
{
	return mutex_owned(&npf->config_lock);
}

void
npf_config_sync(npf_t *npf)
{
	KASSERT(npf_config_locked_p(npf));
	pserialize_perform(npf->qsbr);
}

/*
 * Reader-side synchronisation routines.
 */

int
npf_config_read_enter(void)
{
	return pserialize_read_enter();
}

void
npf_config_read_exit(int s)
{
	pserialize_read_exit(s);
}

/*
 * Accessors.
 */

npf_ruleset_t *
npf_config_ruleset(npf_t *npf)
{
	return npf->config->n_rules;
}

npf_ruleset_t *
npf_config_natset(npf_t *npf)
{
	return npf->config->n_nat_rules;
}

npf_tableset_t *
npf_config_tableset(npf_t *npf)
{
	return npf->config->n_tables;
}

npf_rprocset_t *
npf_config_rprocs(npf_t *npf)
{
	return npf->config->n_rprocs;
}

bool
npf_default_pass(npf_t *npf)
{
	return npf->config->n_default_pass;
}
