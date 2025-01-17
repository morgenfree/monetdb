/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "opt_multiplex.h"
#include "manifold.h"
#include "mal_interpreter.h"

/*
 * The generic solution to the multiplex operators is to translate
 * them to a MAL loop.
 * The call optimizer.multiplex(MOD,FCN,A1,...An) introduces the following code
 * structure:
 *
 * 	resB:= bat.new(A1);
 * barrier (h,t1):= iterator.new(A1);
 * 	t2:= algebra.fetch(A2,h)
 * 	...
 * 	cr:= MOD.FCN(t1,...,tn);
 * 	bat.append(resB,cr);
 * 	redo (h,t):= iterator.next(A1);
 * end h;
 *
 * The algorithm consists of two phases: phase one deals with
 * collecting the relevant information, phase two is the actual
 * code construction.
 */
static str
OPTexpandMultiplex(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	int i = 2, iter = 0;
	int hvar, tvar;
	str mod, fcn;
	int *alias, *resB;
	InstrPtr q;
	int ht, tt;
	int bat = (getModuleId(pci) == batmalRef) ;

	(void) cntxt;
	(void) stk;
	for (i = 0; i < pci->retc; i++) {
		ht = getHeadType(getArgType(mb, pci, i));
		if (ht != TYPE_oid)
			throw(MAL, "optimizer.multiplex", "Target head type is missing");
		tt = getColumnType(getArgType(mb, pci, i));
		if (tt== TYPE_any)
			throw(MAL, "optimizer.multiplex", "Target tail type is missing");
		if (isAnyExpression(getArgType(mb, pci, i)))
			throw(MAL, "optimizer.multiplex", "Target type is missing");
	}

	mod = VALget(&getVar(mb, getArg(pci, pci->retc))->value);
	mod = putName(mod,strlen(mod));
	fcn = VALget(&getVar(mb, getArg(pci, pci->retc+1))->value);
	fcn = putName(fcn,strlen(fcn));
#ifndef NDEBUG
	mnstr_printf(GDKstdout,"#WARNING To speedup %s.%s a bulk operator implementation is needed\n#", mod,fcn);
	printInstruction(GDKstdout, mb, stk, pci, LIST_MAL_DEBUG);
#endif

	/* search the iterator bat */
	for (i = pci->retc+2; i < pci->argc; i++)
		if (isaBatType(getArgType(mb, pci, i))) {
			iter = getArg(pci, i);
			if (getHeadType(getVarType(mb,iter)) != TYPE_oid)
				throw(MAL, "optimizer.multiplex", "Iterator BAT is not OID-headed");
			break;
		}
	if( i == pci->argc)
		throw(MAL, "optimizer.multiplex", "Iterator BAT type is missing");

	OPTDEBUGmultiplex {
		char *tpenme;
		mnstr_printf(cntxt->fdout,"#calling the optimize multiplex script routine\n");
		printFunction(cntxt->fdout,mb, 0, LIST_MAL_ALL );
		tpenme = getTypeName(getVarType(mb,iter));
		mnstr_printf(cntxt->fdout,"#multiplex against operator %d %s\n",iter, tpenme);
		GDKfree(tpenme);
		printInstruction(cntxt->fdout,mb, 0, pci,LIST_MAL_ALL);
	}
	/*
	 * Beware, the operator constant (arg=1) is passed along as well,
	 * because in the end we issue a recursive function call that should
	 * find the actual arguments at the proper place of the callee.
	 */

	alias= (int*) GDKmalloc(sizeof(int) * pci->maxarg);
	resB = (int*) GDKmalloc(sizeof(int) * pci->retc);
	if (alias == NULL || resB == NULL)  {
		GDKfree(alias);
		GDKfree(resB);
		return NULL;
	}

	/* resB := new(refBat) */
	for (i = 0; i < pci->retc; i++) {
		q = newFcnCall(mb, batRef, newRef);
		resB[i] = getArg(q, 0);

		ht = getHeadType(getArgType(mb, pci, i));
		tt = getColumnType(getArgType(mb, pci, i));

		setVarType(mb, getArg(q, 0), newBatType(ht, tt));
		q = pushType(mb, q, ht);
		q = pushType(mb, q, tt);
	}

	/* barrier (h,r) := iterator.new(refBat); */
	q = newFcnCall(mb, iteratorRef, newRef);
	q->barrier = BARRIERsymbol;
	hvar = newTmpVariable(mb, TYPE_any);
	getArg(q,0) = hvar;
	tvar = newTmpVariable(mb, TYPE_any);
	q= pushReturn(mb, q, tvar);
	(void) pushArgument(mb,q,iter);

	/* $1:= algebra.fetch(Ai,h) or constant */
	for (i = pci->retc+2; i < pci->argc; i++) {
		if (getArg(pci, i) != iter && isaBatType(getArgType(mb, pci, i))) {
			q = newFcnCall(mb, algebraRef, "fetch");
			alias[i] = newTmpVariable(mb, getColumnType(getArgType(mb, pci, i)));
			getArg(q, 0) = alias[i];
			q= pushArgument(mb, q, getArg(pci, i));
			(void) pushArgument(mb, q, hvar);
		}
	}

	/* cr:= mod.CMD($1,...,$n); */
	q = newFcnCall(mb, mod, fcn);
	for (i = 0; i < pci->retc; i++) {
		int nvar = 0;
		if (bat) {
			ht = getHeadType(getArgType(mb, pci, i));
			tt = getColumnType(getArgType(mb, pci, i));
			nvar = newTmpVariable(mb, newBatType(ht, tt));
		} else {
			nvar = newTmpVariable(mb, TYPE_any);
		}
		if (i)
			q = pushReturn(mb, q, nvar);
		else
			getArg(q, 0) = nvar;
	}

	for (i = pci->retc+2; i < pci->argc; i++) {
		if (getArg(pci, i) == iter) {
			q = pushArgument(mb, q, tvar);
		} else if (isaBatType(getArgType(mb, pci, i))) {
			q = pushArgument(mb, q, alias[i]);
		} else {
			q = pushArgument(mb, q, getArg(pci, i));
		}
	}

	for (i = 0; i < pci->retc; i++) {
		InstrPtr a = newFcnCall(mb, batRef, appendRef);
		a = pushArgument(mb, a, resB[i]);
		(void) pushArgument(mb, a, getArg(q,i));
	}

/* redo (h,r):= iterator.next(refBat); */
	q = newFcnCall(mb, iteratorRef, nextRef);
	q->barrier = REDOsymbol;
	getArg(q,0) = hvar;
	q= pushReturn(mb, q, tvar);
	(void) pushArgument(mb,q,iter);

	q = newAssignment(mb);
	q->barrier = EXITsymbol;
	getArg(q,0) = hvar;
	(void) pushReturn(mb, q, tvar);

	for (i = 0; i < pci->retc; i++) {
		q = newAssignment(mb);
		getArg(q, 0) = getArg(pci, i);
		(void) pushArgument(mb, q, resB[i]);
	}
	GDKfree(alias);
	GDKfree(resB);
	return MAL_SUCCEED;
}

/*
 * The multiplexSimple is called by the MAL scenario. It bypasses
 * the optimizer infrastructure, to avoid excessive space allocation
 * and interpretation overhead.
 */
str
OPTmultiplexSimple(Client cntxt)
{
	MalBlkPtr mb= cntxt->curprg->def;
	int i, doit=0;
	InstrPtr p;
	if(mb)
	for( i=0; i<mb->stop; i++){
		p= getInstrPtr(mb,i);
		if(isMultiplex(p))
			doit++;
	}
	if( doit) {
		OPTmultiplexImplementation(cntxt, mb, 0, 0);
		chkTypes(cntxt->fdout, cntxt->nspace, mb,TRUE);
		if ( mb->errors == 0) {
			chkFlow(cntxt->fdout, mb);
			chkDeclarations(cntxt->fdout,mb);
		}
	}
	return 0;
}
int
OPTmultiplexImplementation(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	InstrPtr *old, p;
	int i, limit, slimit, actions= 0;
	str msg= MAL_SUCCEED;

	(void) stk;
	(void) pci;

	old = mb->stmt;
	limit = mb->stop;
	slimit = mb->ssize;
	if ( newMalBlkStmt(mb, mb->ssize) < 0 )
		return 0;

	for (i = 0; i < limit; i++) {
		p = old[i];
		if (msg == MAL_SUCCEED && isMultiplex(p)) { 
			if ( MANIFOLDtypecheck(cntxt,mb,p) != NULL){
				setFunctionId(p, manifoldRef);
				pushInstruction(mb, p);
				actions++;
				continue;
			}
			msg = OPTexpandMultiplex(cntxt, mb, stk, p);
			if( msg== MAL_SUCCEED){
				freeInstruction(p);
				old[i]=0;
				actions++;
				continue;
			} 

			pushInstruction(mb, p);
			actions++;
		} else if( old[i])
			pushInstruction(mb, p);
	}
	for(;i<slimit; i++)
		if( old[i])
			freeInstruction(old[i]);
	GDKfree(old);
	if (mb->errors){
		/* rollback */
	}
	GDKfree(msg);
	return mb->errors? 0: actions;
}
