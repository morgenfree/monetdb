/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/*
 * Martin Kersten
 * Performance profiler
 * A key issue in developing fast programs using the Monet database
 * back-end requires a keen eye on where performance is lost.
 * Although performance tracking and measurements are highly
 * application dependent, a simple to use tool makes life
 * a lot easier.
 *
 * Activation of the performance monitor has a global effect,
 * i.e. all concurrent actions on the kernel are traced,
 * but the events are only sent to the client initiated
 * the profiler thread.
 *
 * The profiler event can be handled in several ways.
 * The default strategy is to ship the event record immediately over a stream
 * to a performance monitor.
 * An alternative strategy is preparation of off-line performance analysis.
 *
 * To reduce the  interference of performance measurement with
 * the experiments, the user can use an event cache, which is
 * emptied explicitly upon need.
 */
/*
 * Using the Monet Performance Profiler is constrained by the mal_profiler.
 */
#include "monetdb_config.h"
#include "profiler.h"

str
CMDopenProfilerStream(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pc)
{
	(void) cntxt;
	(void) mb;
	(void) stk;
	(void) pc;
	return openProfilerStream(cntxt->fdout);
}

str
CMDcloseProfilerStream(void *res)
{
	(void) res;
	return closeProfilerStream();
}

str
CMDsetProfilerFile(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str *fnme = getArgReference_str(stk,pci,1);
	(void) mb;		/* fool compiler */
	return setLogFile(cntxt->fdout,cntxt->nspace, *fnme);
}

str
CMDsetProfilerStream (Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str *host = getArgReference_str(stk,pci,1);
	int *port = getArgReference_int(stk,pci,2);
	(void) mb;		/* fool compiler */
	return setLogStream(cntxt->nspace, *host, *port);
}

str
CMDstopProfiler(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	(void) cntxt;
	(void) mb;
	(void) stk;
	(void) pci;

	return stopProfiler();
}

str
CMDnoopProfiler(void *res)
{
	(void) res;		/* fool compiler */
	return MAL_SUCCEED;
}

/*
 * Tracing an active system.
 */
str
CMDclearTrace(void *res)
{
	(void) res;		/* fool compiler */
	clearTrace();
	return MAL_SUCCEED;
}

str
CMDdumpTrace(void *res)
{
	(void) res;		/* fool compiler */
	throw(MAL, "profiler.dump", PROGRAM_NYI);
}

str
CMDgetTrace(bat *res, str *ev)
{
	BAT *bn;

	(void) res;		/* fool compiler */
	bn = getTrace(*ev);
	if (bn) {
		BBPkeepref(*res = bn->batCacheid);
		return MAL_SUCCEED;
	}
	throw(MAL, "getTrace", RUNTIME_OBJECT_MISSING  "%s",*ev);
}

str
CMDsetHeartbeat(void *res, int *ev)
{
	(void) res;
	setHeartbeat(*ev);
	return MAL_SUCCEED;
}

str
CMDcleanup(void *ret){
	(void) ret;
	return cleanupProfiler();
}

str
CMDgetDiskReads(lng *ret)
{
	*ret= getDiskReads();
	return MAL_SUCCEED;
}
str
CMDgetDiskWrites(lng *ret)
{
	*ret= getDiskWrites();
	return MAL_SUCCEED;
}
str
CMDgetUserTime(lng *ret)
{
	*ret= getUserTime();
	return MAL_SUCCEED;
}
str
CMDgetSystemTime(lng *ret)
{
	*ret= getUserTime();
	return MAL_SUCCEED;
}

str
CMDtomograph(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pc)
{
	int beat = *getArgReference_int(stk,pc,1);
	(void) mb;
	if( beat < 0)
		throw(MAL,"profiler.tomograph","negative heart beat not allowed");
	startProfiler(cntxt->user, -1, beat);
	return MAL_SUCCEED;
}

str
CMDstethoscope(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pc)
{
	int beat = *getArgReference_int(stk,pc,1);

	(void) mb;
	if( beat < 0)
		throw(MAL,"profiler.stethoscope","negative heart beat not allowed");
	startProfiler(cntxt->user, 1, beat);
	return MAL_SUCCEED;
}

str
CMDcpustats(lng *user, lng *nice, lng *sys, lng *idle, lng *iowait)
{
	profilerGetCPUStat(user,nice,sys,idle,iowait);
	return MAL_SUCCEED;
}

str
CMDcpuloadPercentage(int *cycles, int *io, lng *user, lng *nice, lng *sys, lng *idle, lng *iowait)
{
	lng userN, niceN, sysN, idleN, iowaitN, N;
	*cycles = 0;
	*io = 0;
	profilerGetCPUStat(&userN,&niceN,&sysN,&idleN,&iowaitN);
	N = (userN - *user + niceN - *nice + sysN - *sys);
	if ( N){
		*cycles = (int) ( ((double) N) / (N + idleN - *idle + iowaitN - *iowait) *100);
		*io = (int) ( ((double) iowaitN- *iowait) / (N + idleN - *idle + iowaitN - *iowait) *100);
	}
	return MAL_SUCCEED;
}
