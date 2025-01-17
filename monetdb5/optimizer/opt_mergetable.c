/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "opt_mergetable.h"

typedef enum mat_type_t {
	mat_none = 0,	/* Simple mat aligned operations (ie batcalc etc) */
	mat_grp = 1,	/* result of phase one of a mat - group.new/derive */
	mat_ext = 2,	/* mat_grp extend */
	mat_cnt = 3,	/* mat_grp count */
	mat_tpn = 4,	/* Phase one of topn on a mat */
	mat_slc = 5,	/* Last phase of topn (or just slice) on a mat */
	mat_rdr = 6	/* Phase one of sorting, ie sorted the parts sofar */
} mat_type_t;

typedef struct mat {
	InstrPtr mi;		/* mat instruction */
	InstrPtr org;		/* orignal instruction */
	int mv;			/* mat variable */
	int im;			/* input mat, for attribute of sub relations */
	int pm;			/* parent mat, for sub relations */
	mat_type_t type;	/* type of operation */
	int packed;
	int pushed;		 
} mat_t;

typedef struct matlist {
	mat_t *v;
	int top;
	int size;
} matlist_t;

static mat_type_t
mat_type( mat_t *mat, int n) 
{
	mat_type_t type = mat_none;
	(void)mat;
	(void)n;
	return type;
}

static int
is_a_mat(int idx, matlist_t *ml){
	int i;
	for(i =0; i<ml->top; i++)
		if (ml->v[i].mv == idx) 
			return i;
	return -1;
}

static int
nr_of_mats(InstrPtr p, matlist_t *ml)
{
	int j,cnt=0;
	for(j=p->retc; j<p->argc; j++)
		if (is_a_mat(getArg(p,j), ml) >= 0) 
			cnt++;
	return cnt;
}

static int
nr_of_bats(MalBlkPtr mb, InstrPtr p)
{
	int j,cnt=0;
	for(j=p->retc; j<p->argc; j++)
		if (isaBatType(getArgType(mb,p,j))) 
			cnt++;
	return cnt;
}

/* some mat's have intermediates (with intermediate result variables), therefor
 * we pass the old output mat variable */
inline static void
mat_add_var(matlist_t *ml, InstrPtr q, InstrPtr p, int var, mat_type_t type, int inputmat, int parentmat) 
{
	mat_t *dst = &ml->v[ml->top];
	if (ml->top == ml->size) {
		int s = ml->size * 2;
		mat_t *v = (mat_t*)GDKzalloc(s * sizeof(mat_t));
		if (!v)
			return;	   /* FIXME: error checking */
		memcpy(v, ml->v, ml->top * sizeof(mat_t));
		GDKfree(ml->v);
		ml->size = s;
		ml->v = v;
		dst = &ml->v[ml->top];
	}
	dst->mi = q;
	dst->org = p;
	dst->mv = var;
	dst->type = type;
	dst->im = inputmat;
	dst->pm = parentmat;
	dst->packed = 0;
	dst->pushed = 1;
	++ml->top;
}

inline static void
mat_add(matlist_t *ml, InstrPtr q, mat_type_t type, char *func) 
{
	mat_add_var(ml, q, NULL, getArg(q,0), type, -1, -1);
	ml->v[ml->top-1].pushed = 0;
	(void)func;
	//printf (" ml.top %d %s\n", ml.top, func);
}

static void 
mat_pack(MalBlkPtr mb, mat_t *mat, int m)
{
	InstrPtr r;

	if (mat[m].packed)
		return ;

	if((mat[m].mi->argc-mat[m].mi->retc) == 1){
		/* simple assignment is sufficient */
		r = newInstruction(mb, ASSIGNsymbol);
		getArg(r,0) = getArg(mat[m].mi,0);
		getArg(r,1) = getArg(mat[m].mi,1);
		r->retc = 1;
		r->argc = 2;
	} else {
		int l;

		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r, matRef);
		setFunctionId(r, packRef);
		getArg(r,0) = getArg(mat[m].mi, 0);
		for(l=mat[m].mi->retc; l< mat[m].mi->argc; l++)
			r= pushArgument(mb,r, getArg(mat[m].mi,l));
	}
	mat[m].packed = 1;
	pushInstruction(mb, r);
}

static void
setPartnr(MalBlkPtr mb, int ivar, int ovar, int pnr)
{
	int tpnr = -1;
	VarPtr partnr = (ivar >= 0)?varGetProp(mb, ivar, toriginProp):NULL;
	ValRecord val;

	if (partnr) {
		varSetProp(mb, ovar, toriginProp, op_eq, &partnr->value);
		tpnr = partnr->value.val.ival;
	}
	val.val.ival = pnr;
	val.vtype = TYPE_int;
	varSetProp(mb, ovar, horiginProp, op_eq, &val);
	(void)tpnr;
	//printf("%d %d ", pnr, tpnr);
}

static void
propagatePartnr(MalBlkPtr mb, int ivar, int ovar, int pnr)
{
	/* prop head ids to tail */
	int tpnr = -1;
	VarPtr partnr = varGetProp(mb, ivar, horiginProp);
	ValRecord val;

	val.val.ival = pnr;
	val.vtype = TYPE_int;
	if (partnr) {
		varSetProp(mb, ovar, toriginProp, op_eq, &partnr->value);
		tpnr = partnr->value.val.ival;
	} 
	varSetProp(mb, ovar, horiginProp, op_eq, &val);
	(void)tpnr;
	//printf("%d %d ", pnr, tpnr);
}

static void
propagateMirror(MalBlkPtr mb, int ivar, int ovar)
{
	/* prop head ids to head and tail */
	VarPtr partnr = varGetProp(mb, ivar, horiginProp);

	if (partnr) {
		varSetProp(mb, ovar, toriginProp, op_eq, &partnr->value);
		varSetProp(mb, ovar, horiginProp, op_eq, &partnr->value);
	} 
}

static int 
overlap( MalBlkPtr mb, int lv, int rv, int lnr, int rnr, int ontails)
{
	VarPtr lpartnr = varGetProp(mb, lv, toriginProp); 
	VarPtr rpartnr = varGetProp(mb, rv, (ontails)?toriginProp:horiginProp); 

	if (!lpartnr && !rpartnr)
		return lnr == rnr;
	if (!rpartnr) 
		return lpartnr->value.val.ival == rnr; 
	if (!lpartnr)
		return rpartnr->value.val.ival == lnr; 
	return lpartnr->value.val.ival == rpartnr->value.val.ival; 
}

static void
mat_set_prop( MalBlkPtr mb, InstrPtr p)
{
	int k, tpe = getArgType(mb, p, 0);

	tpe = getColumnType(tpe);
	for(k=1; k < p->argc; k++) {
		setPartnr(mb, -1, getArg(p,k), k);
		if (tpe == TYPE_oid)
			propagateMirror(mb, getArg(p,k), getArg(p,k));
	}
}

static InstrPtr
mat_delta(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int n, int o, int e, int mvar, int nvar, int ovar, int evar)
{
	int tpe, k, j, is_subdelta = (getFunctionId(p) == subdeltaRef);
	InstrPtr r = NULL;

	//printf("# %s.%s(%d,%d,%d,%d)", getModuleId(p), getFunctionId(p), m, n, o, e);

	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r, 0) = getArg(p,0);
	tpe = getArgType(mb,p,0);

	/* Handle like mat_leftfetchjoin, ie overlapping partitions */
	if (evar == 1 && mat[e].mi->argc != mat[m].mi->argc) {
		int nr = 1;
		for(k=1; k < mat[e].mi->argc; k++) {
			for(j=1; j < mat[m].mi->argc; j++) {
				if (overlap(mb, getArg(mat[e].mi, k), getArg(mat[m].mi, j), k, j, 0)){
					InstrPtr q = copyInstruction(p);

					/* remove last argument */
					if (k < mat[m].mi->argc-1)
						q->argc--;
					/* make sure to resolve again */
					q->token = ASSIGNsymbol; 
					q->typechk = TYPE_UNKNOWN;
        				q->fcn = NULL;
        				q->blk = NULL;

					getArg(q, 0) = newTmpVariable(mb, tpe);
					getArg(q, mvar) = getArg(mat[m].mi, j);
					getArg(q, nvar) = getArg(mat[n].mi, j);
					getArg(q, ovar) = getArg(mat[o].mi, j);
					getArg(q, evar) = getArg(mat[e].mi, k);
					pushInstruction(mb, q);
					setPartnr(mb, getArg(mat[m].mi, j), getArg(q,0), nr);
					r = pushArgument(mb, r, getArg(q, 0));

					nr++;
					break;
				}
			}
		}
	} else {
		for(k=1; k < mat[m].mi->argc; k++) {
			InstrPtr q = copyInstruction(p);

			/* remove last argument */
			if (k < mat[m].mi->argc-1)
				q->argc--;
			/* make sure to resolve again */
			q->token = ASSIGNsymbol; 
			q->typechk = TYPE_UNKNOWN;
        		q->fcn = NULL;
        		q->blk = NULL;

			getArg(q, 0) = newTmpVariable(mb, tpe);
			getArg(q, mvar) = getArg(mat[m].mi, k);
			getArg(q, nvar) = getArg(mat[n].mi, k);
			getArg(q, ovar) = getArg(mat[o].mi, k);
			if (e >= 0)
				getArg(q, evar) = getArg(mat[e].mi, k);
			pushInstruction(mb, q);
			setPartnr(mb, is_subdelta?getArg(mat[m].mi, k):-1, getArg(q,0), k);
			r = pushArgument(mb, r, getArg(q, 0));
		}
	}
	return r;
}


static InstrPtr
mat_apply1(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int var)
{
	int tpe, k, is_select = isSubSelect(p), is_mirror = (getFunctionId(p) == mirrorRef);
	int is_identity = (getFunctionId(p) == identityRef && getModuleId(p) == batcalcRef);
	int ident_var = 0, is_assign = (getFunctionId(p) == NULL), n = 0;
	InstrPtr r = NULL, q;
	mat_t *mat = ml->v;

	/* Find the mat we overwrite */
	if (is_assign) {
		n = is_a_mat(getArg(p, 0), ml);
		is_assign = (n >= 0);
	}

	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r, 0) = getArg(p,0);
	tpe = getArgType(mb,p,0);

	if (is_identity) {
		q = newInstruction(mb, ASSIGNsymbol);
		getArg(q, 0) = newTmpVariable(mb, TYPE_oid);
		q->retc = 1;
		q->argc = 1;
		q = pushOid(mb, q, 0);
		ident_var = getArg(q, 0);
		pushInstruction(mb, q);
	}
	for(k=1; k < mat[m].mi->argc; k++) {
		q = copyInstruction(p);

		if (is_assign)
			getArg(q, 0) = getArg(mat[n].mi, k);
		else
			getArg(q, 0) = newTmpVariable(mb, tpe);
		if (is_identity)
			getArg(q, 1) = newTmpVariable(mb, TYPE_oid);
		getArg(q, var+is_identity) = getArg(mat[m].mi, k);
		if (is_identity) {
			getArg(q, 3) = ident_var;
			q->retc = 2;
			q->argc = 4;
			/* make sure to resolve again */
			q->token = ASSIGNsymbol; 
			q->typechk = TYPE_UNKNOWN;
        		q->fcn = NULL;
        		q->blk = NULL;
		}
		ident_var = getArg(q, 1);
		pushInstruction(mb, q);
		if (is_mirror || is_identity) {
			propagateMirror(mb, getArg(mat[m].mi, k), getArg(q,0));
		} else if (is_select)
			propagatePartnr(mb, getArg(mat[m].mi, k), getArg(q,0), k);
		else
			setPartnr(mb, -1, getArg(q,0), k);
		r = pushArgument(mb, r, getArg(q, 0));
	}
	return r;
}

static InstrPtr
mat_apply2(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int n, int mvar, int nvar)
{
	int tpe, k, is_select = isSubSelect(p);
	InstrPtr r = NULL;

	//printf("# %s.%s(%d,%d)", getModuleId(p), getFunctionId(p), m, n);

	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r, 0) = getArg(p,0);
	tpe = getArgType(mb,p,0);

	for(k=1; k < mat[m].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);

		getArg(q, 0) = newTmpVariable(mb, tpe);
		getArg(q, mvar) = getArg(mat[m].mi, k);
		getArg(q, nvar) = getArg(mat[n].mi, k);
		pushInstruction(mb, q);
		if (is_select)
			setPartnr(mb, getArg(q,2), getArg(q,0), k);
		else
			setPartnr(mb, -1, getArg(q,0), k);
		r = pushArgument(mb, r, getArg(q, 0));
	}
	return r;
}

static InstrPtr
mat_apply3(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int n, int o, int mvar, int nvar, int ovar)
{
	int tpe, k;
	InstrPtr r = NULL;

	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r, 0) = getArg(p,0);
	tpe = getArgType(mb,p,0);

	//printf("# %s.%s(%d,%d,%d)", getModuleId(p), getFunctionId(p), m, n, o);

	for(k=1; k < mat[m].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);

		getArg(q, 0) = newTmpVariable(mb, tpe);
		getArg(q, mvar) = getArg(mat[m].mi, k);
		getArg(q, nvar) = getArg(mat[n].mi, k);
		getArg(q, ovar) = getArg(mat[o].mi, k);
		pushInstruction(mb, q);
		setPartnr(mb, -1, getArg(q,0), k);
		r = pushArgument(mb, r, getArg(q, 0));
	}
	return r;
}


static void
mat_setop(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int n)
{
	int tpe = getArgType(mb,p, 0), k, j;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);
	mat_t *mat = ml->v;

	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r,0) = getArg(p,0);
	
	//printf("# %s.%s(%d,%d)", getModuleId(p), getFunctionId(p), m, n);
	assert(m>=0 || n>=0);
	if (m >= 0 && n >= 0) {
		int nr = 1;
		for(k=1; k<mat[m].mi->argc; k++) { 
			InstrPtr q = copyInstruction(p);
			InstrPtr s = newInstruction(mb, ASSIGNsymbol);

			setModuleId(s,matRef);
			setFunctionId(s,packRef);
			getArg(s,0) = newTmpVariable(mb, tpe);
	
			for (j=1; j<mat[n].mi->argc; j++) {
				if (overlap(mb, getArg(mat[m].mi, k), getArg(mat[n].mi, j), -1, -2, 1)){
					s = pushArgument(mb,s,getArg(mat[n].mi,j));
				}
			}
			pushInstruction(mb,s);

			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = getArg(mat[m].mi,k);
			getArg(q,2) = getArg(s,0);
			setPartnr(mb, getArg(mat[m].mi,k), getArg(q,0), nr);
			pushInstruction(mb,q);

			r = pushArgument(mb,r,getArg(q,0));
			nr++;
		}
	} else {
		assert(m >= 0);
		for(k=1; k<mat[m].mi->argc; k++) {
			InstrPtr q = copyInstruction(p);

			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = getArg(mat[m].mi, k);
			pushInstruction(mb,q);

			setPartnr(mb, getArg(q, 2), getArg(q,0), k);
			r = pushArgument(mb, r, getArg(q,0));
		}
	}

	mat_add(ml, r, mat_none, getFunctionId(p));
}

static void
mat_leftfetchjoin(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int n)
{
	int tpe = getArgType(mb,p, 0), k, j;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);
	mat_t *mat = ml->v;

	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r,0) = getArg(p,0);
	
	//printf("# %s.%s(%d,%d)", getModuleId(p), getFunctionId(p), m, n);
	assert(m>=0 || n>=0);
	if (m >= 0 && n >= 0) {
		int nr = 1;
		for(k=1; k<mat[m].mi->argc; k++) { 
			for (j=1; j<mat[n].mi->argc; j++) {
				if (overlap(mb, getArg(mat[m].mi, k), getArg(mat[n].mi, j), k, j, 0)){
					InstrPtr q = copyInstruction(p);

					getArg(q,0) = newTmpVariable(mb, tpe);
					getArg(q,1) = getArg(mat[m].mi,k);
					getArg(q,2) = getArg(mat[n].mi,j);
					pushInstruction(mb,q);
		
					setPartnr(mb, getArg(mat[n].mi, j), getArg(q,0), nr);
					r = pushArgument(mb,r,getArg(q,0));

					nr++;
					break;
				}
			}
		}
	} else {
		assert(m >= 0);
		for(k=1; k<mat[m].mi->argc; k++) {
			InstrPtr q = copyInstruction(p);

			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = getArg(mat[m].mi, k);
			pushInstruction(mb,q);

			setPartnr(mb, getArg(q, 2), getArg(q,0), k);
			r = pushArgument(mb, r, getArg(q,0));
		}
	}

	mat_add(ml, r, mat_none, getFunctionId(p));
}

static void
mat_join2(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int n)
{
	int tpe = getArgType(mb,p, 0), j,k, nr = 1;
	InstrPtr l = newInstruction(mb, ASSIGNsymbol);
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);
	mat_t *mat = ml->v;

	setModuleId(l,matRef);
	setFunctionId(l,packRef);
	getArg(l,0) = getArg(p,0);

	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r,0) = getArg(p,1);

	//printf("# %s.%s(%d,%d)", getModuleId(p), getFunctionId(p), m, n);
	
	assert(m>=0 || n>=0);
	if (m >= 0 && n >= 0) {
		for(k=1; k<mat[m].mi->argc; k++) {
			for (j=1; j<mat[n].mi->argc; j++) {
				InstrPtr q = copyInstruction(p);

				getArg(q,0) = newTmpVariable(mb, tpe);
				getArg(q,1) = newTmpVariable(mb, tpe);
				getArg(q,2) = getArg(mat[m].mi,k);
				getArg(q,3) = getArg(mat[n].mi,j);
				pushInstruction(mb,q);
	
				propagatePartnr(mb, getArg(mat[m].mi, k), getArg(q,0), nr);
				propagatePartnr(mb, getArg(mat[n].mi, j), getArg(q,1), nr);

				/* add result to mat */
				l = pushArgument(mb,l,getArg(q,0));
				r = pushArgument(mb,r,getArg(q,1));
				nr++;
			}
		}
	} else {
		int mv = (m>=0)?m:n;
		int av = (m<0);
		int bv = (m>=0);

		for(k=1; k<mat[mv].mi->argc; k++) {
			InstrPtr q = copyInstruction(p);

			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = newTmpVariable(mb, tpe);
			getArg(q,p->retc+av) = getArg(mat[mv].mi, k);
			pushInstruction(mb,q);

			propagatePartnr(mb, getArg(mat[mv].mi, k), getArg(q,av), k);
			propagatePartnr(mb, getArg(p, p->retc+bv), getArg(q,bv), k);

			/* add result to mat */
			l = pushArgument(mb, l, getArg(q,0));
			r = pushArgument(mb, r, getArg(q,1));
		}
	}
	mat_add(ml, l, mat_none, getFunctionId(p));
	mat_add(ml, r, mat_none, getFunctionId(p));
}

static void
mat_join3(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int n, int o)
{
	int tpe = getArgType(mb,p, 0), j,k, nr = 1;
	InstrPtr l = newInstruction(mb, ASSIGNsymbol);
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);
	mat_t *mat = ml->v;

	setModuleId(l,matRef);
	setFunctionId(l,packRef);
	getArg(l,0) = getArg(p,0);

	setModuleId(r,matRef);
	setFunctionId(r,packRef);
	getArg(r,0) = getArg(p,1);

	//printf("# %s.%s(%d,%d)", getModuleId(p), getFunctionId(p), m, n);
	
	assert(m>=0 || n>=0);
	if (m >= 0 && n >= 0 && o >= 0) {
		assert(mat[n].mi->argc == mat[o].mi->argc);
		for(k=1; k<mat[m].mi->argc; k++) {
			for (j=1; j<mat[n].mi->argc; j++) {
				InstrPtr q = copyInstruction(p);

				getArg(q,0) = newTmpVariable(mb, tpe);
				getArg(q,1) = newTmpVariable(mb, tpe);
				getArg(q,2) = getArg(mat[m].mi,k);
				getArg(q,3) = getArg(mat[n].mi,j);
				getArg(q,4) = getArg(mat[o].mi,j);
				pushInstruction(mb,q);
	
				propagatePartnr(mb, getArg(mat[m].mi, k), getArg(q,0), nr);
				propagatePartnr(mb, getArg(mat[n].mi, j), getArg(q,1), nr);

				/* add result to mat */
				l = pushArgument(mb,l,getArg(q,0));
				r = pushArgument(mb,r,getArg(q,1));
				nr++;
			}
		}
	} else {
		int mv = (m>=0)?m:n;
		int av = (m<0);
		int bv = (m>=0);

		for(k=1; k<mat[mv].mi->argc; k++) {
			InstrPtr q = copyInstruction(p);

			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = newTmpVariable(mb, tpe);
			getArg(q,p->retc+av) = getArg(mat[mv].mi, k);
			if (o >= 0)
				getArg(q,p->retc+2) = getArg(mat[o].mi, k);
			pushInstruction(mb,q);

			propagatePartnr(mb, getArg(mat[mv].mi, k), getArg(q,av), k);
			propagatePartnr(mb, getArg(p, p->retc+bv), getArg(q,bv), k);

			/* add result to mat */
			l = pushArgument(mb, l, getArg(q,0));
			r = pushArgument(mb, r, getArg(q,1));
		}
	}
	mat_add(ml, l, mat_none, getFunctionId(p));
	mat_add(ml, r, mat_none, getFunctionId(p));
}


static char *
aggr_phase2(char *aggr)
{
	if (aggr == countRef || aggr == count_no_nilRef || aggr == avgRef)
		return sumRef;
	if (aggr == subcountRef || aggr == subavgRef)
		return subsumRef;
	/* min/max/sum/prod and unique are fine */
	return aggr;
}

static void
mat_aggr(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m)
{
	int tp = getArgType(mb,p,0), k, tp2 = TYPE_lng;
	int battp = (getModuleId(p)==aggrRef)?newBatType(TYPE_oid,tp):tp, battp2 = 0;
	int isAvg = (getFunctionId(p) == avgRef);
	InstrPtr r = NULL, s = NULL, q = NULL, u = NULL;

	/* we pack the partitial result */
	r = newInstruction(mb,ASSIGNsymbol);
	setModuleId(r, matRef);
	setFunctionId(r, packRef);
	getArg(r,0) = newTmpVariable(mb, battp);

	if (isAvg) { /* counts */
		battp2 = newBatType(TYPE_oid, tp2);
		u = newInstruction(mb, ASSIGNsymbol);
		setModuleId(u,matRef);
		setFunctionId(u,packRef);
		getArg(u,0) = newTmpVariable(mb, battp2);
	}
	for(k=1; k< mat[m].mi->argc; k++) {
		q = newInstruction(mb,ASSIGNsymbol);
		setModuleId(q,getModuleId(p));
		if (isAvg)
			setModuleId(q,batcalcRef);
		setFunctionId(q,getFunctionId(p));
		getArg(q,0) = newTmpVariable(mb, tp);
		if (isAvg) 
			q = pushReturn(mb, q, newTmpVariable(mb, tp2));
		q = pushArgument(mb,q,getArg(mat[m].mi,k));
		pushInstruction(mb,q);
		
		r = pushArgument(mb,r,getArg(q,0));
		if (isAvg) 
			u = pushArgument(mb,u,getArg(q,1));
	}
	pushInstruction(mb,r);
	if (isAvg)
		pushInstruction(mb, u);

	/* Filter empty partitions */
	if (getModuleId(p) == aggrRef && !isAvg) {
		s = newInstruction(mb,ASSIGNsymbol);
		setModuleId(s, algebraRef);
		setFunctionId(s, selectNotNilRef);
		getArg(s,0) = newTmpVariable(mb, battp);
		s = pushArgument(mb, s, getArg(r,0));
		pushInstruction(mb, s);
		r = s;
	}

	/* for avg we do sum (avg*(count/sumcount) ) */
	if (isAvg) {
		InstrPtr v,w,x,y,cond;

		/* lng w = sum counts */
 		w = newInstruction(mb, ASSIGNsymbol);
		setModuleId(w, aggrRef);
		setFunctionId(w, sumRef);
		getArg(w,0) = newTmpVariable(mb, tp2);
		w = pushArgument(mb, w, getArg(u, 0));
		pushInstruction(mb, w);

		/*  y=count = ifthenelse(w=count==0,NULL,w=count)  */
		cond = newInstruction(mb, ASSIGNsymbol);
		setModuleId(cond, calcRef);
		setFunctionId(cond, eqRef); 
		getArg(cond,0) = newTmpVariable(mb, TYPE_bit);
		cond = pushArgument(mb, cond, getArg(w, 0));
		cond = pushWrd(mb, cond, 0);
		pushInstruction(mb,cond);

		y = newInstruction(mb, ASSIGNsymbol);
		setModuleId(y, calcRef);
		setFunctionId(y, ifthenelseRef); 
		getArg(y,0) = newTmpVariable(mb, tp2);
		y = pushArgument(mb, y, getArg(cond, 0));
		y = pushNil(mb, y, tp2);
		y = pushArgument(mb, y, getArg(w, 0));
		pushInstruction(mb,y);

		/* dbl v = double(count) */
		v = newInstruction(mb, ASSIGNsymbol);
		setModuleId(v, batcalcRef);
		setFunctionId(v, dblRef); 
		getArg(v,0) = newTmpVariable(mb, newBatType(TYPE_oid, TYPE_dbl));
		v = pushArgument(mb, v, getArg(u, 0));
		pushInstruction(mb, v);

		/* dbl x = v / y */
		x = newInstruction(mb, ASSIGNsymbol);
		setModuleId(x, batcalcRef);
		setFunctionId(x, divRef); 
		getArg(x,0) = newTmpVariable(mb, newBatType(TYPE_oid, TYPE_dbl));
		x = pushArgument(mb, x, getArg(v, 0));
		x = pushArgument(mb, x, getArg(y, 0));
		pushInstruction(mb, x);

		/* dbl w = avg * x */
		w = newInstruction(mb, ASSIGNsymbol);
		setModuleId(w, batcalcRef);
		setFunctionId(w, mulRef); 
		getArg(w,0) = newTmpVariable(mb, battp);
		w = pushArgument(mb, w, getArg(r, 0));
		w = pushArgument(mb, w, getArg(x, 0));
		pushInstruction(mb, w);

		r = w;

		/* filter nils */
		s = newInstruction(mb,ASSIGNsymbol);
		setModuleId(s, algebraRef);
		setFunctionId(s, selectNotNilRef);
		getArg(s,0) = newTmpVariable(mb, battp);
		s = pushArgument(mb, s, getArg(r,0));
		pushInstruction(mb, s);
		r = s;
	}

	s = newInstruction(mb,ASSIGNsymbol);
	setModuleId(s,getModuleId(p));
	setFunctionId(s, aggr_phase2(getFunctionId(p)));
	getArg(s,0) = getArg(p,0);
	s = pushArgument(mb, s, getArg(r,0));
	pushInstruction(mb, s);
}

static int
chain_by_length(mat_t *mat, int g)
{
	int cnt = 0;
	while(g >= 0) {
		g = mat[g].pm;
		cnt++;
	}
	return cnt;
}

static int
walk_n_back(mat_t *mat, int g, int cnt)
{
	while(cnt > 0){ 
		g = mat[g].pm;
		cnt--;
	}
	return g;
}

static int
group_by_ext(matlist_t *ml, int g)
{
	int i;

	for(i=g; i< ml->top; i++){ 
		if (ml->v[i].pm == g)
			return i;
	}
	return 0;
}

/* In some cases we have non groupby attribute columns, these require 
 * gext.leftfetchjoin(mat.pack(per partition ext.leftfetchjoins(x))) 
 */

static void
mat_group_project(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int e, int a)
{
	int tp = getArgType(mb,p,0), k;
	int tail = getColumnType(tp);
	InstrPtr ai1 = newInstruction(mb, ASSIGNsymbol), r;
	mat_t *mat = ml->v;

	setModuleId(ai1,matRef);
	setFunctionId(ai1,packRef);
	getArg(ai1,0) = newTmpVariable(mb, tp);

	assert(mat[e].mi->argc == mat[a].mi->argc);
	for(k=1; k<mat[a].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);

		getArg(q,0) = newTmpVariable(mb, tp);
		getArg(q,1) = getArg(mat[e].mi,k);
		getArg(q,2) = getArg(mat[a].mi,k);
		pushInstruction(mb,q);

		/* pack the result into a mat */
		ai1 = pushArgument(mb,ai1,getArg(q,0));
	}
	pushInstruction(mb, ai1);

	r = copyInstruction(p);
	getArg(r,1) = mat[e].mv;
	getArg(r,2) = getArg(ai1,0);
	pushInstruction(mb,r);
	if (tail == TYPE_oid)
		mat_add_var(ml, ai1, r, getArg(r, 0), mat_ext,  -1, -1);
}

/* Per partition aggregates are merged and aggregated together. For 
 * most (handled) aggregates thats relatively simple. AVG is somewhat
 * more complex. */
static void
mat_group_aggr(MalBlkPtr mb, InstrPtr p, mat_t *mat, int b, int g, int e)
{
	int tp = getArgType(mb,p,0), k, tp2 = 0;
	char *aggr2 = aggr_phase2(getFunctionId(p));
	int isAvg = (getFunctionId(p) == subavgRef);
	InstrPtr ai1 = newInstruction(mb, ASSIGNsymbol), ai10 = NULL, ai2;

	setModuleId(ai1,matRef);
	setFunctionId(ai1,packRef);
	getArg(ai1,0) = newTmpVariable(mb, tp);

	if (isAvg) { /* counts */
		tp2 = newBatType(TYPE_oid, TYPE_wrd);
		ai10 = newInstruction(mb, ASSIGNsymbol);
		setModuleId(ai10,matRef);
		setFunctionId(ai10,packRef);
		getArg(ai10,0) = newTmpVariable(mb, tp2);
	}

	for(k=1; k<mat[b].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);

		getArg(q,0) = newTmpVariable(mb, tp);
		if (isAvg) {
			getArg(q,1) = newTmpVariable(mb, tp2);
			q = pushArgument(mb, q, getArg(q,1)); /* push at end, create space */
			q->retc = 2;
			getArg(q,q->argc-1) = getArg(q,q->argc-2);
			getArg(q,q->argc-2) = getArg(q,q->argc-3);
		}
		getArg(q,1+isAvg) = getArg(mat[b].mi,k);
		getArg(q,2+isAvg) = getArg(mat[g].mi,k);
		getArg(q,3+isAvg) = getArg(mat[e].mi,k);
		pushInstruction(mb,q);

		/* pack the result into a mat */
		ai1 = pushArgument(mb,ai1,getArg(q,0));
		if (isAvg)
			ai10 = pushArgument(mb,ai10,getArg(q,1));
	}
	pushInstruction(mb, ai1);
	if (isAvg)
		pushInstruction(mb, ai10);

	/* for avg we do sum (avg*(count/sumcount) ) */
	if (isAvg) {
		InstrPtr r,s,v,w, cond;

		/* wrd s = sum counts */
 		s = newInstruction(mb, ASSIGNsymbol);
		setModuleId(s, aggrRef);
		setFunctionId(s, subsumRef);
		getArg(s,0) = newTmpVariable(mb, tp2);
		s = pushArgument(mb, s, getArg(ai10, 0));
		s = pushArgument(mb, s, mat[g].mv);
		s = pushArgument(mb, s, mat[e].mv);
		s = pushBit(mb, s, 1); /* skip nils */
		s = pushBit(mb, s, 1);
		pushInstruction(mb,s);

		/*  w=count = ifthenelse(s=count==0,NULL,s=count)  */
		cond = newInstruction(mb, ASSIGNsymbol);
		setModuleId(cond, batcalcRef);
		setFunctionId(cond, eqRef); 
		getArg(cond,0) = newTmpVariable(mb, newBatType(TYPE_oid, TYPE_bit));
		cond = pushArgument(mb, cond, getArg(s, 0));
		cond = pushWrd(mb, cond, 0);
		pushInstruction(mb,cond);

		w = newInstruction(mb, ASSIGNsymbol);
		setModuleId(w, batcalcRef);
		setFunctionId(w, ifthenelseRef); 
		getArg(w,0) = newTmpVariable(mb, tp2);
		w = pushArgument(mb, w, getArg(cond, 0));
		w = pushNil(mb, w, TYPE_wrd);
		w = pushArgument(mb, w, getArg(s, 0));
		pushInstruction(mb,w);

		/* fetchjoin with groups */
 		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r, algebraRef);
		setFunctionId(r, leftfetchjoinRef);
		getArg(r, 0) = newTmpVariable(mb, tp2);
		r = pushArgument(mb, r, mat[g].mv);
		r = pushArgument(mb, r, getArg(w,0));
		pushInstruction(mb,r);
		s = r;

		/* dbl v = double(count) */
		v = newInstruction(mb, ASSIGNsymbol);
		setModuleId(v, batcalcRef);
		setFunctionId(v, dblRef); 
		getArg(v,0) = newTmpVariable(mb, newBatType(TYPE_oid, TYPE_dbl));
		v = pushArgument(mb, v, getArg(ai10, 0));
		pushInstruction(mb, v);

		/* dbl r = v / s */
		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r, batcalcRef);
		setFunctionId(r, divRef); 
		getArg(r,0) = newTmpVariable(mb, newBatType(TYPE_oid, TYPE_dbl));
		r = pushArgument(mb, r, getArg(v, 0));
		r = pushArgument(mb, r, getArg(s, 0));
		pushInstruction(mb,r);

		/* dbl s = avg * r */
		s = newInstruction(mb, ASSIGNsymbol);
		setModuleId(s, batcalcRef);
		setFunctionId(s, mulRef); 
		getArg(s,0) = newTmpVariable(mb, tp);
		s = pushArgument(mb, s, getArg(ai1, 0));
		s = pushArgument(mb, s, getArg(r, 0));
		pushInstruction(mb,s);

		ai1 = s;
	}
 	ai2 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(ai2, aggrRef);
	setFunctionId(ai2, aggr2);
	getArg(ai2,0) = getArg(p,0);
	ai2 = pushArgument(mb, ai2, getArg(ai1, 0));
	ai2 = pushArgument(mb, ai2, mat[g].mv);
	ai2 = pushArgument(mb, ai2, mat[e].mv);
	if (isAvg)
		ai2 = pushBit(mb, ai2, 0); /* do not skip nils */
	else
		ai2 = pushBit(mb, ai2, 1); /* skip nils */
	if (getFunctionId(p) != subminRef && getFunctionId(p) != submaxRef)
		ai2 = pushBit(mb, ai2, 1);
	pushInstruction(mb, ai2);
}

/* The mat_group_{new,derive} keep an ext,attr1..attrn table.
 * This is the input for the final second phase group by.
 */
static void
mat_pack_group(MalBlkPtr mb, matlist_t *ml, int g)
{
	mat_t *mat = ml->v;
	int cnt = chain_by_length(mat, g), i;
	InstrPtr cur = NULL;

	for(i=cnt-1; i>=0; i--) {
		InstrPtr grp = newInstruction(mb, ASSIGNsymbol);
		int ogrp = walk_n_back(mat, g, i);
		int oext = group_by_ext(ml, ogrp);
		int attr = mat[oext].im;

		setModuleId(grp,groupRef);
		setFunctionId(grp, i?subgroupRef:subgroupdoneRef);
		
		getArg(grp,0) = mat[ogrp].mv;
		grp = pushReturn(mb, grp, mat[oext].mv);
		grp = pushReturn(mb, grp, newTmpVariable(mb, newBatType( TYPE_oid, TYPE_wrd)));
		grp = pushArgument(mb, grp, getArg(mat[attr].mi, 0));
		if (cur) 
			grp = pushArgument(mb, grp, getArg(cur, 0));
		pushInstruction(mb, grp);
		cur = grp;
	}
	mat[g].im = -1; /* only pack once */
}

/* 
 * foreach parent subgroup, do the 
 * 	e2.leftfetchjoin(grp.leftfetchjoin((ext.leftfetchjoin(b))) 
 * and one for the current group 
 */
static void
mat_group_attr(MalBlkPtr mb, matlist_t *ml, int g, InstrPtr cext, int push )
{
	int cnt = chain_by_length(ml->v, g), i;	/* number of attributes */
	int ogrp = g; 				/* previous group */

	for(i = 0; i < cnt; i++) {
		int agrp = walk_n_back(ml->v, ogrp, i); 
		int b = ml->v[agrp].im;
		int aext = group_by_ext(ml, agrp);
		int a = ml->v[aext].im;
		int atp = getArgType(mb,ml->v[a].mi,0), k;
		InstrPtr attr = newInstruction(mb, ASSIGNsymbol);

		setModuleId(attr,matRef);
		setFunctionId(attr,packRef);
		//getArg(attr,0) = newTmpVariable(mb, atp);
		getArg(attr,0) = getArg(ml->v[b].mi,0);

		for (k = 1; k<ml->v[a].mi->argc; k++ ) {
			InstrPtr r = newInstruction(mb, ASSIGNsymbol);
			InstrPtr q = newInstruction(mb, ASSIGNsymbol);

			setModuleId(r, algebraRef);
			setFunctionId(r, leftfetchjoinRef);
			getArg(r, 0) = newTmpVariable(mb, newBatType(TYPE_oid,TYPE_oid));
			r = pushArgument(mb, r, getArg(cext,k));
			r = pushArgument(mb, r, getArg(ml->v[ogrp].mi,k));
			pushInstruction(mb,r);

			setModuleId(q, algebraRef);
			setFunctionId(q, leftfetchjoinRef);
			getArg(q, 0) = newTmpVariable(mb, atp);
			q = pushArgument(mb, q, getArg(r,0));
			q = pushArgument(mb, q, getArg(ml->v[a].mi,k));
			pushInstruction(mb,q);
	
			attr = pushArgument(mb, attr, getArg(q, 0)); 
		}
		if (push)
			pushInstruction(mb,attr);
		mat_add_var(ml, attr, NULL, getArg(attr, 0), mat_ext,  -1, -1);
		ml->v[ml->top-1].pushed = push;
		/* keep new attribute with the group extend */
		ml->v[aext].im = ml->top-1;
	}	
}

static void
mat_group_new(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int b)
{
	int tp0 = getArgType(mb,p,0);
	int tp1 = getArgType(mb,p,1);
	int tp2 = getArgType(mb,p,2);
	int atp = getArgType(mb,p,3), i, a, g, push = 0;
	InstrPtr r0, r1, r2, attr;

	if (getFunctionId(p) == subgroupdoneRef)
		push = 1;

	r0 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r0,matRef);
	setFunctionId(r0,packRef);
	getArg(r0,0) = newTmpVariable(mb, tp0);

	r1 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r1,matRef);
	setFunctionId(r1,packRef);
	getArg(r1,0) = newTmpVariable(mb, tp1);

	r2 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r2,matRef);
	setFunctionId(r2,packRef);
	getArg(r2,0) = newTmpVariable(mb, tp2);

	/* we keep an extend, attr table result, which will later be used
	 * when we pack the group result */
	attr = newInstruction(mb, ASSIGNsymbol);
	setModuleId(attr,matRef);
	setFunctionId(attr,packRef);
	getArg(attr,0) = getArg(ml->v[b].mi,0);

	for(i=1; i<ml->v[b].mi->argc; i++) {
		InstrPtr q = copyInstruction(p), r;
		getArg(q, 0) = newTmpVariable(mb, tp0);
		getArg(q, 1) = newTmpVariable(mb, tp1);
		getArg(q, 2) = newTmpVariable(mb, tp2);
		getArg(q, 3) = getArg(ml->v[b].mi, i);
		pushInstruction(mb, q);

		/* add result to mats */
		r0 = pushArgument(mb,r0,getArg(q,0));
		r1 = pushArgument(mb,r1,getArg(q,1));
		r2 = pushArgument(mb,r2,getArg(q,2));

		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r, algebraRef);
		setFunctionId(r, leftfetchjoinRef);
		getArg(r, 0) = newTmpVariable(mb, atp);

		r = pushArgument(mb, r, getArg(q,1));
		r = pushArgument(mb, r, getArg(ml->v[b].mi,i));
		pushInstruction(mb,r);

		attr = pushArgument(mb, attr, getArg(r, 0)); 
	}
	pushInstruction(mb,r0);
	pushInstruction(mb,r1);
	pushInstruction(mb,r2);
	if (push)
		pushInstruction(mb,attr);

	/* create mat's for the intermediates */
	a = ml->top;
	mat_add_var(ml, attr, NULL, getArg(attr, 0), mat_ext,  -1, -1);
	ml->v[a].pushed = push;
	g = ml->top;
	mat_add_var(ml, r0, p, getArg(p, 0), mat_grp, b, -1);
	mat_add_var(ml, r1, p, getArg(p, 1), mat_ext, a, ml->top-1); /* point back at group */
	mat_add_var(ml, r2, p, getArg(p, 2), mat_cnt, -1, ml->top-1); /* point back at ext */
	if (push)
		mat_pack_group(mb, ml, g);
}

static void
mat_group_derive(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int b, int g)
{
	int tp0 = getArgType(mb,p,0);
	int tp1 = getArgType(mb,p,1);
	int tp2 = getArgType(mb,p,2); 
	int atp = getArgType(mb,p,3), i, a, push = 0; 
	InstrPtr r0, r1, r2, attr;

	if (getFunctionId(p) == subgroupdoneRef)
		push = 1;

	if (ml->v[g].im == -1){ /* already packed */
		pushInstruction(mb, copyInstruction(p));
		return;
	}

	r0 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r0,matRef);
	setFunctionId(r0,packRef);
	getArg(r0,0) = newTmpVariable(mb, tp0);

	r1 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r1,matRef);
	setFunctionId(r1,packRef);
	getArg(r1,0) = newTmpVariable(mb, tp1);

	r2 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r2,matRef);
	setFunctionId(r2,packRef);
	getArg(r2,0) = newTmpVariable(mb, tp2);
	
	/* we keep an extend, attr table result, which will later be used
	 * when we pack the group result */
	attr = newInstruction(mb, ASSIGNsymbol);
	setModuleId(attr,matRef);
	setFunctionId(attr,packRef);
	getArg(attr,0) = getArg(ml->v[b].mi,0);

	/* we need overlapping ranges */
	for(i=1; i<ml->v[b].mi->argc; i++) {
		InstrPtr q = copyInstruction(p), r;

		getArg(q,0) = newTmpVariable(mb, tp0);
		getArg(q,1) = newTmpVariable(mb, tp1);
		getArg(q,2) = newTmpVariable(mb, tp2);
		getArg(q,3) = getArg(ml->v[b].mi,i);
		getArg(q,4) = getArg(ml->v[g].mi,i);
		pushInstruction(mb,q);
	
		/* add result to mats */
		r0 = pushArgument(mb,r0,getArg(q,0));
		r1 = pushArgument(mb,r1,getArg(q,1));
		r2 = pushArgument(mb,r2,getArg(q,2));

		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r, algebraRef);
		setFunctionId(r, leftfetchjoinRef);
		getArg(r, 0) = newTmpVariable(mb, atp);

		r = pushArgument(mb, r, getArg(q,1));
		r = pushArgument(mb, r, getArg(ml->v[b].mi,i));
		pushInstruction(mb,r);

		attr = pushArgument(mb, attr, getArg(r, 0)); 
	}
	pushInstruction(mb,r0);
	pushInstruction(mb,r1);
	pushInstruction(mb,r2);
	if (push)
		pushInstruction(mb,attr);

	mat_group_attr(mb, ml, g, r1, push);

	/* create mat's for the intermediates */
	a = ml->top;
	mat_add_var(ml, attr, NULL, getArg(attr, 0), mat_ext,  -1, -1);
	ml->v[a].pushed = push;
	mat_add_var(ml, r0, p, getArg(p, 0), mat_grp, b, g);
	g = ml->top-1;
	mat_add_var(ml, r1, p, getArg(p, 1), mat_ext, a, ml->top-1); /* point back at group */
	mat_add_var(ml, r2, p, getArg(p, 2), mat_cnt, -1, ml->top-1); /* point back at ext */

	if (push)
		mat_pack_group(mb, ml, g);
}

static void
mat_topn_project(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int n)
{
	int tpe = getArgType(mb, p, 0), k;
	InstrPtr pck, q;

	pck = newInstruction(mb, ASSIGNsymbol);
	setModuleId(pck,matRef);
	setFunctionId(pck,packRef);
	getArg(pck,0) = newTmpVariable(mb, tpe);

	for(k=1; k<mat[m].mi->argc; k++) { 
		InstrPtr q = copyInstruction(p);

		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi, k);
		getArg(q,2) = getArg(mat[n].mi, k);
		pushInstruction(mb, q);

		pck = pushArgument(mb, pck, getArg(q, 0));
	}
	pushInstruction(mb, pck);

       	q = copyInstruction(p);
	getArg(q,2) = getArg(pck,0);
	pushInstruction(mb, q);
}

static void
mat_pack_topn(MalBlkPtr mb, InstrPtr slc, mat_t *mat, int m)
{
	/* find chain of topn's */
	int cnt = chain_by_length(mat, m), i;
	InstrPtr cur = NULL;

	for(i=cnt-1; i>=0; i--) {
		int otpn = walk_n_back(mat, m, i), var = 1, k;
		int attr = mat[otpn].im;
		int tpe = getVarType(mb, getArg(mat[attr].mi,0));
		InstrPtr pck, tpn, otopn = mat[otpn].org, a;

	        pck = newInstruction(mb, ASSIGNsymbol);
		setModuleId(pck,matRef);
		setFunctionId(pck,packRef);
		getArg(pck,0) = newTmpVariable(mb, tpe);

		/* m.leftfetchjoin(attr); */
		for(k=1; k < mat[attr].mi->argc; k++) {
			InstrPtr q = newInstruction(mb, ASSIGNsymbol);
			setModuleId(q, algebraRef);
			setFunctionId(q, leftjoinRef);
			getArg(q, 0) = newTmpVariable(mb, tpe);

			q = pushArgument(mb, q, getArg(slc, k));
			q = pushArgument(mb, q, getArg(mat[attr].mi, k));
			pushInstruction(mb, q);

			pck = pushArgument(mb, pck, getArg(q,0));
		}
		pushInstruction(mb, pck);

		a = pck;

		tpn = copyInstruction(otopn);
		var = 1;
		if (cur) {
			getArg(tpn, tpn->retc+var) = getArg(cur, 0);
			var++;
			if (cur->retc == 2) {
				getArg(tpn, tpn->retc+var) = getArg(cur, 1);
				var++;
			}
		}
		getArg(tpn, tpn->retc) = getArg(a,0);
		pushInstruction(mb, tpn);
		cur = tpn;
	}
}

static void
mat_topn(MalBlkPtr mb, InstrPtr p, matlist_t *ml, int m, int n, int o)
{
	int tpe = getArgType(mb,p,0), k, is_slice = isSlice(p), zero = -1;
	InstrPtr pck, gpck = NULL, q, r;
	int with_groups = (p->retc == 2), piv = 0, topn2 = (n >= 0);

	assert( topn2 || o < 0);
	/* dummy mat instruction (needed to share result of p) */
	pck = newInstruction(mb,ASSIGNsymbol);
	setModuleId(pck, matRef);
	setFunctionId(pck, packRef);
	getArg(pck,0) = getArg(p,0);

	if (with_groups) {
		gpck = newInstruction(mb,ASSIGNsymbol);
		setModuleId(gpck, matRef);
		setFunctionId(gpck, packRef);
		getArg(gpck,0) = getArg(p,1);
	}

	if (is_slice) {
		ValRecord cst;
		cst.vtype= getArgType(mb,p,2);
		cst.val.wval= 0;
		zero = defConstant(mb, cst.vtype, &cst);
	}
	assert( (n<0 && o<0) || 
		(ml->v[m].mi->argc == ml->v[n].mi->argc && 
		 ml->v[m].mi->argc == ml->v[o].mi->argc));
	
	for(k=1; k< ml->v[m].mi->argc; k++) {
		q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tpe);
		if (with_groups)
			getArg(q,1) = newTmpVariable(mb, tpe);
		getArg(q,q->retc) = getArg(ml->v[m].mi,k);
		if (is_slice) /* lower bound should always be 0 on partial slices */
			getArg(q,q->retc+1) = zero;
		else if (topn2) {
			getArg(q,q->retc+1) = getArg(ml->v[n].mi,k);
			getArg(q,q->retc+2) = getArg(ml->v[o].mi,k);
		}
		pushInstruction(mb,q);
		
		pck = pushArgument(mb, pck, getArg(q,0));
		if (with_groups)
			gpck = pushArgument(mb, gpck, getArg(q,1));
	}

	piv = ml->top;
	mat_add_var(ml, pck, p, getArg(p,0), is_slice?mat_slc:mat_tpn, m, n);
	ml->v[ml->top-1].pushed = 0;
	if (with_groups)
		mat_add_var(ml, gpck, p, getArg(p,1), is_slice?mat_slc:mat_tpn, m, piv);

	if (is_slice || p->retc ==1 /* single result, ie last of the topn's */) {
		if (ml->v[m].type == mat_tpn || !is_slice) 
			mat_pack_topn(mb, pck, ml->v, (!is_slice)?piv:m);

		/* topn/slice over merged parts */
		if (is_slice) {
			/* real instruction */
			r = newInstruction(mb,ASSIGNsymbol);
			setModuleId(r, matRef);
			setFunctionId(r, packRef);
			getArg(r,0) = newTmpVariable(mb, tpe);
	
			for(k=1; k< pck->argc; k++) 
				r = pushArgument(mb, r, getArg(pck,k));
			pushInstruction(mb,r);

			q = copyInstruction(p);
			if (ml->v[m].type != mat_tpn || is_slice) 
				getArg(q,1) = getArg(r,0);
			pushInstruction(mb,q);
		}

		ml->v[piv].packed = 1;
		ml->v[piv].type = mat_slc;
	}
}

int
OPTmergetableImplementation(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p) 
{
	InstrPtr *old;
	matlist_t ml;
	int oldtop, fm, fn, fo, fe, i, k, m, n, o, e, slimit;
	int size=0, match, actions=0, distinct_topn = 0, /*topn_res = 0,*/ groupdone = 0, *vars;

	old = mb->stmt;
	oldtop= mb->stop;

	vars= (int*) GDKmalloc(sizeof(int)* mb->vtop);
	if( vars == NULL){
		GDKerror("mergetable"MAL_MALLOC_FAIL);
		return 0;
	}
	/* check for bailout conditions */
	for (i = 1; i < oldtop; i++) {
		int j;

		p = old[i];

		for (j = 0; j<p->retc; j++) {
 			int res = getArg(p, j);
			vars[res] = i;
		}

		/* pack if there is a group statement following a groupdone (ie aggr(distinct)) */
		if (getModuleId(p) == groupRef && p->argc == 5 && 
		   (getFunctionId(p) == subgroupRef || getFunctionId(p) == subgroupdoneRef)) {
			InstrPtr q = old[vars[getArg(p, p->argc-1)]]; /* group result from a previous group(done) */

			if (getModuleId(q) == groupRef && getFunctionId(q) == subgroupdoneRef)
				groupdone = 1;
		}

		/*
		if (isTopn(p))
			topn_res = getArg(p, 0);
			*/
		/* not idea how to detect this yet */
			//distinct_topn = 1;
	}
	GDKfree(vars);

	/* the number of MATs is limited to the variable stack*/
	ml.size = mb->vtop;
	ml.top = 0;
	ml.v = (mat_t*) GDKzalloc(ml.size * sizeof(mat_t));
	if ( ml.v == NULL) 
		return 0;

	slimit = mb->ssize;
	size = (mb->stop * 1.2 < mb->ssize)? mb->ssize:(int)(mb->stop * 1.2);
	mb->stmt = (InstrPtr *) GDKzalloc(size * sizeof(InstrPtr));
	if ( mb->stmt == NULL) {
		mb->stmt = old;
		GDKfree(ml.v);
		return 0;
	}
	mb->ssize = size;
	mb->stop = 0;

	for( i=0; i<oldtop; i++){
		int bats = 0;
		InstrPtr r;

		p = old[i];
		if (getModuleId(p) == matRef && 
		   (getFunctionId(p) == newRef || getFunctionId(p) == packRef)){
			mat_set_prop(mb, p);
			mat_add_var(&ml, p, NULL, getArg(p,0), mat_none, -1, -1);
			continue;
		}

		/*
		 * If the instruction does not contain MAT references it can simply be added.
		 * Otherwise we have to decide on either packing them or replacement.
		 */
		if ((match = nr_of_mats(p, &ml)) == 0) {
			pushInstruction(mb, copyInstruction(p));
			continue;
		}
		bats = nr_of_bats(mb, p);

		/* (l,r) Join (L, R, ..) */
		if (match > 0 && isMatJoinOp(p) && p->argc >= 3 && p->retc == 2 &&
				match <= 3 && bats >= 2) {
		   	m = is_a_mat(getArg(p,p->retc), &ml);
		   	n = is_a_mat(getArg(p,p->retc+1), &ml);
		   	o = is_a_mat(getArg(p,p->retc+2), &ml);

			if (bats == 3 && match >= 2)
				mat_join3(mb, p, &ml, m, n, o);
			else
				mat_join2(mb, p, &ml, m, n);
			actions++;
			continue;
		}
		if (match > 0 && isMatLeftJoinOp(p) && p->argc >= 3 && p->retc == 2 &&
				match == 1 && bats == 2) {
		   	m = is_a_mat(getArg(p,p->retc), &ml);
			n = -1;

			if (m >= 0) {
				mat_join2(mb, p, &ml, m, n);
				actions++;
				continue;
			}
		}
		/*
		 * Aggregate handling is a prime target for optimization.
		 * The simple cases are dealt with first.
		 * Handle the rewrite v:=aggr.count(b) and sum()
		 * And the min/max is as easy
		 */
		if (match == 1 && p->argc == 2 &&
		   ((getModuleId(p)==aggrRef &&
			(getFunctionId(p)== countRef || 
			 getFunctionId(p)== count_no_nilRef || 
			 getFunctionId(p)== minRef ||
			 getFunctionId(p)== maxRef ||
			 getFunctionId(p)== avgRef ||
			 getFunctionId(p)== sumRef ||
			 getFunctionId(p) == prodRef))) &&
			(m=is_a_mat(getArg(p,1), &ml)) >= 0) {
			mat_aggr(mb, p, ml.v, m);
			actions++;
			continue;
		} 

		if (match == 1 && bats == 1 && p->argc == 4 && isSlice(p) && ((m=is_a_mat(getArg(p,p->retc), &ml)) >= 0)) {
			mat_topn(mb, p, &ml, m, -1, -1);
			actions++;
			continue;
		}

		if (!distinct_topn && match == 1 && bats == 1 && (p->argc-p->retc) == 4 && isTopn(p) && ((m=is_a_mat(getArg(p,p->retc), &ml)) >= 0)) {
			mat_topn(mb, p, &ml, m, -1, -1);
			actions++;
			continue;
		}
		if (!distinct_topn && match == 3 && bats == 3 && (p->argc-p->retc) == 6 && isTopn(p) &&
	 	   ((m=is_a_mat(getArg(p,p->retc), &ml)) >= 0) &&
	 	   ((n=is_a_mat(getArg(p,p->retc+1), &ml)) >= 0) &&
	 	   ((o=is_a_mat(getArg(p,p->retc+2), &ml)) >= 0)) {
			mat_topn(mb, p, &ml, m, n, o);
			actions++;
			continue;
		}

		/* Now we handle subgroup and aggregation statements. */
		if (!groupdone && match == 1 && bats == 1 && p->argc == 4 && getModuleId(p) == groupRef && 
		   (getFunctionId(p) == subgroupRef || getFunctionId(p) == subgroupdoneRef) && 
	 	   ((m=is_a_mat(getArg(p,p->retc), &ml)) >= 0)) {
			mat_group_new(mb, p, &ml, m);
			actions++;
			continue;
		}
		if (!groupdone && match == 2 && bats == 2 && p->argc == 5 && getModuleId(p) == groupRef && 
		   (getFunctionId(p) == subgroupRef || getFunctionId(p) == subgroupdoneRef) && 
		   ((m=is_a_mat(getArg(p,p->retc), &ml)) >= 0) &&
		   ((n=is_a_mat(getArg(p,p->retc+1), &ml)) >= 0) && 
		     ml.v[n].im >= 0 /* not packed */) {
			mat_group_derive(mb, p, &ml, m, n);
			actions++;
			continue;
		}
		/* TODO sub'aggr' with cand list */
		if (match == 3 && bats == 3 && getModuleId(p) == aggrRef && p->argc >= 4 &&
		   (getFunctionId(p) == subcountRef ||
		    getFunctionId(p) == subminRef ||
		    getFunctionId(p) == submaxRef ||
		    getFunctionId(p) == subavgRef ||
		    getFunctionId(p) == subsumRef ||
		    getFunctionId(p) == subprodRef) &&
		   ((m=is_a_mat(getArg(p,1), &ml)) >= 0) &&
		   ((n=is_a_mat(getArg(p,2), &ml)) >= 0) &&
		   ((o=is_a_mat(getArg(p,3), &ml)) >= 0)) {
			mat_group_aggr(mb, p, ml.v, m, n, o);
			actions++;
			continue;
		}
		/* Handle cases of ext.leftfetchjoin and .leftfetchjoin(grp) */
		if (match == 2 && getModuleId(p) == algebraRef &&
		    getFunctionId(p) == leftfetchjoinRef &&
		   (m=is_a_mat(getArg(p,1), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,2), &ml)) >= 0 &&
		   (ml.v[m].type == mat_ext || ml.v[n].type == mat_grp)) {
			assert(ml.v[m].pushed);
			if (!ml.v[n].pushed) 
				mat_group_project(mb, p, &ml, m, n);
			else
				pushInstruction(mb, copyInstruction(p));
			continue;
		}

		/* Handle cases of slice.leftfetchjoin */
		if (match == 2 && getModuleId(p) == algebraRef &&
		    getFunctionId(p) == leftfetchjoinRef &&
		   (m=is_a_mat(getArg(p,1), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,2), &ml)) >= 0 &&
		   (ml.v[m].type == mat_slc)) {
			mat_topn_project(mb, p, ml.v, m, n);
			actions++;
			continue;
		}

		/* Handle leftfetchjoin */
		if (match > 0 && getModuleId(p) == algebraRef &&
		    getFunctionId(p) == leftfetchjoinRef && 
		   (m=is_a_mat(getArg(p,1), &ml)) >= 0) { 
		   	n=is_a_mat(getArg(p,2), &ml);
			mat_leftfetchjoin(mb, p, &ml, m, n);
			actions++;
			continue;
		}
		/* Handle setops */
		if (match > 0 && getModuleId(p) == algebraRef &&
		    (getFunctionId(p) == tdiffRef || 
		     getFunctionId(p) == tinterRef) && 
		   (m=is_a_mat(getArg(p,1), &ml)) >= 0) { 
		   	n=is_a_mat(getArg(p,2), &ml);
			mat_setop(mb, p, &ml, m, n);
			actions++;
			continue;
		}

		m = n = o = e = -1;
		for( fm= p->argc-1; fm>=p->retc ; fm--)
			if ((m=is_a_mat(getArg(p,fm), &ml)) >= 0)
				break;

		for( fn= fm-1; fn>=p->retc ; fn--)
			if ((n=is_a_mat(getArg(p,fn), &ml)) >= 0)
				break;

		for( fo= fn-1; fo>=p->retc ; fo--)
			if ((o=is_a_mat(getArg(p,fo), &ml)) >= 0)
				break;

		for( fe= fo-1; fe>=p->retc ; fe--)
			if ((e=is_a_mat(getArg(p,fe), &ml)) >= 0)
				break;

		/* delta* operator have a ins bat as last argument, we move the inserts into the last delta statement, ie
  		 * all but last need to remove one argument */
		if (match == 3 && bats == 4 && isDelta(p) && 
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,fn), &ml)) >= 0 &&
		   (o=is_a_mat(getArg(p,fo), &ml)) >= 0){
			if ((r = mat_delta(mb, p, ml.v, m, n, o, -1, fm, fn, fo, 0)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}
		if (match == 4 && bats == 5 && isDelta(p) && 
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,fn), &ml)) >= 0 &&
		   (o=is_a_mat(getArg(p,fo), &ml)) >= 0 &&
		   (e=is_a_mat(getArg(p,fe), &ml)) >= 0){
			if ((r = mat_delta(mb, p, ml.v, m, n, o, e, fm, fn, fo, fe)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}

		/* subselect on insert, should use last tid only */
		if (match == 1 && fm == 2 && isSubSelect(p) && p->retc == 1 &&
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0) {
			r = copyInstruction(p);
			getArg(r, fm) = getArg(ml.v[m].mi, ml.v[m].mi->argc-1);
			pushInstruction(mb, r);
			actions++;
			continue;
		}

		/* subselect on update, with nil bat */
		if (match == 1 && fm == 1 && isSubSelect(p) && p->retc == 1 && 
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0 && bats == 2 &&
		   isaBatType(getArgType(mb,p,2)) && isVarConstant(mb,getArg(p,2)) && getVarConstant(mb,getArg(p,2)).val.bval == bat_nil) {
			if ((r = mat_apply1(mb, p, &ml, m, fm)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}


		if (match == 3 && bats == 3 && (isFragmentGroup(p) || isFragmentGroup2(p) || isMapOp(p)) &&  p->retc != 2 &&
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,fn), &ml)) >= 0 &&
		   (o=is_a_mat(getArg(p,fo), &ml)) >= 0){
			assert(ml.v[m].mi->argc == ml.v[n].mi->argc); 
			if ((r = mat_apply3(mb, p, ml.v, m, n, o, fm, fn, fo)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}
		if (match == 2 && bats == 2 && (isFragmentGroup(p) || isFragmentGroup2(p) || isMapOp(p)) &&  p->retc != 2 &&
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0 &&
		   (n=is_a_mat(getArg(p,fn), &ml)) >= 0){
			assert(ml.v[m].mi->argc == ml.v[n].mi->argc); 
			if ((r = mat_apply2(mb, p, ml.v, m, n, fm, fn)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}

		if (match == 1 && bats == 1 && (isFragmentGroup(p) || isMapOp(p) || 
		   (!getModuleId(p) && !getFunctionId(p) && p->barrier == 0 /* simple assignment */)) && p->retc != 2 && 
		   (m=is_a_mat(getArg(p,fm), &ml)) >= 0){
			if ((r = mat_apply1(mb, p, &ml, m, fm)) != NULL)
				mat_add(&ml, r, mat_type(ml.v, m), getFunctionId(p));
			actions++;
			continue;
		}

		/*
		 * All other instructions should be checked for remaining MAT dependencies.
		 * It requires MAT materialization.
		 */
		OPTDEBUGmergetable mnstr_printf(GDKout, "# %s.%s %d\n", getModuleId(p), getFunctionId(p), match);

		for (k = p->retc; k<p->argc; k++) {
			if((m=is_a_mat(getArg(p,k), &ml)) >= 0){
				mat_pack(mb, ml.v, m);
				actions++;
			}
		}
		pushInstruction(mb, copyInstruction(p));
	}
	(void) stk; 
	chkTypes(cntxt->fdout, cntxt->nspace,mb, TRUE);

	OPTDEBUGmergetable {
		str err;
		mnstr_printf(GDKout,"#Result of multi table optimizer\n");
		err= optimizerCheck(cntxt,mb,"merge test",1,0);
		printFunction(GDKout, mb, 0, LIST_MAL_ALL);
		if( err) GDKfree(err);
	}

	if ( mb->errors == 0) {
		for(i=0; i<slimit; i++)
			if (old[i]) 
				freeInstruction(old[i]);
		GDKfree(old);
	}
	for (i=0; i<ml.top; i++) {
		if (ml.v[i].mi && !ml.v[i].pushed)
			freeInstruction(ml.v[i].mi);
	}
	GDKfree(ml.v);
	return actions;
}
