/*-
 * Copyright (c) 2000 John Baldwin <jhb@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This module holds the global variables used by KTR and the ktr_tracepoint()
 * function that does the actual tracing.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ddb.h"
#include "opt_ktr.h"
#include "opt_alq.h"

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/alq.h>
#include <sys/cons.h>
#include <sys/cpuset.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <machine/cpu.h>
#ifdef __sparc64__
#include <machine/ktr.h>
#endif

#ifdef DDB
#include <ddb/ddb.h>
#include <ddb/db_output.h>
#endif

#ifndef KTR_ENTRIES
#define	KTR_ENTRIES	1024
#endif

#ifndef KTR_MASK
#define	KTR_MASK	(0)
#endif

#ifndef KTR_TIME
#define	KTR_TIME	get_cyclecount()
#endif

#ifndef KTR_CPU
#define	KTR_CPU		PCPU_GET(cpuid)
#endif

FEATURE(ktr, "Kernel support for KTR kernel tracing facility");

static SYSCTL_NODE(_debug, OID_AUTO, ktr, CTLFLAG_RD, 0, "KTR options");

int	ktr_mask = KTR_MASK;
TUNABLE_INT("debug.ktr.mask", &ktr_mask);
SYSCTL_INT(_debug_ktr, OID_AUTO, mask, CTLFLAG_RW,
    &ktr_mask, 0, "Bitmask of KTR event classes for which logging is enabled");

int	ktr_compile = KTR_COMPILE;
SYSCTL_INT(_debug_ktr, OID_AUTO, compile, CTLFLAG_RD,
    &ktr_compile, 0, "Bitmask of KTR event classes compiled into the kernel");

int	ktr_entries = KTR_ENTRIES;
SYSCTL_INT(_debug_ktr, OID_AUTO, entries, CTLFLAG_RD,
    &ktr_entries, 0, "Number of entries in the KTR buffer");

int	ktr_version = KTR_VERSION;
SYSCTL_INT(_debug_ktr, OID_AUTO, version, CTLFLAG_RD,
    &ktr_version, 0, "Version of the KTR interface");

cpuset_t ktr_cpumask;
static char ktr_cpumask_str[CPUSETBUFSIZ];
TUNABLE_STR("debug.ktr.cpumask", ktr_cpumask_str, sizeof(ktr_cpumask_str));

static void
ktr_cpumask_initializer(void *dummy __unused)
{

	CPU_FILL(&ktr_cpumask);
#ifdef KTR_CPUMASK
	if (cpusetobj_strscan(&ktr_cpumask, KTR_CPUMASK) == -1)
		CPU_FILL(&ktr_cpumask);
#endif

	/*
	 * TUNABLE_STR() runs with SI_ORDER_MIDDLE priority, thus it must be
	 * already set, if necessary.
	 */
	if (ktr_cpumask_str[0] != '\0' &&
	    cpusetobj_strscan(&ktr_cpumask, ktr_cpumask_str) == -1)
		CPU_FILL(&ktr_cpumask);
}
SYSINIT(ktr_cpumask_initializer, SI_SUB_TUNABLES, SI_ORDER_ANY,
    ktr_cpumask_initializer, NULL);

static int
sysctl_debug_ktr_cpumask(SYSCTL_HANDLER_ARGS)
{
	char lktr_cpumask_str[CPUSETBUFSIZ];
	cpuset_t imask;
	int error;

	cpusetobj_strprint(lktr_cpumask_str, &ktr_cpumask);
	error = sysctl_handle_string(oidp, lktr_cpumask_str,
	    sizeof(lktr_cpumask_str), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (cpusetobj_strscan(&imask, lktr_cpumask_str) == -1)
		return (EINVAL);
	CPU_COPY(&imask, &ktr_cpumask);

	return (error);
}
SYSCTL_PROC(_debug_ktr, OID_AUTO, cpumask,
    CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_STRING, NULL, 0,
    sysctl_debug_ktr_cpumask, "S",
    "Bitmask of CPUs on which KTR logging is enabled");

volatile int	ktr_idx = 0;
struct	ktr_entry ktr_buf[KTR_ENTRIES];

static int
sysctl_debug_ktr_clear(SYSCTL_HANDLER_ARGS)
{
	int clear, error;

	clear = 0;
	error = sysctl_handle_int(oidp, &clear, 0, req);
	if (error || !req->newptr)
		return (error);

	if (clear) {
		bzero(ktr_buf, sizeof(ktr_buf));
		ktr_idx = 0;
	}

	return (error);
}
SYSCTL_PROC(_debug_ktr, OID_AUTO, clear, CTLTYPE_INT|CTLFLAG_RW, 0, 0,
    sysctl_debug_ktr_clear, "I", "Clear KTR Buffer");

#ifdef KTR_VERBOSE
int	ktr_verbose = KTR_VERBOSE;
TUNABLE_INT("debug.ktr.verbose", &ktr_verbose);
SYSCTL_INT(_debug_ktr, OID_AUTO, verbose, CTLFLAG_RW, &ktr_verbose, 0, "");
#endif

#ifdef KTR_ALQ
struct alq *ktr_alq;
char	ktr_alq_file[MAXPATHLEN] = "/tmp/ktr.out";
int	ktr_alq_cnt = 0;
int	ktr_alq_depth = KTR_ENTRIES;
int	ktr_alq_enabled = 0;
int	ktr_alq_failed = 0;
int	ktr_alq_max = 0;

SYSCTL_INT(_debug_ktr, OID_AUTO, alq_max, CTLFLAG_RW, &ktr_alq_max, 0,
    "Maximum number of entries to write");
SYSCTL_INT(_debug_ktr, OID_AUTO, alq_cnt, CTLFLAG_RD, &ktr_alq_cnt, 0,
    "Current number of written entries");
SYSCTL_INT(_debug_ktr, OID_AUTO, alq_failed, CTLFLAG_RD, &ktr_alq_failed, 0,
    "Number of times we overran the buffer");
SYSCTL_INT(_debug_ktr, OID_AUTO, alq_depth, CTLFLAG_RW, &ktr_alq_depth, 0,
    "Number of items in the write buffer");
SYSCTL_STRING(_debug_ktr, OID_AUTO, alq_file, CTLFLAG_RW, ktr_alq_file,
    sizeof(ktr_alq_file), "KTR logging file");

static int
sysctl_debug_ktr_alq_enable(SYSCTL_HANDLER_ARGS)
{
	int error;
	int enable;

	enable = ktr_alq_enabled;

	error = sysctl_handle_int(oidp, &enable, 0, req);
	if (error || !req->newptr)
		return (error);

	if (enable) {
		if (ktr_alq_enabled)
			return (0);
		error = alq_open(&ktr_alq, (const char *)ktr_alq_file,
		    req->td->td_ucred, ALQ_DEFAULT_CMODE,
		    sizeof(struct ktr_entry), ktr_alq_depth);
		if (error == 0) {
			ktr_alq_cnt = 0;
			ktr_alq_failed = 0;
			ktr_alq_enabled = 1;
		}
	} else {
		if (ktr_alq_enabled == 0)
			return (0);
		ktr_alq_enabled = 0;
		alq_close(ktr_alq);
		ktr_alq = NULL;
	}

	return (error);
}
SYSCTL_PROC(_debug_ktr, OID_AUTO, alq_enable,
    CTLTYPE_INT|CTLFLAG_RW, 0, 0, sysctl_debug_ktr_alq_enable,
    "I", "Enable KTR logging");
#endif

void
ktr_tracepoint(u_int mask, const char *file, int line, const char *format,
    u_long arg1, u_long arg2, u_long arg3, u_long arg4, u_long arg5,
    u_long arg6)
{
	struct ktr_entry *entry;
#ifdef KTR_ALQ
	struct ale *ale = NULL;
#endif
	int newindex, saveindex;
#if defined(KTR_VERBOSE) || defined(KTR_ALQ)
	struct thread *td;
#endif
	int cpu;

	if (panicstr)
		return;
	if ((ktr_mask & mask) == 0)
		return;
	cpu = KTR_CPU;
	if (!CPU_ISSET(cpu, &ktr_cpumask))
		return;
#if defined(KTR_VERBOSE) || defined(KTR_ALQ)
	td = curthread;
	if (td->td_pflags & TDP_INKTR)
		return;
	td->td_pflags |= TDP_INKTR;
#endif
#ifdef KTR_ALQ
	if (ktr_alq_enabled) {
		if (td->td_critnest == 0 &&
		    (td->td_flags & TDF_IDLETD) == 0 &&
		    td != ald_thread) {
			if (ktr_alq_max && ktr_alq_cnt > ktr_alq_max)
				goto done;
			if ((ale = alq_get(ktr_alq, ALQ_NOWAIT)) == NULL) {
				ktr_alq_failed++;
				goto done;
			}
			ktr_alq_cnt++;
			entry = (struct ktr_entry *)ale->ae_data;
		} else {
			goto done;
		}
	} else
#endif
	{
		do {
			saveindex = ktr_idx;
			newindex = (saveindex + 1) % KTR_ENTRIES;
		} while (atomic_cmpset_rel_int(&ktr_idx, saveindex, newindex) == 0);
		entry = &ktr_buf[saveindex];
	}
	entry->ktr_timestamp = KTR_TIME;
	entry->ktr_cpu = cpu;
	entry->ktr_thread = curthread;
	if (file != NULL)
		while (strncmp(file, "../", 3) == 0)
			file += 3;
	entry->ktr_file = file;
	entry->ktr_line = line;
#ifdef KTR_VERBOSE
	if (ktr_verbose) {
#ifdef SMP
		printf("cpu%d ", cpu);
#endif
		if (ktr_verbose > 1) {
			printf("%s.%d\t", entry->ktr_file,
			    entry->ktr_line);
		}
		printf(format, arg1, arg2, arg3, arg4, arg5, arg6);
		printf("\n");
	}
#endif
	entry->ktr_desc = format;
	entry->ktr_parms[0] = arg1;
	entry->ktr_parms[1] = arg2;
	entry->ktr_parms[2] = arg3;
	entry->ktr_parms[3] = arg4;
	entry->ktr_parms[4] = arg5;
	entry->ktr_parms[5] = arg6;
#ifdef KTR_ALQ
	if (ktr_alq_enabled && ale)
		alq_post(ktr_alq, ale);
done:
#endif
#if defined(KTR_VERBOSE) || defined(KTR_ALQ)
	td->td_pflags &= ~TDP_INKTR;
#endif
}

#ifdef DDB

struct tstate {
	int	cur;
	int	first;
};
static	struct tstate tstate;
static	int db_ktr_verbose;
static	int db_mach_vtrace(void);

DB_SHOW_COMMAND(ktr, db_ktr_all)
{
	
	tstate.cur = (ktr_idx - 1) % KTR_ENTRIES;
	tstate.first = -1;
	db_ktr_verbose = 0;
	db_ktr_verbose |= (strchr(modif, 'v') != NULL) ? 2 : 0;
	db_ktr_verbose |= (strchr(modif, 'V') != NULL) ? 1 : 0; /* just timestap please */
	if (strchr(modif, 'a') != NULL) {
		db_disable_pager();
		while (cncheckc() != -1)
			if (db_mach_vtrace() == 0)
				break;
	} else {
		while (!db_pager_quit)
			if (db_mach_vtrace() == 0)
				break;
	}
}

static int
db_mach_vtrace(void)
{
	struct ktr_entry	*kp;

	if (tstate.cur == tstate.first) {
		db_printf("--- End of trace buffer ---\n");
		return (0);
	}
	kp = &ktr_buf[tstate.cur];

	/* Skip over unused entries. */
	if (kp->ktr_desc == NULL) {
		db_printf("--- End of trace buffer ---\n");
		return (0);
	}
	db_printf("%d (%p", tstate.cur, kp->ktr_thread);
#ifdef SMP
	db_printf(":cpu%d", kp->ktr_cpu);
#endif
	db_printf(")");
	if (db_ktr_verbose >= 1) {
		db_printf(" %10.10lld", (long long)kp->ktr_timestamp);
	}
	if (db_ktr_verbose >= 2) {
		db_printf(" %s.%d", kp->ktr_file, kp->ktr_line);
	}
	db_printf(": ");
	db_printf(kp->ktr_desc, kp->ktr_parms[0], kp->ktr_parms[1],
	    kp->ktr_parms[2], kp->ktr_parms[3], kp->ktr_parms[4],
	    kp->ktr_parms[5]);
	db_printf("\n");

	if (tstate.first == -1)
		tstate.first = tstate.cur;

	if (--tstate.cur < 0)
		tstate.cur = KTR_ENTRIES - 1;

	return (1);
}

#endif	/* DDB */