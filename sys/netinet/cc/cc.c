/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2007-2008
 *	Swinburne University of Technology, Melbourne, Australia.
 * Copyright (c) 2009-2010 Lawrence Stewart <lstewart@freebsd.org>
 * Copyright (c) 2010 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed at the Centre for Advanced Internet
 * Architectures, Swinburne University of Technology, by Lawrence Stewart and
 * James Healy, made possible in part by a grant from the Cisco University
 * Research Program Fund at Community Foundation Silicon Valley.
 *
 * Portions of this software were developed at the Centre for Advanced
 * Internet Architectures, Swinburne University of Technology, Melbourne,
 * Australia by David Hayes under sponsorship from the FreeBSD Foundation.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This software was first released in 2007 by James Healy and Lawrence Stewart
 * whilst working on the NewTCP research project at Swinburne University of
 * Technology's Centre for Advanced Internet Architectures, Melbourne,
 * Australia, which was made possible in part by a grant from the Cisco
 * University Research Program Fund at Community Foundation Silicon Valley.
 * More details are available at:
 *   http://caia.swin.edu.au/urp/newtcp/
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");
#include <opt_cc.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>

#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_log_buf.h>
#include <netinet/tcp_hpts.h>
#include <netinet/cc/cc.h>
#include <netinet/cc/cc_module.h>

/*
 * Have a sane default if no CC_DEFAULT is specified in the kernel config file.
 */
#ifndef CC_DEFAULT
#define CC_DEFAULT "newreno"
#endif

MALLOC_DEFINE(M_CC_MEM, "CC Mem", "Congestion Control State memory");

/*
 * List of available cc algorithms on the current system. First element
 * is used as the system default CC algorithm.
 */
struct cc_head cc_list = STAILQ_HEAD_INITIALIZER(cc_list);

/* Protects the cc_list TAILQ. */
struct rwlock cc_list_lock;

VNET_DEFINE(struct cc_algo *, default_cc_ptr) = NULL;

VNET_DEFINE(uint32_t, newreno_beta) = 50;
#define V_newreno_beta VNET(newreno_beta)

/*
 * Sysctl handler to show and change the default CC algorithm.
 */
static int
cc_default_algo(SYSCTL_HANDLER_ARGS)
{
	char default_cc[TCP_CA_NAME_MAX];
	struct cc_algo *funcs;
	int error;

	/* Get the current default: */
	CC_LIST_RLOCK();
	if (CC_DEFAULT_ALGO() != NULL)
		strlcpy(default_cc, CC_DEFAULT_ALGO()->name, sizeof(default_cc));
	else
		memset(default_cc, 0, TCP_CA_NAME_MAX);
	CC_LIST_RUNLOCK();

	error = sysctl_handle_string(oidp, default_cc, sizeof(default_cc), req);

	/* Check for error or no change */
	if (error != 0 || req->newptr == NULL)
		goto done;

	error = ESRCH;
	/* Find algo with specified name and set it to default. */
	CC_LIST_RLOCK();
	STAILQ_FOREACH(funcs, &cc_list, entries) {
		if (strncmp(default_cc, funcs->name, sizeof(default_cc)))
			continue;
		V_default_cc_ptr = funcs;
		error = 0;
		break;
	}
	CC_LIST_RUNLOCK();
done:
	return (error);
}

/*
 * Sysctl handler to display the list of available CC algorithms.
 */
static int
cc_list_available(SYSCTL_HANDLER_ARGS)
{
	struct cc_algo *algo;
	struct sbuf *s;
	int err, first, nalgos;

	err = nalgos = 0;
	first = 1;

	CC_LIST_RLOCK();
	STAILQ_FOREACH(algo, &cc_list, entries) {
		nalgos++;
	}
	CC_LIST_RUNLOCK();
	if (nalgos == 0) {
		return (ENOENT);
	}
	s = sbuf_new(NULL, NULL, nalgos * TCP_CA_NAME_MAX, SBUF_FIXEDLEN);

	if (s == NULL)
		return (ENOMEM);

	/*
	 * It is theoretically possible for the CC list to have grown in size
	 * since the call to sbuf_new() and therefore for the sbuf to be too
	 * small. If this were to happen (incredibly unlikely), the sbuf will
	 * reach an overflow condition, sbuf_printf() will return an error and
	 * the sysctl will fail gracefully.
	 */
	CC_LIST_RLOCK();
	STAILQ_FOREACH(algo, &cc_list, entries) {
		err = sbuf_printf(s, first ? "%s" : ", %s", algo->name);
		if (err) {
			/* Sbuf overflow condition. */
			err = EOVERFLOW;
			break;
		}
		first = 0;
	}
	CC_LIST_RUNLOCK();

	if (!err) {
		sbuf_finish(s);
		err = sysctl_handle_string(oidp, sbuf_data(s), 0, req);
	}

	sbuf_delete(s);
	return (err);
}

/*
 * Return the number of times a proposed removal_cc is
 * being used as the default.
 */
static int
cc_check_default(struct cc_algo *remove_cc)
{
	int cnt = 0;
	VNET_ITERATOR_DECL(vnet_iter);

	CC_LIST_LOCK_ASSERT();

	VNET_LIST_RLOCK_NOSLEEP();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		if ((CC_DEFAULT_ALGO() != NULL) &&
		    strncmp(CC_DEFAULT_ALGO()->name,
			    remove_cc->name,
			    TCP_CA_NAME_MAX) == 0) {
			cnt++;
		}
		CURVNET_RESTORE();
	}
	VNET_LIST_RUNLOCK_NOSLEEP();
	return (cnt);
}

/*
 * Initialise CC subsystem on system boot.
 */
static void
cc_init(void)
{
	CC_LIST_LOCK_INIT();
	STAILQ_INIT(&cc_list);
}

/*
 * Returns non-zero on success, 0 on failure.
 */
int
cc_deregister_algo(struct cc_algo *remove_cc)
{
	struct cc_algo *funcs, *tmpfuncs;
	int err;

	err = ENOENT;

	/* Remove algo from cc_list so that new connections can't use it. */
	CC_LIST_WLOCK();
	STAILQ_FOREACH_SAFE(funcs, &cc_list, entries, tmpfuncs) {
		if (funcs == remove_cc) {
			if (cc_check_default(remove_cc)) {
				err = EBUSY;
				break;
			}
			/* Add a temp flag to stop new adds to it */
			funcs->flags |= CC_MODULE_BEING_REMOVED;
			break;
		}
	}
	CC_LIST_WUNLOCK();
	err = tcp_ccalgounload(remove_cc);
	/*
	 * Now back through and we either remove the temp flag
	 * or pull the registration.
	 */
	CC_LIST_WLOCK();
	STAILQ_FOREACH_SAFE(funcs, &cc_list, entries, tmpfuncs) {
		if (funcs == remove_cc) {
			if (err == 0)
				STAILQ_REMOVE(&cc_list, funcs, cc_algo, entries);
			else
				funcs->flags &= ~CC_MODULE_BEING_REMOVED;
			break;
		}
	}
	CC_LIST_WUNLOCK();
	return (err);
}

/*
 * Returns 0 on success, non-zero on failure.
 */
int
cc_register_algo(struct cc_algo *add_cc)
{
	struct cc_algo *funcs;
	int err;

	err = 0;

	/*
	 * Iterate over list of registered CC algorithms and make sure
	 * we're not trying to add a duplicate.
	 */
	CC_LIST_WLOCK();
	STAILQ_FOREACH(funcs, &cc_list, entries) {
		if (funcs == add_cc ||
		    strncmp(funcs->name, add_cc->name,
			    TCP_CA_NAME_MAX) == 0) {
			err = EEXIST;
			break;
		}
	}
	/*
	 * The first loaded congestion control module will become
	 * the default until we find the "CC_DEFAULT" defined in
	 * the config (if we do).
	 */
	if (!err) {
		STAILQ_INSERT_TAIL(&cc_list, add_cc, entries);
		if (strcmp(add_cc->name, CC_DEFAULT) == 0) {
			V_default_cc_ptr = add_cc;
		} else if (V_default_cc_ptr == NULL) {
			V_default_cc_ptr = add_cc;
		}
	}
	CC_LIST_WUNLOCK();

	return (err);
}

static void
vnet_cc_sysinit(void *arg)
{
	struct cc_algo *cc;

	if (IS_DEFAULT_VNET(curvnet))
		return;

	CURVNET_SET(vnet0);
	cc = V_default_cc_ptr;
	CURVNET_RESTORE();

	V_default_cc_ptr = cc;
}
VNET_SYSINIT(vnet_cc_sysinit, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY,
    vnet_cc_sysinit, NULL);

/*
 * Perform any necessary tasks before we exit congestion recovery.
 */
void
newreno_cc_post_recovery(struct cc_var *ccv)
{
	int pipe;

	if (IN_FASTRECOVERY(CCV(ccv, t_flags))) {
		/*
		 * Fast recovery will conclude after returning from this
		 * function. Window inflation should have left us with
		 * approximately snd_ssthresh outstanding data. But in case we
		 * would be inclined to send a burst, better to do it via the
		 * slow start mechanism.
		 *
		 * XXXLAS: Find a way to do this without needing curack
		 */
		if (V_tcp_do_newsack)
			pipe = tcp_compute_pipe(ccv->ccvc.tcp);
		else
			pipe = CCV(ccv, snd_max) - ccv->curack;
		if (pipe < CCV(ccv, snd_ssthresh))
			/*
			 * Ensure that cwnd does not collapse to 1 MSS under
			 * adverse conditons. Implements RFC6582
			 */
			CCV(ccv, snd_cwnd) = max(pipe, CCV(ccv, t_maxseg)) +
			    CCV(ccv, t_maxseg);
		else
			CCV(ccv, snd_cwnd) = CCV(ccv, snd_ssthresh);
	}
}

void
newreno_cc_after_idle(struct cc_var *ccv)
{
	uint32_t rw;
	/*
	 * If we've been idle for more than one retransmit timeout the old
	 * congestion window is no longer current and we have to reduce it to
	 * the restart window before we can transmit again.
	 *
	 * The restart window is the initial window or the last CWND, whichever
	 * is smaller.
	 *
	 * This is done to prevent us from flooding the path with a full CWND at
	 * wirespeed, overloading router and switch buffers along the way.
	 *
	 * See RFC5681 Section 4.1. "Restarting Idle Connections".
	 *
	 * In addition, per RFC2861 Section 2, the ssthresh is set to the
	 * maximum of the former ssthresh or 3/4 of the old cwnd, to
	 * not exit slow-start prematurely.
	 */
	rw = tcp_compute_initwnd(tcp_maxseg(ccv->ccvc.tcp));

	CCV(ccv, snd_ssthresh) = max(CCV(ccv, snd_ssthresh),
	    CCV(ccv, snd_cwnd)-(CCV(ccv, snd_cwnd)>>2));

	CCV(ccv, snd_cwnd) = min(rw, CCV(ccv, snd_cwnd));
}

/*
 * Perform any necessary tasks before we enter congestion recovery.
 */
void
newreno_cc_cong_signal(struct cc_var *ccv, uint32_t type)
{
	uint32_t cwin, factor;
	u_int mss;

	cwin = CCV(ccv, snd_cwnd);
	mss = tcp_fixed_maxseg(ccv->ccvc.tcp);
	/*
	 * Other TCP congestion controls use newreno_cong_signal(), but
	 * with their own private cc_data. Make sure the cc_data is used
	 * correctly.
	 */
	factor = V_newreno_beta;

	/* Catch algos which mistakenly leak private signal types. */
	KASSERT((type & CC_SIGPRIVMASK) == 0,
	    ("%s: congestion signal type 0x%08x is private\n", __func__, type));

	cwin = max(((uint64_t)cwin * (uint64_t)factor) / (100ULL * (uint64_t)mss),
	    2) * mss;

	switch (type) {
	case CC_NDUPACK:
		if (!IN_FASTRECOVERY(CCV(ccv, t_flags))) {
			if (!IN_CONGRECOVERY(CCV(ccv, t_flags)))
				CCV(ccv, snd_ssthresh) = cwin;
			ENTER_RECOVERY(CCV(ccv, t_flags));
		}
		break;
	case CC_ECN:
		if (!IN_CONGRECOVERY(CCV(ccv, t_flags))) {
			CCV(ccv, snd_ssthresh) = cwin;
			CCV(ccv, snd_cwnd) = cwin;
			ENTER_CONGRECOVERY(CCV(ccv, t_flags));
		}
		break;
	case CC_RTO:
		CCV(ccv, snd_ssthresh) = max(min(CCV(ccv, snd_wnd),
						 CCV(ccv, snd_cwnd)) / 2 / mss,
					     2) * mss;
		CCV(ccv, snd_cwnd) = mss;
		break;
	}
}

void
newreno_cc_ack_received(struct cc_var *ccv, uint16_t type)
{
	if (type == CC_ACK && !IN_RECOVERY(CCV(ccv, t_flags)) &&
	    (ccv->flags & CCF_CWND_LIMITED)) {
		u_int cw = CCV(ccv, snd_cwnd);
		u_int incr = CCV(ccv, t_maxseg);

		/*
		 * Regular in-order ACK, open the congestion window.
		 * Method depends on which congestion control state we're
		 * in (slow start or cong avoid) and if ABC (RFC 3465) is
		 * enabled.
		 *
		 * slow start: cwnd <= ssthresh
		 * cong avoid: cwnd > ssthresh
		 *
		 * slow start and ABC (RFC 3465):
		 *   Grow cwnd exponentially by the amount of data
		 *   ACKed capping the max increment per ACK to
		 *   (abc_l_var * maxseg) bytes.
		 *
		 * slow start without ABC (RFC 5681):
		 *   Grow cwnd exponentially by maxseg per ACK.
		 *
		 * cong avoid and ABC (RFC 3465):
		 *   Grow cwnd linearly by maxseg per RTT for each
		 *   cwnd worth of ACKed data.
		 *
		 * cong avoid without ABC (RFC 5681):
		 *   Grow cwnd linearly by approximately maxseg per RTT using
		 *   maxseg^2 / cwnd per ACK as the increment.
		 *   If cwnd > maxseg^2, fix the cwnd increment at 1 byte to
		 *   avoid capping cwnd.
		 */
		if (cw > CCV(ccv, snd_ssthresh)) {
			if (V_tcp_do_rfc3465) {
				if (ccv->flags & CCF_ABC_SENTAWND)
					ccv->flags &= ~CCF_ABC_SENTAWND;
				else
					incr = 0;
			} else
				incr = max((incr * incr / cw), 1);
		} else if (V_tcp_do_rfc3465) {
			/*
			 * In slow-start with ABC enabled and no RTO in sight?
			 * (Must not use abc_l_var > 1 if slow starting after
			 * an RTO. On RTO, snd_nxt = snd_una, so the
			 * snd_nxt == snd_max check is sufficient to
			 * handle this).
			 *
			 * XXXLAS: Find a way to signal SS after RTO that
			 * doesn't rely on tcpcb vars.
			 */
			uint16_t abc_val;

			if (ccv->flags & CCF_USE_LOCAL_ABC)
				abc_val = ccv->labc;
			else
				abc_val = V_tcp_abc_l_var;
			if (CCV(ccv, snd_nxt) == CCV(ccv, snd_max))
				incr = min(ccv->bytes_this_ack,
				    ccv->nsegs * abc_val *
				    CCV(ccv, t_maxseg));
			else
				incr = min(ccv->bytes_this_ack, CCV(ccv, t_maxseg));

		}
		/* ABC is on by default, so incr equals 0 frequently. */
		if (incr > 0)
			CCV(ccv, snd_cwnd) = min(cw + incr,
			    TCP_MAXWIN << CCV(ccv, snd_scale));
	}
}

/*
 * Handles kld related events. Returns 0 on success, non-zero on failure.
 */
int
cc_modevent(module_t mod, int event_type, void *data)
{
	struct cc_algo *algo;
	int err;

	err = 0;
	algo = (struct cc_algo *)data;

	switch(event_type) {
	case MOD_LOAD:
		if ((algo->cc_data_sz == NULL) && (algo->cb_init != NULL)) {
			/*
			 * A module must have a cc_data_sz function
			 * even if it has no data it should return 0.
			 */
			printf("Module Load Fails, it lacks a cc_data_sz() function but has a cb_init()!\n");
			err = EINVAL;
			break;
		}
		if (algo->mod_init != NULL)
			err = algo->mod_init();
		if (!err)
			err = cc_register_algo(algo);
		break;

	case MOD_QUIESCE:
	case MOD_SHUTDOWN:
	case MOD_UNLOAD:
		err = cc_deregister_algo(algo);
		if (!err && algo->mod_destroy != NULL)
			algo->mod_destroy();
		if (err == ENOENT)
			err = 0;
		break;

	default:
		err = EINVAL;
		break;
	}

	return (err);
}

SYSINIT(cc, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_FIRST, cc_init, NULL);

/* Declare sysctl tree and populate it. */
SYSCTL_NODE(_net_inet_tcp, OID_AUTO, cc, CTLFLAG_RW | CTLFLAG_MPSAFE, NULL,
    "Congestion control related settings");

SYSCTL_PROC(_net_inet_tcp_cc, OID_AUTO, algorithm,
    CTLFLAG_VNET | CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE,
    NULL, 0, cc_default_algo, "A",
    "Default congestion control algorithm");

SYSCTL_PROC(_net_inet_tcp_cc, OID_AUTO, available,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
    NULL, 0, cc_list_available, "A",
    "List available congestion control algorithms");

VNET_DEFINE(int, cc_do_abe) = 0;
SYSCTL_INT(_net_inet_tcp_cc, OID_AUTO, abe, CTLFLAG_VNET | CTLFLAG_RW,
    &VNET_NAME(cc_do_abe), 0,
    "Enable draft-ietf-tcpm-alternativebackoff-ecn (TCP Alternative Backoff with ECN)");

VNET_DEFINE(int, cc_abe_frlossreduce) = 0;
SYSCTL_INT(_net_inet_tcp_cc, OID_AUTO, abe_frlossreduce, CTLFLAG_VNET | CTLFLAG_RW,
    &VNET_NAME(cc_abe_frlossreduce), 0,
    "Apply standard beta instead of ABE-beta during ECN-signalled congestion "
    "recovery episodes if loss also needs to be repaired");
