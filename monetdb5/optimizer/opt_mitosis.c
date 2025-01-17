/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "opt_mitosis.h"
#include "mal_interpreter.h"
#include <gdk_utils.h>

static int
eligible(MalBlkPtr mb)
{
	InstrPtr p;
	int i;
	for (i = 1; i < mb->stop; i++) {
		p = getInstrPtr(mb, i);
		if (getModuleId(p) == sqlRef && getFunctionId(p) == assertRef &&
			p->argc > 2 && getArgType(mb, p, 2) == TYPE_str &&
			isVarConstant(mb, getArg(p, 2)) &&
			getVarConstant(mb, getArg(p, 2)).val.sval != NULL &&
			(strstr(getVarConstant(mb, getArg(p, 2)).val.sval, "PRIMARY KEY constraint") ||
			 strstr(getVarConstant(mb, getArg(p, 2)).val.sval, "UNIQUE constraint")))
			return 0;
	}
	return 1;
}

static int
getVarMergeTableId(MalBlkPtr mb, int v)
{
	VarPtr p = varGetProp(mb, v, mtProp);

	if (!p)
		return -1;
	if (p->value.vtype == TYPE_int)
		return p->value.val.ival;
	return -1;
}


/* The plans are marked with the concurrent user load.
 *  * If this has changed, we may want to recompile the query
 *   */
int
OPTmitosisPlanOverdue(Client cntxt, str fname)
{
    Symbol s;

    s = findSymbol(cntxt->nspace, userRef, fname);
    if(s )
        return s->def->activeClients != MCactiveClients();
    return 0;
}

int
OPTmitosisImplementation(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p)
{
	int i, j, limit, slimit, estimate = 0, pieces = 1, mito_parts = 0, mito_size = 0, row_size = 0, mt = -1;
	str schema = 0, table = 0;
	wrd r = 0, rowcnt = 0;    /* table should be sizeable to consider parallel execution*/
	InstrPtr q, *old, target = 0;
	size_t argsize = 6 * sizeof(lng);
	/*     per op:   6 = (2+1)*2   <=  2 args + 1 res, each with head & tail */
	int threads = GDKnr_threads ? GDKnr_threads : 1;
	int activeClients;

	(void) cntxt;
	(void) stk;
	if (!eligible(mb))
		return 0;

	activeClients = mb->activeClients = MCactiveClients();
	old = mb->stmt;
	for (i = 1; i < mb->stop; i++) {
		InstrPtr p = old[i];

		/* mitosis/mergetable bailout conditions */
		
		/* Mergetable cannot handle order related batcalc ops */
		if ((getModuleId(p) == batcalcRef || getModuleId(p) == sqlRef) && 
		   (getFunctionId(p) == rankRef || getFunctionId(p) == rank_grpRef ||
		    getFunctionId(p) == mark_grpRef || getFunctionId(p) == dense_rank_grpRef)) 
			return 0;

		if (p->argc > 2 && getModuleId(p) == aggrRef && 
		        getFunctionId(p) != subcountRef &&
		    	getFunctionId(p) != subminRef &&
		    	getFunctionId(p) != submaxRef &&
		    	getFunctionId(p) != subavgRef &&
		    	getFunctionId(p) != subsumRef &&
		    	getFunctionId(p) != subprodRef)
			return 0;

		if (p->argc > 2 && getModuleId(p) == rapiRef && 
		        getFunctionId(p) == subeval_aggrRef)
			return 0;

		/* Mergetable cannot handle intersect/except's for now */
		if (getModuleId(p) == algebraRef && getFunctionId(p) == groupbyRef) 
			return 0;

		/* locate the largest non-partitioned table */
		if (getModuleId(p) != sqlRef || (getFunctionId(p) != bindRef && getFunctionId(p) != bindidxRef))
			continue;
		/* don't split insert BATs */
		if (getVarConstant(mb, getArg(p, 5)).val.ival == 1)
			continue;
		if (p->argc > 6)
			continue;  /* already partitioned */
		/*
		 * The SQL optimizer already collects the counts of the base
		 * table and passes them on as a row property.  All pieces for a
		 * single subplan should ideally fit together.
		 */
		r = getVarRows(mb, getArg(p, 0));
		if (r >= rowcnt) {
			/* the rowsize depends on the column types, assume void-headed */
			row_size = ATOMsize(getColumnType(getArgType(mb,p,0)));
			rowcnt = r;
			target = p;
			estimate++;
			r = 0;
		}
	}
	if (target == 0)
		return 0;
	/*
	 * The number of pieces should be based on the footprint of the
	 * queryplan, such that preferrably it can be handled without
	 * swapping intermediates.  For the time being we just go for pieces
	 * that fit into memory in isolation.  A fictive rowcount is derived
	 * based on argument types, such that all pieces would fit into
	 * memory conveniently for processing. We attempt to use not more
	 * threads than strictly needed.
	 * Experience shows that the pieces should not be too small.
	 * If we should limit to |threads| is still an open issue.
	 *
	 * Take into account the number of client connections, 
	 * because all user together are responsible for resource contentions
	 */
	r = (wrd) (monet_memory / argsize);
	/* if data exceeds memory size,
	 * i.e., (rowcnt*argsize > monet_memory),
	 * i.e., (rowcnt > monet_memory/argsize = r) */
	if (rowcnt > r && r / threads / activeClients > 0) {
		/* create |pieces| > |threads| partitions such that
		 * |threads| partitions at a time fit in memory,
		 * i.e., (threads*(rowcnt/pieces) <= r),
		 * i.e., (rowcnt/pieces <= r/threads),
		 * i.e., (pieces => rowcnt/(r/threads))
		 * (assuming that (r > threads*MINPARTCNT)) */
		pieces = (int) (rowcnt / (r / threads / activeClients)) + 1;
	} else if (rowcnt > MINPARTCNT) {
	/* exploit parallelism, but ensure minimal partition size to
	 * limit overhead */
		pieces = (int) MIN((rowcnt / MINPARTCNT), (wrd) threads);
	}
	/* when testing, always aim for full parallelism, but avoid
	 * empty pieces */
	FORCEMITODEBUG
	if (pieces < threads)
		pieces = (int) MIN((wrd) threads, rowcnt);
	/* prevent plan explosion */
	if (pieces > MAXSLICES)
		pieces = MAXSLICES;
	/* to enable experimentation we introduce the option to set
	 * the number of parts required and/or the size of each chunk (in K)
	 */
	mito_parts = GDKgetenv_int("mito_parts", 0);
	if (mito_parts > 0) 
		pieces = mito_parts;
	mito_size = GDKgetenv_int("mito_size", 0);
	if (mito_size > 0) 
		pieces = (int) ((rowcnt * row_size) / (mito_size * 1024));

	OPTDEBUGmitosis
	mnstr_printf(cntxt->fdout, "#opt_mitosis: target is %s.%s "
							   " with " SSZFMT " rows of size %d into " SSZFMT 
								" rows/piece %d threads %d pieces"
								" fixed parts %d fixed size %d\n",
				 getVarConstant(mb, getArg(target, 2)).val.sval,
				 getVarConstant(mb, getArg(target, 3)).val.sval,
				 rowcnt, row_size, r, threads, pieces, mito_parts, mito_size);
	if (pieces <= 1)
		return 0;

	limit = mb->stop;
	slimit = mb->ssize;
	if (newMalBlkStmt(mb, mb->stop + 2 * estimate) < 0)
		return 0;
	estimate = 0;

	schema = getVarConstant(mb, getArg(target, 2)).val.sval;
	table = getVarConstant(mb, getArg(target, 3)).val.sval;
	mt = getVarMergeTableId(mb, getArg(target, 0));
	for (i = 0; i < limit; i++) {
		int upd = 0, qtpe, rtpe = 0, qv, rv;
		InstrPtr matq, matr = NULL;
		p = old[i];

		if (getModuleId(p) != sqlRef ||
			!(getFunctionId(p) == bindRef ||
			  getFunctionId(p) == bindidxRef ||
			  getFunctionId(p) == tidRef)) {
			pushInstruction(mb, p);
			continue;
		}
		/* don't split insert BATs */
		if (p->argc == 6 && getVarConstant(mb, getArg(p, 5)).val.ival == 1) {
			pushInstruction(mb, p);
			continue;
		}
		/* Don't split the (index) bat if we already have identified a range */
		/* This will happen if we inline separately optimized routines */
		if (p->argc > 7) {
			pushInstruction(mb, p);
			continue;
		}
		if (p->retc == 2)
			upd = 1;
		if (mt < 0 && (strcmp(schema, getVarConstant(mb, getArg(p, 2 + upd)).val.sval) ||
			       strcmp(table, getVarConstant(mb, getArg(p, 3 + upd)).val.sval))) {
			pushInstruction(mb, p);
			continue;
		}
		if (mt >= 0 && getVarMergeTableId(mb, getArg(p, 0)) != mt) {
			pushInstruction(mb, p);
			continue;
		}
		/* we keep the original bind operation, because it allows for
		 * easy undo when the mergtable can not do something */
		pushInstruction(mb, p);

		qtpe = getVarType(mb, getArg(p, 0));

		matq = newInstruction(NULL, ASSIGNsymbol);
		setModuleId(matq, matRef);
		setFunctionId(matq, newRef);
		getArg(matq, 0) = getArg(p, 0);

		if (upd) {
			matr = newInstruction(NULL, ASSIGNsymbol);
			setModuleId(matr, matRef);
			setFunctionId(matr, newRef);
			getArg(matr, 0) = getArg(p, 1);
			rtpe = getVarType(mb, getArg(p, 1));
		}

		for (j = 0; j < pieces; j++) {
			q = copyInstruction(p);
			q = pushInt(mb, q, j);
			q = pushInt(mb, q, pieces);

			qv = getArg(q, 0) = newTmpVariable(mb, qtpe);
			setVarUDFtype(mb, qv);
			setVarUsed(mb, qv);
			if (upd) {
				rv = getArg(q, 1) = newTmpVariable(mb, rtpe);
				setVarUDFtype(mb, rv);
				setVarUsed(mb, rv);
			}
			pushInstruction(mb, q);
			matq = pushArgument(mb, matq, qv);
			if (upd)
				matr = pushArgument(mb, matr, rv);
		}
		pushInstruction(mb, matq);
		if (upd)
			pushInstruction(mb, matr);
	}
	for (; i<limit; i++) 
		if (old[i])
			pushInstruction(mb,old[i]);
	for (; i<slimit; i++) 
		if (old[i])
			freeInstruction(old[i]);
	GDKfree(old);
	return 1;
}
