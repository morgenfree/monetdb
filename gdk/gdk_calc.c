/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_private.h"
#include "gdk_calc_private.h"
#include <math.h>

/* Define symbol FULL_IMPLEMENTATION to get implementations for all
 * sensible output types for +, -, *, /.  Without the symbol, all
 * combinations of input types are supported, but only output types
 * that are either the largest of the input types or one size larger
 * (if available) for +, -, *.  For division the output type can be
 * either input type of flt or dbl. */

/* Generally, the functions return a new BAT aligned with the input
 * BAT(s).  If there are multiple input BATs, they must be aligned.
 * If there is a candidate list, the calculations are only done for
 * the candidates, all other values are NIL (so that the output is
 * still aligned). */

/* format strings for the seven/eight basic types we deal with */
#define FMTbte	"%d"
#define FMTsht	"%d"
#define FMTint	"%d"
#define FMTlng	LLFMT
#ifdef HAVE_HGE
#define FMThge	"%.40g"
#endif
#define FMTflt	"%.9g"
#define FMTdbl	"%.17g"
#define FMToid	OIDFMT

/* casts; only required for type hge, since there is no genuine format
 * string for it (i.e., for __int128) (yet?) */
#define CSTbte
#define CSTsht
#define CSTint
#define CSTlng
#ifdef HAVE_HGE
#define CSThge  (dbl)
#endif
#define CSTflt
#define CSTdbl
#define CSToid

/* Most of the internal routines return a count of the number of NIL
 * values they produced.  They indicate an error by returning a value
 * >= BUN_NONE.  BUN_NONE means that the error was dealt with by
 * calling GDKerror (generally for overflow or conversion errors).
 * BUN_NONE+1 is returned by the DIV and MOD functions to indicate
 * division by zero.  */

static gdk_return
checkbats(BAT *b1, BAT *b2, const char *func)
{
	if (!BAThdense(b1) || (b2 != NULL && !BAThdense(b2))) {
		GDKerror("%s: inputs must have dense head.\n", func);
		return GDK_FAIL;
	}
	if (b2 != NULL) {
		if (b1->batCount != b2->batCount) {
			GDKerror("%s: inputs not the same size.\n", func);
			return GDK_FAIL;
		}
	}
	return GDK_SUCCEED;
}

#define CHECKCAND(dst, i, candoff, NIL)				\
	/* cannot use do/while trick because of continue */	\
	/* NOTE: because of the continue, you *must* use the */	\
	/* index (i) to index src/dst */			\
	if (cand) {						\
		if ((i) < *cand - (candoff)) {			\
			nils++;					\
			(dst)[i] = (NIL);			\
			continue;				\
		}						\
		assert((i) == *cand - (candoff));		\
		if (++cand == (candend))			\
			end = (i) + 1;				\
	}

/* fill in NILs from low to high, used to write NILs before and after
 * the range that the candidates list covers */
#define CANDLOOP(dst, i, NIL, low, high)		\
	do {						\
		for ((i) = (low); (i) < (high); (i)++)	\
			(dst)[i] = NIL;			\
		nils += (high) - (low);			\
	} while (0)

#define UNARY_2TYPE_FUNC(TYPE1, TYPE2, FUNC)				\
	do {								\
		const TYPE1 *restrict src = (const TYPE1 *) Tloc(b, b->batFirst); \
		TYPE2 *restrict dst = (TYPE2 *) Tloc(bn, bn->batFirst); \
		CANDLOOP(dst, i, TYPE2##_nil, 0, start);		\
		if (b->T->nonil && cand == NULL) {			\
			for (i = start; i < end; i++)			\
				dst[i] = FUNC(src[i]);			\
		} else {						\
			for (i = start; i < end; i++) {			\
				CHECKCAND(dst, i, b->H->seq, TYPE2##_nil); \
				if (src[i] == TYPE1##_nil) {		\
					nils++;				\
					dst[i] = TYPE2##_nil;		\
				} else {				\
					dst[i] = FUNC(src[i]);		\
				}					\
			}						\
		}							\
		CANDLOOP(dst, i, TYPE2##_nil, end, cnt);		\
	} while (0)

#define BINARY_3TYPE_FUNC(TYPE1, TYPE2, TYPE3, FUNC)			\
	do {								\
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, 0, start);	\
		for (i = start * incr1, j = start * incr2, k = start;	\
		     k < end; i += incr1, j += incr2, k++) {		\
			CHECKCAND((TYPE3 *) dst, k, candoff, TYPE3##_nil); \
			if (((const TYPE1 *) lft)[i] == TYPE1##_nil ||	\
			    ((const TYPE2 *) rgt)[j] == TYPE2##_nil) {	\
				nils++;					\
				((TYPE3 *) dst)[k] = TYPE3##_nil;	\
			} else {					\
				((TYPE3 *) dst)[k] = FUNC(((const TYPE1 *) lft)[i], \
							  ((const TYPE2 *) rgt)[j]); \
			}						\
		}							\
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, end, cnt);	\
	} while (0)

#define BINARY_3TYPE_FUNC_nonil(TYPE1, TYPE2, TYPE3, FUNC)		\
	do {								\
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, 0, start);	\
		for (i = start * incr1, j = start * incr2, k = start;	\
		     k < end; i += incr1, j += incr2, k++)		\
			((TYPE3 *) dst)[k] = FUNC(((const TYPE1 *) lft)[i], \
						  ((const TYPE2 *) rgt)[j]); \
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, end, cnt);	\
	} while (0)

#define BINARY_3TYPE_FUNC_CHECK(TYPE1, TYPE2, TYPE3, FUNC, CHECK)	\
	do {								\
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, 0, start);	\
		for (i = start * incr1, j = start * incr2, k = start;	\
		     k < end; i += incr1, j += incr2, k++) {		\
			CHECKCAND((TYPE3 *) dst, k, candoff, TYPE3##_nil); \
			if (((const TYPE1 *) lft)[i] == TYPE1##_nil ||	\
			    ((const TYPE2 *) rgt)[j] == TYPE2##_nil) {	\
				nils++;					\
				((TYPE3 *) dst)[k] = TYPE3##_nil;	\
			} else if (CHECK(((const TYPE1 *) lft)[i],	\
					 ((const TYPE2 *) rgt)[j])) {	\
				if (abort_on_error) {			\
					GDKerror("%s: shift operand too large in " \
						 #FUNC"("FMT##TYPE1","FMT##TYPE2").\n", \
						 func,			\
						 CST##TYPE1 ((const TYPE1 *) lft)[i], \
						 CST##TYPE2 ((const TYPE2 *) rgt)[j]); \
					goto checkfail;			\
				}					\
				((TYPE3 *)dst)[k] = TYPE3##_nil;	\
				nils++;					\
			} else {					\
				((TYPE3 *) dst)[k] = FUNC(((const TYPE1 *) lft)[i], \
							  ((const TYPE2 *) rgt)[j]); \
			}						\
		}							\
		CANDLOOP((TYPE3 *) dst, k, TYPE3##_nil, end, cnt);	\
	} while (0)

/* ---------------------------------------------------------------------- */
/* logical (for type bit) or bitwise (for integral types) NOT */

#define NOT(x)		(~(x))
#define NOTBIT(x)	(!(x))

BAT *
BATcalcnot(BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils = 0;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcnot", NULL);
	if (checkbats(b, NULL, "BATcalcnot") != GDK_SUCCEED)
		return NULL;
	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		if (b->T->type == TYPE_bit) {
			UNARY_2TYPE_FUNC(bit, bit, NOTBIT);
		} else {
			UNARY_2TYPE_FUNC(bte, bte, NOT);
		}
		break;
	case TYPE_sht:
		UNARY_2TYPE_FUNC(sht, sht, NOT);
		break;
	case TYPE_int:
		UNARY_2TYPE_FUNC(int, int, NOT);
		break;
	case TYPE_lng:
		UNARY_2TYPE_FUNC(lng, lng, NOT);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		UNARY_2TYPE_FUNC(hge, hge, NOT);
		break;
#endif
	default:
		BBPunfix(bn->batCacheid);
		GDKerror("BATcalcnot: type %s not supported.\n",
			 ATOMname(b->T->type));
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* NOT reverses the order, but NILs mess it up */
	bn->T->sorted = nils == 0 && b->T->revsorted;
	bn->T->revsorted = nils == 0 && b->T->sorted;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;
	bn->T->key = b->T->key & 1;

	if (nils != 0 && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

gdk_return
VARcalcnot(ValPtr ret, const ValRecord *v)
{
	ret->vtype = v->vtype;
	switch (ATOMbasetype(v->vtype)) {
	case TYPE_bte:
		if (v->val.btval == bit_nil)
			ret->val.btval = bit_nil;
		else if (v->vtype == TYPE_bit)
			ret->val.btval = !v->val.btval;
		else
			ret->val.btval = ~v->val.btval;
		break;
	case TYPE_sht:
		if (v->val.shval == sht_nil)
			ret->val.shval = sht_nil;
		else
			ret->val.shval = ~v->val.shval;
		break;
	case TYPE_int:
		if (v->val.ival == int_nil)
			ret->val.ival = int_nil;
		else
			ret->val.ival = ~v->val.ival;
		break;
	case TYPE_lng:
		if (v->val.lval == lng_nil)
			ret->val.lval = lng_nil;
		else
			ret->val.lval = ~v->val.lval;
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (v->val.hval == hge_nil)
			ret->val.hval = hge_nil;
		else
			ret->val.hval = ~v->val.hval;
		break;
#endif
	default:
		GDKerror("VARcalcnot: bad input type %s.\n",
			 ATOMname(v->vtype));
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* negate value (any numeric type) */

#define NEGATE(x)	(-(x))

BAT *
BATcalcnegate(BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils = 0;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcnegate", NULL);
	if (checkbats(b, NULL, "BATcalcnegate") != GDK_SUCCEED)
		return NULL;
	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		UNARY_2TYPE_FUNC(bte, bte, NEGATE);
		break;
	case TYPE_sht:
		UNARY_2TYPE_FUNC(sht, sht, NEGATE);
		break;
	case TYPE_int:
		UNARY_2TYPE_FUNC(int, int, NEGATE);
		break;
	case TYPE_lng:
		UNARY_2TYPE_FUNC(lng, lng, NEGATE);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		UNARY_2TYPE_FUNC(hge, hge, NEGATE);
		break;
#endif
	case TYPE_flt:
		UNARY_2TYPE_FUNC(flt, flt, NEGATE);
		break;
	case TYPE_dbl:
		UNARY_2TYPE_FUNC(dbl, dbl, NEGATE);
		break;
	default:
		BBPunfix(bn->batCacheid);
		GDKerror("BATcalcnegate: type %s not supported.\n",
			 ATOMname(b->T->type));
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* unary - reverses the order, but NILs mess it up */
	bn->T->sorted = nils == 0 && b->T->revsorted;
	bn->T->revsorted = nils == 0 && b->T->sorted;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;
	bn->T->key = b->T->key & 1;

	if (nils != 0 && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

gdk_return
VARcalcnegate(ValPtr ret, const ValRecord *v)
{
	ret->vtype = v->vtype;
	switch (ATOMbasetype(v->vtype)) {
	case TYPE_bte:
		if (v->val.btval == bte_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = -v->val.btval;
		break;
	case TYPE_sht:
		if (v->val.shval == sht_nil)
			ret->val.shval = sht_nil;
		else
			ret->val.shval = -v->val.shval;
		break;
	case TYPE_int:
		if (v->val.ival == int_nil)
			ret->val.ival = int_nil;
		else
			ret->val.ival = -v->val.ival;
		break;
	case TYPE_lng:
		if (v->val.lval == lng_nil)
			ret->val.lval = lng_nil;
		else
			ret->val.lval = -v->val.lval;
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (v->val.hval == hge_nil)
			ret->val.hval = hge_nil;
		else
			ret->val.hval = -v->val.hval;
		break;
#endif
	case TYPE_flt:
		if (v->val.fval == flt_nil)
			ret->val.fval = flt_nil;
		else
			ret->val.fval = -v->val.fval;
		break;
	case TYPE_dbl:
		if (v->val.dval == dbl_nil)
			ret->val.dval = dbl_nil;
		else
			ret->val.dval = -v->val.dval;
		break;
	default:
		GDKerror("VARcalcnegate: bad input type %s.\n",
			 ATOMname(v->vtype));
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* absolute value (any numeric type) */

BAT *
BATcalcabsolute(BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils= 0;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcabsolute", NULL);
	if (checkbats(b, NULL, "BATcalcabsolute") != GDK_SUCCEED)
		return NULL;
	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		UNARY_2TYPE_FUNC(bte, bte, (bte) abs);
		break;
	case TYPE_sht:
		UNARY_2TYPE_FUNC(sht, sht, (sht) abs);
		break;
	case TYPE_int:
		UNARY_2TYPE_FUNC(int, int, abs);
		break;
	case TYPE_lng:
		UNARY_2TYPE_FUNC(lng, lng, llabs);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		UNARY_2TYPE_FUNC(hge, hge, ABSOLUTE);
		break;
#endif
	case TYPE_flt:
		UNARY_2TYPE_FUNC(flt, flt, fabsf);
		break;
	case TYPE_dbl:
		UNARY_2TYPE_FUNC(dbl, dbl, fabs);
		break;
	default:
		BBPunfix(bn->batCacheid);
		GDKerror("BATcalcabsolute: bad input type %s.\n",
			 ATOMname(b->T->type));
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* ABSOLUTE messes up order (unless all values were negative
	 * or all values were positive, but we don't know anything
	 * about that) */
	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	if (nils && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

gdk_return
VARcalcabsolute(ValPtr ret, const ValRecord *v)
{
	ret->vtype = v->vtype;
	switch (ATOMbasetype(v->vtype)) {
	case TYPE_bte:
		if (v->val.btval == bte_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = (bte) abs(v->val.btval);
		break;
	case TYPE_sht:
		if (v->val.shval == sht_nil)
			ret->val.shval = sht_nil;
		else
			ret->val.shval = (sht) abs(v->val.shval);
		break;
	case TYPE_int:
		if (v->val.ival == int_nil)
			ret->val.ival = int_nil;
		else
			ret->val.ival = abs(v->val.ival);
		break;
	case TYPE_lng:
		if (v->val.lval == lng_nil)
			ret->val.lval = lng_nil;
		else
			ret->val.lval = llabs(v->val.lval);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (v->val.hval == hge_nil)
			ret->val.hval = hge_nil;
		else
			ret->val.hval = ABSOLUTE(v->val.hval);
		break;
#endif
	case TYPE_flt:
		if (v->val.fval == flt_nil)
			ret->val.fval = flt_nil;
		else
			ret->val.fval = fabsf(v->val.fval);
		break;
	case TYPE_dbl:
		if (v->val.dval == dbl_nil)
			ret->val.dval = dbl_nil;
		else
			ret->val.dval = fabs(v->val.dval);
		break;
	default:
		GDKerror("VARcalcabsolute: bad input type %s.\n",
			 ATOMname(v->vtype));
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* is the value equal to zero (any numeric type) */

#define ISZERO(x)		((bit) ((x) == 0))

BAT *
BATcalciszero(BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils = 0;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalciszero", NULL);
	if (checkbats(b, NULL, "BATcalciszero") != GDK_SUCCEED)
		return NULL;
	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, TYPE_bit, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		UNARY_2TYPE_FUNC(bte, bit, ISZERO);
		break;
	case TYPE_sht:
		UNARY_2TYPE_FUNC(sht, bit, ISZERO);
		break;
	case TYPE_int:
		UNARY_2TYPE_FUNC(int, bit, ISZERO);
		break;
	case TYPE_lng:
		UNARY_2TYPE_FUNC(lng, bit, ISZERO);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		UNARY_2TYPE_FUNC(hge, bit, ISZERO);
		break;
#endif
	case TYPE_flt:
		UNARY_2TYPE_FUNC(flt, bit, ISZERO);
		break;
	case TYPE_dbl:
		UNARY_2TYPE_FUNC(dbl, bit, ISZERO);
		break;
	default:
		BBPunfix(bn->batCacheid);
		GDKerror("BATcalciszero: bad input type %s.\n",
			 ATOMname(b->T->type));
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	if (nils != 0 && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

gdk_return
VARcalciszero(ValPtr ret, const ValRecord *v)
{
	ret->vtype = TYPE_bit;
	switch (ATOMbasetype(v->vtype)) {
	case TYPE_bte:
		if (v->val.btval == bte_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.btval);
		break;
	case TYPE_sht:
		if (v->val.shval == sht_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.shval);
		break;
	case TYPE_int:
		if (v->val.ival == int_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.ival);
		break;
	case TYPE_lng:
		if (v->val.lval == lng_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.lval);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (v->val.hval == hge_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.hval);
		break;
#endif
	case TYPE_flt:
		if (v->val.fval == flt_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.fval);
		break;
	case TYPE_dbl:
		if (v->val.dval == dbl_nil)
			ret->val.btval = bit_nil;
		else
			ret->val.btval = ISZERO(v->val.dval);
		break;
	default:
		GDKerror("VARcalciszero: bad input type %s.\n",
			 ATOMname(v->vtype));
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* sign of value (-1 for negative, 0 for 0, +1 for positive; any
 * numeric type) */

#define SIGN(x)		((bte) ((x) < 0 ? -1 : (x) > 0))

BAT *
BATcalcsign(BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils = 0;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcsign", NULL);
	if (checkbats(b, NULL, "BATcalcsign") != GDK_SUCCEED)
		return NULL;
	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, TYPE_bte, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		UNARY_2TYPE_FUNC(bte, bte, SIGN);
		break;
	case TYPE_sht:
		UNARY_2TYPE_FUNC(sht, bte, SIGN);
		break;
	case TYPE_int:
		UNARY_2TYPE_FUNC(int, bte, SIGN);
		break;
	case TYPE_lng:
		UNARY_2TYPE_FUNC(lng, bte, SIGN);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		UNARY_2TYPE_FUNC(hge, bte, SIGN);
		break;
#endif
	case TYPE_flt:
		UNARY_2TYPE_FUNC(flt, bte, SIGN);
		break;
	case TYPE_dbl:
		UNARY_2TYPE_FUNC(dbl, bte, SIGN);
		break;
	default:
		BBPunfix(bn->batCacheid);
		GDKerror("BATcalcsign: bad input type %s.\n",
			 ATOMname(b->T->type));
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* SIGN is ordered if the input is ordered (negative comes
	 * first, positive comes after) and NILs stay in the same
	 * position */
	bn->T->sorted = b->T->sorted || cnt <= 1 || nils == cnt;
	bn->T->revsorted = b->T->revsorted || cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	if (nils != 0 && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

gdk_return
VARcalcsign(ValPtr ret, const ValRecord *v)
{
	ret->vtype = TYPE_bte;
	switch (ATOMbasetype(v->vtype)) {
	case TYPE_bte:
		if (v->val.btval == bte_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.btval);
		break;
	case TYPE_sht:
		if (v->val.shval == sht_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.shval);
		break;
	case TYPE_int:
		if (v->val.ival == int_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.ival);
		break;
	case TYPE_lng:
		if (v->val.lval == lng_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.lval);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (v->val.hval == hge_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.hval);
		break;
#endif
	case TYPE_flt:
		if (v->val.fval == flt_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.fval);
		break;
	case TYPE_dbl:
		if (v->val.dval == dbl_nil)
			ret->val.btval = bte_nil;
		else
			ret->val.btval = SIGN(v->val.dval);
		break;
	default:
		GDKerror("VARcalcsign: bad input type %s.\n",
			 ATOMname(v->vtype));
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* is the value nil (any type) */

#define ISNIL_TYPE(TYPE, NOTNIL)					\
	do {								\
		const TYPE *restrict src = (const TYPE *) Tloc(b, b->batFirst);	\
		for (i = start; i < end; i++) {				\
			CHECKCAND(dst, i, b->H->seq, bit_nil);		\
			dst[i] = (bit) ((src[i] == TYPE##_nil) ^ NOTNIL); \
		}							\
	} while (0)

static BAT *
BATcalcisnil_implementation(BAT *b, BAT *s, int notnil)
{
	BAT *bn;
	BUN i, cnt, start, end;
	const oid *restrict cand = NULL, *candend = NULL;
	bit *restrict dst;
	BUN nils = 0;

	BATcheck(b, "BATcalcisnil", NULL);

	CANDINIT(b, s, start, end, cnt, cand, candend);

	if (start == 0 && end == cnt && cand == NULL) {
		if (b->T->nonil ||
		    (b->T->type == TYPE_void && b->T->seq != oid_nil)) {
			bit zero = 0;

			bn = BATconstant(TYPE_bit, &zero, cnt, TRANSIENT);
			BATseqbase(bn, b->H->seq);
			return bn;
		} else if (b->T->type == TYPE_void && b->T->seq == oid_nil) {
			bit one = 1;

			bn = BATconstant(TYPE_bit, &one, cnt, TRANSIENT);
			BATseqbase(bn, b->H->seq);
			return bn;
		}
	}

	bn = BATnew(TYPE_void, TYPE_bit, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	dst = (bit *) Tloc(bn, bn->batFirst);

	CANDLOOP(dst, i, bit_nil, 0, start);

	switch (ATOMbasetype(b->T->type)) {
	case TYPE_bte:
		ISNIL_TYPE(bte, notnil);
		break;
	case TYPE_sht:
		ISNIL_TYPE(sht, notnil);
		break;
	case TYPE_int:
		ISNIL_TYPE(int, notnil);
		break;
	case TYPE_lng:
		ISNIL_TYPE(lng, notnil);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ISNIL_TYPE(hge, notnil);
		break;
#endif
	case TYPE_flt:
		ISNIL_TYPE(flt, notnil);
		break;
	case TYPE_dbl:
		ISNIL_TYPE(dbl, notnil);
		break;
	default:
	{
		BATiter bi = bat_iterator(b);
		int (*atomcmp)(const void *, const void *) = ATOMcompare(b->T->type);
		const void *nil = ATOMnilptr(b->T->type);

		for (i = start; i < end; i++) {
			CHECKCAND(dst, i, b->H->seq, bit_nil);
			dst[i] = (bit) (((*atomcmp)(BUNtail(bi, i + BUNfirst(b)), nil) == 0) ^ notnil);
		}
		break;
	}
	}
	CANDLOOP(dst, i, bit_nil, end, cnt);

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* If b sorted, all nils are at the start, i.e. bn starts with
	 * 1's and ends with 0's, hence bn is revsorted.  Similarly
	 * for revsorted.  This reasoning breaks down if there is a
	 * candidate list. */
	bn->T->sorted = s == NULL && b->T->revsorted;
	bn->T->revsorted = s == NULL && b->T->sorted;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;
	bn->T->key = cnt <= 1;

	return bn;
}

BAT *
BATcalcisnil(BAT *b, BAT *s)
{
	return BATcalcisnil_implementation(b, s, 0);
}

BAT *
BATcalcisnotnil(BAT *b, BAT *s)
{
	return BATcalcisnil_implementation(b, s, 1);
}

gdk_return
VARcalcisnil(ValPtr ret, const ValRecord *v)
{
	ret->vtype = TYPE_bit;
	ret->val.btval = (bit) VALisnil(v);
	return GDK_SUCCEED;
}

gdk_return
VARcalcisnotnil(ValPtr ret, const ValRecord *v)
{
	ret->vtype = TYPE_bit;
	ret->val.btval = (bit) !VALisnil(v);
	return GDK_SUCCEED;
}

BAT *
BATcalcmin(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	BUN i;
	const oid *restrict cand = NULL, *candend = NULL;
	const void *restrict nil;
	const void *p1, *p2;
	BATiter b1i, b2i;
	int (*cmp)(const void *, const void *);

	BATcheck(b1, "BATcalcmin", NULL);
	BATcheck(b2, "BATcalcmin", NULL);

	if (checkbats(b1, b2, "BATcalcmin") != GDK_SUCCEED)
		return NULL;
	if (ATOMtype(b1->ttype) != ATOMtype(b2->ttype)) {
		GDKerror("BATcalcmin: inputs have incompatible types\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->ttype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;
	nil = ATOMnilptr(b1->ttype);
	cmp = ATOMcompare(b1->ttype);
	b1i = bat_iterator(b1);
	b2i = bat_iterator(b2);

	for (i = 0; i < start; i++)
		bunfastapp(bn, nil);
	nils = start;
	for (i = start; i < end; i++) {
		if (cand) {
			if (i < *cand - b1->hseqbase) {
				nils++;
				bunfastapp(bn, nil);
				continue;
			}
			assert(i == *cand - b1->hseqbase);
			if (++cand == candend)
				end = i + 1;
		}
		p1 = BUNtail(b1i, i + BUNfirst(b1));
		p2 = BUNtail(b2i, i + BUNfirst(b2));
		if (cmp(p1, nil) == 0 || cmp(p2, nil) == 0) {
			nils++;
			p1 = nil;
		} else if (cmp(p1, p2) > 0) {
			p1 = p2;
		}
		bunfastapp(bn, p1);
	}
	for (i = end; i < cnt; i++)
		bunfastapp(bn, nil);
	nils += cnt - end;
	BATseqbase(bn, b1->hseqbase);
	bn->T->nil = nils > 0;
	bn->T->nonil = nils == 0;
	if (cnt <= 1) {
		bn->tsorted = 1;
		bn->trevsorted = 1;
		bn->tkey = 1;
		bn->tdense = ATOMtype(b1->ttype) == TYPE_oid;
		if (bn->tdense)
			bn->tseqbase = cnt == 1 ? *(oid*)Tloc(bn,BUNfirst(bn)) : 0;
	} else {
		bn->tsorted = 0;
		bn->trevsorted = 0;
		bn->tkey = 0;
		bn->tdense = 0;
	}
	return bn;
  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

BAT *
BATcalcmin_no_nil(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	BUN i;
	const oid *restrict cand = NULL, *candend = NULL;
	const void *restrict nil;
	const void *p1, *p2;
	BATiter b1i, b2i;
	int (*cmp)(const void *, const void *);

	BATcheck(b1, "BATcalcmin_no_nil", NULL);
	BATcheck(b2, "BATcalcmin_no_nil", NULL);

	if (checkbats(b1, b2, "BATcalcmin_no_nil") != GDK_SUCCEED)
		return NULL;
	if (ATOMtype(b1->ttype) != ATOMtype(b2->ttype)) {
		GDKerror("BATcalcmin_no_nil: inputs have incompatible types\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->ttype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;
	nil = ATOMnilptr(b1->ttype);
	cmp = ATOMcompare(b1->ttype);
	b1i = bat_iterator(b1);
	b2i = bat_iterator(b2);

	for (i = 0; i < start; i++)
		bunfastapp(bn, nil);
	nils = start;
	for (i = start; i < end; i++) {
		if (cand) {
			if (i < *cand - b1->hseqbase) {
				nils++;
				bunfastapp(bn, nil);
				continue;
			}
			assert(i == *cand - b1->hseqbase);
			if (++cand == candend)
				end = i + 1;
		}
		p1 = BUNtail(b1i, i + BUNfirst(b1));
		p2 = BUNtail(b2i, i + BUNfirst(b2));
		if (cmp(p1, nil) == 0) {
			if (cmp(p2, nil) == 0) {
				/* both values are nil */
				nils++;
			} else {
				p1 = p2;
			}
		} else if (cmp(p2, nil) != 0 && cmp(p1, p2) > 0) {
			p1 = p2;
		}
		bunfastapp(bn, p1);
	}
	for (i = end; i < cnt; i++)
		bunfastapp(bn, nil);
	nils += cnt - end;
	BATseqbase(bn, b1->hseqbase);
	bn->T->nil = nils > 0;
	bn->T->nonil = nils == 0;
	if (cnt <= 1) {
		bn->tsorted = 1;
		bn->trevsorted = 1;
		bn->tkey = 1;
		bn->tdense = ATOMtype(b1->ttype) == TYPE_oid;
		if (bn->tdense)
			bn->tseqbase = cnt == 1 ? *(oid*)Tloc(bn,BUNfirst(bn)) : 0;
	} else {
		bn->tsorted = 0;
		bn->trevsorted = 0;
		bn->tkey = 0;
		bn->tdense = 0;
	}
	return bn;
  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

BAT *
BATcalcmax(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	BUN i;
	const oid *restrict cand = NULL, *candend = NULL;
	const void *restrict nil;
	const void *p1, *p2;
	BATiter b1i, b2i;
	int (*cmp)(const void *, const void *);

	BATcheck(b1, "BATcalcmax", NULL);
	BATcheck(b2, "BATcalcmax", NULL);

	if (checkbats(b1, b2, "BATcalcmax") != GDK_SUCCEED)
		return NULL;
	if (ATOMtype(b1->ttype) != ATOMtype(b2->ttype)) {
		GDKerror("BATcalcmax: inputs have incompatible types\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->ttype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;
	nil = ATOMnilptr(b1->ttype);
	cmp = ATOMcompare(b1->ttype);
	b1i = bat_iterator(b1);
	b2i = bat_iterator(b2);

	for (i = 0; i < start; i++)
		bunfastapp(bn, nil);
	nils = start;
	for (i = start; i < end; i++) {
		if (cand) {
			if (i < *cand - b1->hseqbase) {
				nils++;
				bunfastapp(bn, nil);
				continue;
			}
			assert(i == *cand - b1->hseqbase);
			if (++cand == candend)
				end = i + 1;
		}
		p1 = BUNtail(b1i, i + BUNfirst(b1));
		p2 = BUNtail(b2i, i + BUNfirst(b2));
		if (cmp(p1, nil) == 0 || cmp(p2, nil) == 0) {
			nils++;
			p1 = nil;
		} else if (cmp(p1, p2) < 0) {
			p1 = p2;
		}
		bunfastapp(bn, p1);
	}
	for (i = end; i < cnt; i++)
		bunfastapp(bn, nil);
	nils += cnt - end;
	BATseqbase(bn, b1->hseqbase);
	bn->T->nil = nils > 0;
	bn->T->nonil = nils == 0;
	if (cnt <= 1) {
		bn->tsorted = 1;
		bn->trevsorted = 1;
		bn->tkey = 1;
		bn->tdense = ATOMtype(b1->ttype) == TYPE_oid;
		if (bn->tdense)
			bn->tseqbase = cnt == 1 ? *(oid*)Tloc(bn,BUNfirst(bn)) : 0;
	} else {
		bn->tsorted = 0;
		bn->trevsorted = 0;
		bn->tkey = 0;
		bn->tdense = 0;
	}
	return bn;
  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

BAT *
BATcalcmax_no_nil(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	BUN i;
	const oid *restrict cand = NULL, *candend = NULL;
	const void *restrict nil;
	const void *p1, *p2;
	BATiter b1i, b2i;
	int (*cmp)(const void *, const void *);

	BATcheck(b1, "BATcalcmax_no_nil", NULL);
	BATcheck(b2, "BATcalcmax_no_nil", NULL);

	if (checkbats(b1, b2, "BATcalcmax_no_nil") != GDK_SUCCEED)
		return NULL;
	if (ATOMtype(b1->ttype) != ATOMtype(b2->ttype)) {
		GDKerror("BATcalcmax_no_nil: inputs have incompatible types\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->ttype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;
	nil = ATOMnilptr(b1->ttype);
	cmp = ATOMcompare(b1->ttype);
	b1i = bat_iterator(b1);
	b2i = bat_iterator(b2);

	for (i = 0; i < start; i++)
		bunfastapp(bn, nil);
	nils = start;
	for (i = start; i < end; i++) {
		if (cand) {
			if (i < *cand - b1->hseqbase) {
				nils++;
				bunfastapp(bn, nil);
				continue;
			}
			assert(i == *cand - b1->hseqbase);
			if (++cand == candend)
				end = i + 1;
		}
		p1 = BUNtail(b1i, i + BUNfirst(b1));
		p2 = BUNtail(b2i, i + BUNfirst(b2));
		if (cmp(p1, nil) == 0) {
			if (cmp(p2, nil) == 0) {
				/* both values are nil */
				nils++;
			} else {
				p1 = p2;
			}
		} else if (cmp(p2, nil) != 0 && cmp(p1, p2) < 0) {
			p1 = p2;
		}
		bunfastapp(bn, p1);
	}
	for (i = end; i < cnt; i++)
		bunfastapp(bn, nil);
	nils += cnt - end;
	BATseqbase(bn, b1->hseqbase);
	bn->T->nil = nils > 0;
	bn->T->nonil = nils == 0;
	if (cnt <= 1) {
		bn->tsorted = 1;
		bn->trevsorted = 1;
		bn->tkey = 1;
		bn->tdense = ATOMtype(b1->ttype) == TYPE_oid;
		if (bn->tdense)
			bn->tseqbase = cnt == 1 ? *(oid*)Tloc(bn,BUNfirst(bn)) : 0;
	} else {
		bn->tsorted = 0;
		bn->trevsorted = 0;
		bn->tkey = 0;
		bn->tdense = 0;
	}
	return bn;
  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* addition (any numeric type) */

#define ON_OVERFLOW(TYPE1, TYPE2, OP)				\
	do {							\
		GDKerror("22003!overflow in calculation "	\
			 FMT##TYPE1 OP FMT##TYPE2 ".\n",	\
			 CST##TYPE1 lft[i], CST##TYPE2 rgt[j]);	\
		return BUN_NONE;				\
	} while (0)

#define ADD_3TYPE(TYPE1, TYPE2, TYPE3)					\
static BUN								\
add_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			ADD_WITH_CHECK(TYPE1, lft[i],			\
				       TYPE2, rgt[j],			\
				       TYPE3, dst[k],			\
				       ON_OVERFLOW(TYPE1, TYPE2, "+"));	\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#define ADD_3TYPE_enlarge(TYPE1, TYPE2, TYPE3)				\
static BUN								\
add_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff)	\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) lft[i] + rgt[j];		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

ADD_3TYPE(bte, bte, bte)
ADD_3TYPE_enlarge(bte, bte, sht)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(bte, bte, int)
ADD_3TYPE_enlarge(bte, bte, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(bte, bte, hge)
#endif
ADD_3TYPE_enlarge(bte, bte, flt)
ADD_3TYPE_enlarge(bte, bte, dbl)
#endif
ADD_3TYPE(bte, sht, sht)
ADD_3TYPE_enlarge(bte, sht, int)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(bte, sht, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(bte, sht, hge)
#endif
ADD_3TYPE_enlarge(bte, sht, flt)
ADD_3TYPE_enlarge(bte, sht, dbl)
#endif
ADD_3TYPE(bte, int, int)
ADD_3TYPE_enlarge(bte, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(bte, int, hge)
#endif
ADD_3TYPE_enlarge(bte, int, flt)
ADD_3TYPE_enlarge(bte, int, dbl)
#endif
ADD_3TYPE(bte, lng, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(bte, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(bte, lng, flt)
ADD_3TYPE_enlarge(bte, lng, dbl)
#endif
#ifdef HAVE_HGE
ADD_3TYPE(bte, hge, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(bte, hge, flt)
ADD_3TYPE_enlarge(bte, hge, dbl)
#endif
#endif
ADD_3TYPE(bte, flt, flt)
ADD_3TYPE_enlarge(bte, flt, dbl)
ADD_3TYPE(bte, dbl, dbl)
ADD_3TYPE(sht, bte, sht)
ADD_3TYPE_enlarge(sht, bte, int)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(sht, bte, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(sht, bte, hge)
#endif
ADD_3TYPE_enlarge(sht, bte, flt)
ADD_3TYPE_enlarge(sht, bte, dbl)
#endif
ADD_3TYPE(sht, sht, sht)
ADD_3TYPE_enlarge(sht, sht, int)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(sht, sht, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(sht, sht, hge)
#endif
ADD_3TYPE_enlarge(sht, sht, flt)
ADD_3TYPE_enlarge(sht, sht, dbl)
#endif
ADD_3TYPE(sht, int, int)
ADD_3TYPE_enlarge(sht, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(sht, int, hge)
#endif
ADD_3TYPE_enlarge(sht, int, flt)
ADD_3TYPE_enlarge(sht, int, dbl)
#endif
ADD_3TYPE(sht, lng, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(sht, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(sht, lng, flt)
ADD_3TYPE_enlarge(sht, lng, dbl)
#endif
#ifdef HAVE_HGE
ADD_3TYPE(sht, hge, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(sht, hge, flt)
ADD_3TYPE_enlarge(sht, hge, dbl)
#endif
#endif
ADD_3TYPE(sht, flt, flt)
ADD_3TYPE_enlarge(sht, flt, dbl)
ADD_3TYPE(sht, dbl, dbl)
ADD_3TYPE(int, bte, int)
ADD_3TYPE_enlarge(int, bte, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(int, bte, hge)
#endif
ADD_3TYPE_enlarge(int, bte, flt)
ADD_3TYPE_enlarge(int, bte, dbl)
#endif
ADD_3TYPE(int, sht, int)
ADD_3TYPE_enlarge(int, sht, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(int, sht, hge)
#endif
ADD_3TYPE_enlarge(int, sht, flt)
ADD_3TYPE_enlarge(int, sht, dbl)
#endif
ADD_3TYPE(int, int, int)
ADD_3TYPE_enlarge(int, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(int, int, hge)
#endif
ADD_3TYPE_enlarge(int, int, flt)
ADD_3TYPE_enlarge(int, int, dbl)
#endif
ADD_3TYPE(int, lng, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(int, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(int, lng, flt)
ADD_3TYPE_enlarge(int, lng, dbl)
#endif
#ifdef HAVE_HGE
ADD_3TYPE(int, hge, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(int, hge, flt)
ADD_3TYPE_enlarge(int, hge, dbl)
#endif
#endif
ADD_3TYPE(int, flt, flt)
ADD_3TYPE_enlarge(int, flt, dbl)
ADD_3TYPE(int, dbl, dbl)
ADD_3TYPE(lng, bte, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(lng, bte, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(lng, bte, flt)
ADD_3TYPE_enlarge(lng, bte, dbl)
#endif
ADD_3TYPE(lng, sht, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(lng, sht, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(lng, sht, flt)
ADD_3TYPE_enlarge(lng, sht, dbl)
#endif
ADD_3TYPE(lng, int, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(lng, int, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(lng, int, flt)
ADD_3TYPE_enlarge(lng, int, dbl)
#endif
ADD_3TYPE(lng, lng, lng)
#ifdef HAVE_HGE
ADD_3TYPE_enlarge(lng, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(lng, lng, flt)
ADD_3TYPE_enlarge(lng, lng, dbl)
#endif
#ifdef HAVE_HGE
ADD_3TYPE(lng, hge, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(lng, hge, flt)
ADD_3TYPE_enlarge(lng, hge, dbl)
#endif
#endif
ADD_3TYPE(lng, flt, flt)
ADD_3TYPE_enlarge(lng, flt, dbl)
ADD_3TYPE(lng, dbl, dbl)
#ifdef HAVE_HGE
ADD_3TYPE(hge, bte, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(hge, bte, flt)
ADD_3TYPE_enlarge(hge, bte, dbl)
#endif
ADD_3TYPE(hge, sht, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(hge, sht, flt)
ADD_3TYPE_enlarge(hge, sht, dbl)
#endif
ADD_3TYPE(hge, int, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(hge, int, flt)
ADD_3TYPE_enlarge(hge, int, dbl)
#endif
ADD_3TYPE(hge, lng, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(hge, lng, flt)
ADD_3TYPE_enlarge(hge, lng, dbl)
#endif
ADD_3TYPE(hge, hge, hge)
#ifdef FULL_IMPLEMENTATION
ADD_3TYPE_enlarge(hge, hge, flt)
ADD_3TYPE_enlarge(hge, hge, dbl)
#endif
ADD_3TYPE(hge, flt, flt)
ADD_3TYPE_enlarge(hge, flt, dbl)
ADD_3TYPE(hge, dbl, dbl)
#endif
ADD_3TYPE(flt, bte, flt)
ADD_3TYPE_enlarge(flt, bte, dbl)
ADD_3TYPE(flt, sht, flt)
ADD_3TYPE_enlarge(flt, sht, dbl)
ADD_3TYPE(flt, int, flt)
ADD_3TYPE_enlarge(flt, int, dbl)
ADD_3TYPE(flt, lng, flt)
ADD_3TYPE_enlarge(flt, lng, dbl)
#ifdef HAVE_HGE
ADD_3TYPE(flt, hge, flt)
ADD_3TYPE_enlarge(flt, hge, dbl)
#endif
ADD_3TYPE(flt, flt, flt)
ADD_3TYPE_enlarge(flt, flt, dbl)
ADD_3TYPE(flt, dbl, dbl)
ADD_3TYPE(dbl, bte, dbl)
ADD_3TYPE(dbl, sht, dbl)
ADD_3TYPE(dbl, int, dbl)
ADD_3TYPE(dbl, lng, dbl)
#ifdef HAVE_HGE
ADD_3TYPE(dbl, hge, dbl)
#endif
ADD_3TYPE(dbl, flt, dbl)
ADD_3TYPE(dbl, dbl, dbl)

static BUN
add_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	tp = ATOMbasetype(tp);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = add_bte_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_sht:
				nils = add_bte_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = add_bte_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_lng:
				nils = add_bte_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_bte_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_bte_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_bte_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = add_bte_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = add_bte_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = add_bte_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_bte_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_bte_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_bte_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = add_bte_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = add_bte_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_bte_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_bte_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_bte_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = add_bte_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_bte_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_bte_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_bte_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = add_bte_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_bte_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_bte_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_bte_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_bte_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_bte_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_sht:
				nils = add_sht_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = add_sht_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = add_sht_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_sht_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_sht_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_sht_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = add_sht_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = add_sht_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = add_sht_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_sht_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_sht_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_sht_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = add_sht_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = add_sht_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_sht_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_sht_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_sht_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = add_sht_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_sht_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_sht_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_sht_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = add_sht_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_sht_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_sht_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_sht_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_sht_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_sht_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_int:
				nils = add_int_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = add_int_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_int_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_int_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_int_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_int:
				nils = add_int_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = add_int_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_int_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_int_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_int_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = add_int_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = add_int_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_int_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = add_int_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_int_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = add_int_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_int_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_int_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_int_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = add_int_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_int_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_int_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_int_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_int_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_int_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_lng:
				nils = add_lng_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_lng_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_lng_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_lng_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_lng:
				nils = add_lng_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_lng_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_lng_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_lng_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_lng:
				nils = add_lng_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_lng_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_lng_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_lng_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = add_lng_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = add_lng_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_lng_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_lng_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = add_lng_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_lng_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_lng_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_lng_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_lng_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_lng_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_hge:
				nils = add_hge_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_hge_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_hge_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_hge:
				nils = add_hge_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_hge_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_hge_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_hge:
				nils = add_hge_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_hge_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_hge_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_hge:
				nils = add_hge_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_hge_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_hge_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = add_hge_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = add_hge_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = add_hge_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_hge_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_hge_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_hge_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	case TYPE_flt:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = add_flt_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = add_flt_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_flt_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_dbl:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = add_dbl_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	default:
		goto unsupported;
	}

	return nils;

  unsupported:
	GDKerror("%s: type combination (add(%s,%s)->%s) not supported.\n",
		 func, ATOMname(tp1), ATOMname(tp2), ATOMname(tp));
	return BUN_NONE;
}

static BUN
addstr_loop(BAT *b1, const char *l, BAT *b2, const char *r, BAT *bn,
	    BUN cnt, BUN start, BUN end, const oid *restrict cand, const oid *candend)
{
	BUN i, j, k, frst = BUNfirst(bn);
	BUN nils = start + (cnt - end);
	char *s;
	size_t slen, llen, rlen;
	BATiter b1i, b2i;
	oid candoff;

	assert(b1 != NULL || b2 != NULL); /* at least one not NULL */
	candoff = b1 ? b1->H->seq : b2->H->seq;
	b1i = bat_iterator(b1);
	b2i = bat_iterator(b2);
	slen = 1024;
	s = GDKmalloc(slen);
	if (s == NULL)
		goto bunins_failed;
	for (k = 0; k < start; k++)
		tfastins_nocheck(bn, k + frst, str_nil, Tsize(bn));
	for (i = start + (b1 ? BUNfirst(b1) : 0), j = start + (b2 ? BUNfirst(b2) : 0), k = start;
	     k < end; i++, j++, k++) {
		if (cand) {
			if (k < *cand - candoff) {
				nils++;
				tfastins_nocheck(bn, k + frst, str_nil, Tsize(bn));
				continue;
			}
			assert(k == *cand - candoff);
			if (++cand == candend)
				end = k + 1;
		}
		if (b1)
			l = BUNtvar(b1i, i);
		if (b2)
			r = BUNtvar(b2i, j);
		if (strcmp(l, str_nil) == 0 || strcmp(r, str_nil) == 0) {
			nils++;
			tfastins_nocheck(bn, k + frst, str_nil, Tsize(bn));
		} else {
			llen = strlen(l);
			rlen = strlen(r);
			if (llen + rlen >= slen) {
				slen = llen + rlen + 1024;
				GDKfree(s);
				s = GDKmalloc(slen);
				if (s == NULL)
					goto bunins_failed;
			}
#ifdef HAVE_STPCPY
			(void) stpcpy(stpcpy(s, l), r);
#else
			snprintf(s, slen, "%s%s", l, r);
#endif
			tfastins_nocheck(bn, k + frst, s, Tsize(bn));
		}
	}
	for (k = end; k < cnt; k++)
		tfastins_nocheck(bn, k + frst, str_nil, Tsize(bn));
	GDKfree(s);
	return nils;

  bunins_failed:
	GDKfree(s);
	return BUN_NONE;
}

BAT *
BATcalcadd(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcadd", NULL);
	BATcheck(b2, "BATcalcadd", NULL);

	if (checkbats(b1, b2, "BATcalcadd") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (b1->T->type == TYPE_str && b2->T->type == TYPE_str && tp == TYPE_str) {
		nils = addstr_loop(b1, NULL, b2, NULL, bn,
				   cnt, start, end, cand, candend);
	} else {
		nils = add_typeswitchloop(Tloc(b1, b1->batFirst),
					  b1->T->type, 1,
					  Tloc(b2, b2->batFirst),
					  b2->T->type, 1,
					  Tloc(bn, bn->batFirst), tp,
					  cnt, start, end,
					  cand, candend, b1->H->seq,
					  abort_on_error, "BATcalcadd");
	}

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	/* if both inputs are sorted the same way, and no overflow
	 * occurred (we only know for sure if abort_on_error is set),
	 * the result is also sorted */
	bn->T->sorted = (abort_on_error && b1->T->sorted & b2->T->sorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && b1->T->revsorted & b2->T->revsorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcaddcst(BAT *b, const ValRecord *v, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcaddcst", NULL);

	if (checkbats(b, NULL, "BATcalcaddcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (b->T->type == TYPE_str && v->vtype == TYPE_str && tp == TYPE_str) {
		nils = addstr_loop(b, NULL, NULL, v->val.sval, bn,
				   cnt, start, end, cand, candend);
	} else {
		nils = add_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
					  VALptr(v), v->vtype, 0,
					  Tloc(bn, bn->batFirst), tp,
					  cnt, start, end,
					  cand, candend, b->H->seq,
					  abort_on_error, "BATcalcaddcst");
	}

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted */
	bn->T->sorted = (abort_on_error && b->T->sorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && b->T->revsorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstadd(const ValRecord *v, BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstadd", NULL);

	if (checkbats(b, NULL, "BATcalccstadd") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (b->T->type == TYPE_str && v->vtype == TYPE_str && tp == TYPE_str) {
		nils = addstr_loop(NULL, v->val.sval, b, NULL, bn,
				   cnt, start, end, cand, candend);
	} else {
		nils = add_typeswitchloop(VALptr(v), v->vtype, 0,
					  Tloc(b, b->batFirst), b->T->type, 1,
					  Tloc(bn, bn->batFirst), tp,
					  cnt, start, end,
					  cand, candend, b->H->seq,
					  abort_on_error, "BATcalccstadd");
	}

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted */
	bn->T->sorted = (abort_on_error && b->T->sorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && b->T->revsorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcadd(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	if (add_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcadd") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

static BAT *
BATcalcincrdecr(BAT *b, BAT *s, int abort_on_error,
		BUN (*typeswitchloop)(const void *, int, int, const void *,
				      int, int, void *, int, BUN, BUN, BUN,
				      const oid *restrict, const oid *, oid, int,
				      const char *),
		const char *func)
{
	BAT *bn;
	BUN nils= 0;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;
	bte one = 1;

	BATcheck(b, func, NULL);
	if (checkbats(b, NULL, func) != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = (*typeswitchloop)(Tloc(b, b->batFirst), b->T->type, 1,
				 &one, TYPE_bte, 0,
				 Tloc(bn, bn->batFirst), bn->T->type,
				 cnt, start, end,
				 cand, candend, b->H->seq,
				 abort_on_error, func);

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted */
	bn->T->sorted = (abort_on_error && b->T->sorted) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && b->T->revsorted) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	if (nils && !b->T->nil) {
		b->T->nil = 1;
		b->batDirtydesc = 1;
	}
	if (nils == 0 && !b->T->nonil) {
		b->T->nonil = 1;
		b->batDirtydesc = 1;
	}

	return bn;
}

BAT *
BATcalcincr(BAT *b, BAT *s, int abort_on_error)
{
	return BATcalcincrdecr(b, s, abort_on_error, add_typeswitchloop,
			       "BATcalcincr");
}

gdk_return
VARcalcincr(ValPtr ret, const ValRecord *v, int abort_on_error)
{
	bte one = 1;

	if (add_typeswitchloop(VALptr(v), v->vtype, 0,
			       &one, TYPE_bte, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcincr") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* subtraction (any numeric type) */

#define SUB_3TYPE(TYPE1, TYPE2, TYPE3)					\
static BUN								\
sub_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			SUB_WITH_CHECK(TYPE1, lft[i],			\
				       TYPE2, rgt[j],			\
				       TYPE3, dst[k],			\
				       ON_OVERFLOW(TYPE1, TYPE2, "-"));	\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#define SUB_3TYPE_enlarge(TYPE1, TYPE2, TYPE3)				\
static BUN								\
sub_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff)	\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) lft[i] - rgt[j];		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

SUB_3TYPE(bte, bte, bte)
SUB_3TYPE_enlarge(bte, bte, sht)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(bte, bte, int)
SUB_3TYPE_enlarge(bte, bte, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(bte, bte, hge)
#endif
SUB_3TYPE_enlarge(bte, bte, flt)
SUB_3TYPE_enlarge(bte, bte, dbl)
#endif
SUB_3TYPE(bte, sht, sht)
SUB_3TYPE_enlarge(bte, sht, int)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(bte, sht, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(bte, sht, hge)
#endif
SUB_3TYPE_enlarge(bte, sht, flt)
SUB_3TYPE_enlarge(bte, sht, dbl)
#endif
SUB_3TYPE(bte, int, int)
SUB_3TYPE_enlarge(bte, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(bte, int, hge)
#endif
SUB_3TYPE_enlarge(bte, int, flt)
SUB_3TYPE_enlarge(bte, int, dbl)
#endif
SUB_3TYPE(bte, lng, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(bte, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(bte, lng, flt)
SUB_3TYPE_enlarge(bte, lng, dbl)
#endif
#ifdef HAVE_HGE
SUB_3TYPE(bte, hge, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(bte, hge, flt)
SUB_3TYPE_enlarge(bte, hge, dbl)
#endif
#endif
SUB_3TYPE(bte, flt, flt)
SUB_3TYPE_enlarge(bte, flt, dbl)
SUB_3TYPE(bte, dbl, dbl)
SUB_3TYPE(sht, bte, sht)
SUB_3TYPE_enlarge(sht, bte, int)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(sht, bte, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(sht, bte, hge)
#endif
SUB_3TYPE_enlarge(sht, bte, flt)
SUB_3TYPE_enlarge(sht, bte, dbl)
#endif
SUB_3TYPE(sht, sht, sht)
SUB_3TYPE_enlarge(sht, sht, int)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(sht, sht, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(sht, sht, hge)
#endif
SUB_3TYPE_enlarge(sht, sht, flt)
SUB_3TYPE_enlarge(sht, sht, dbl)
#endif
SUB_3TYPE(sht, int, int)
SUB_3TYPE_enlarge(sht, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(sht, int, hge)
#endif
SUB_3TYPE_enlarge(sht, int, flt)
SUB_3TYPE_enlarge(sht, int, dbl)
#endif
SUB_3TYPE(sht, lng, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(sht, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(sht, lng, flt)
SUB_3TYPE_enlarge(sht, lng, dbl)
#endif
#ifdef HAVE_HGE
SUB_3TYPE(sht, hge, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(sht, hge, flt)
SUB_3TYPE_enlarge(sht, hge, dbl)
#endif
#endif
SUB_3TYPE(sht, flt, flt)
SUB_3TYPE_enlarge(sht, flt, dbl)
SUB_3TYPE(sht, dbl, dbl)
SUB_3TYPE(int, bte, int)
SUB_3TYPE_enlarge(int, bte, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(int, bte, hge)
#endif
SUB_3TYPE_enlarge(int, bte, flt)
SUB_3TYPE_enlarge(int, bte, dbl)
#endif
SUB_3TYPE(int, sht, int)
SUB_3TYPE_enlarge(int, sht, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(int, sht, hge)
#endif
SUB_3TYPE_enlarge(int, sht, flt)
SUB_3TYPE_enlarge(int, sht, dbl)
#endif
SUB_3TYPE(int, int, int)
SUB_3TYPE_enlarge(int, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(int, int, hge)
#endif
SUB_3TYPE_enlarge(int, int, flt)
SUB_3TYPE_enlarge(int, int, dbl)
#endif
SUB_3TYPE(int, lng, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(int, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(int, lng, flt)
SUB_3TYPE_enlarge(int, lng, dbl)
#endif
#ifdef HAVE_HGE
SUB_3TYPE(int, hge, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(int, hge, flt)
SUB_3TYPE_enlarge(int, hge, dbl)
#endif
#endif
SUB_3TYPE(int, flt, flt)
SUB_3TYPE_enlarge(int, flt, dbl)
SUB_3TYPE(int, dbl, dbl)
SUB_3TYPE(lng, bte, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(lng, bte, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(lng, bte, flt)
SUB_3TYPE_enlarge(lng, bte, dbl)
#endif
SUB_3TYPE(lng, sht, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(lng, sht, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(lng, sht, flt)
SUB_3TYPE_enlarge(lng, sht, dbl)
#endif
SUB_3TYPE(lng, int, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(lng, int, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(lng, int, flt)
SUB_3TYPE_enlarge(lng, int, dbl)
#endif
SUB_3TYPE(lng, lng, lng)
#ifdef HAVE_HGE
SUB_3TYPE_enlarge(lng, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(lng, lng, flt)
SUB_3TYPE_enlarge(lng, lng, dbl)
#endif
#ifdef HAVE_HGE
SUB_3TYPE(lng, hge, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(lng, hge, flt)
SUB_3TYPE_enlarge(lng, hge, dbl)
#endif
#endif
SUB_3TYPE(lng, flt, flt)
SUB_3TYPE_enlarge(lng, flt, dbl)
SUB_3TYPE(lng, dbl, dbl)
#ifdef HAVE_HGE
SUB_3TYPE(hge, bte, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(hge, bte, flt)
SUB_3TYPE_enlarge(hge, bte, dbl)
#endif
SUB_3TYPE(hge, sht, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(hge, sht, flt)
SUB_3TYPE_enlarge(hge, sht, dbl)
#endif
SUB_3TYPE(hge, int, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(hge, int, flt)
SUB_3TYPE_enlarge(hge, int, dbl)
#endif
SUB_3TYPE(hge, lng, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(hge, lng, flt)
SUB_3TYPE_enlarge(hge, lng, dbl)
#endif
SUB_3TYPE(hge, hge, hge)
#ifdef FULL_IMPLEMENTATION
SUB_3TYPE_enlarge(hge, hge, flt)
SUB_3TYPE_enlarge(hge, hge, dbl)
#endif
SUB_3TYPE(hge, flt, flt)
SUB_3TYPE_enlarge(hge, flt, dbl)
SUB_3TYPE(hge, dbl, dbl)
#endif
SUB_3TYPE(flt, bte, flt)
SUB_3TYPE_enlarge(flt, bte, dbl)
SUB_3TYPE(flt, sht, flt)
SUB_3TYPE_enlarge(flt, sht, dbl)
SUB_3TYPE(flt, int, flt)
SUB_3TYPE_enlarge(flt, int, dbl)
SUB_3TYPE(flt, lng, flt)
SUB_3TYPE_enlarge(flt, lng, dbl)
#ifdef HAVE_HGE
SUB_3TYPE(flt, hge, flt)
SUB_3TYPE_enlarge(flt, hge, dbl)
#endif
SUB_3TYPE(flt, flt, flt)
SUB_3TYPE_enlarge(flt, flt, dbl)
SUB_3TYPE(flt, dbl, dbl)
SUB_3TYPE(dbl, bte, dbl)
SUB_3TYPE(dbl, sht, dbl)
SUB_3TYPE(dbl, int, dbl)
SUB_3TYPE(dbl, lng, dbl)
#ifdef HAVE_HGE
SUB_3TYPE(dbl, hge, dbl)
#endif
SUB_3TYPE(dbl, flt, dbl)
SUB_3TYPE(dbl, dbl, dbl)

static BUN
sub_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	tp = ATOMbasetype(tp);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = sub_bte_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_sht:
				nils = sub_bte_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = sub_bte_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_lng:
				nils = sub_bte_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_bte_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_bte_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_bte_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = sub_bte_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = sub_bte_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = sub_bte_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_bte_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_bte_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_bte_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = sub_bte_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = sub_bte_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_bte_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_bte_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_bte_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = sub_bte_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_bte_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_bte_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_bte_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = sub_bte_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_bte_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_bte_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_bte_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_bte_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_bte_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_sht:
				nils = sub_sht_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = sub_sht_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = sub_sht_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_sht_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_sht_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_sht_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = sub_sht_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = sub_sht_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = sub_sht_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_sht_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_sht_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_sht_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = sub_sht_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = sub_sht_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_sht_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_sht_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_sht_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = sub_sht_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_sht_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_sht_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_sht_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = sub_sht_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_sht_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_sht_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_sht_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_sht_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_sht_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_int:
				nils = sub_int_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = sub_int_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_int_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_int_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_int_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_int:
				nils = sub_int_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = sub_int_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_int_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_int_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_int_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = sub_int_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = sub_int_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_int_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = sub_int_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_int_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = sub_int_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_int_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_int_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_int_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = sub_int_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_int_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_int_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_int_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_int_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_int_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_lng:
				nils = sub_lng_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_lng_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_lng_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_lng_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_lng:
				nils = sub_lng_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_lng_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_lng_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_lng_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_lng:
				nils = sub_lng_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_lng_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_lng_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_lng_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = sub_lng_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = sub_lng_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_lng_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_lng_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = sub_lng_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_lng_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_lng_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_lng_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_lng_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_lng_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_hge:
				nils = sub_hge_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_hge_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_hge_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_hge:
				nils = sub_hge_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_hge_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_hge_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_hge:
				nils = sub_hge_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_hge_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_hge_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_hge:
				nils = sub_hge_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_hge_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_hge_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = sub_hge_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = sub_hge_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = sub_hge_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_hge_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_hge_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_hge_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	case TYPE_flt:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = sub_flt_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = sub_flt_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_flt_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_dbl:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = sub_dbl_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	default:
		goto unsupported;
	}

	return nils;

  unsupported:
	GDKerror("%s: type combination (sub(%s,%s)->%s) not supported.\n",
		 func, ATOMname(tp1), ATOMname(tp2), ATOMname(tp));
	return BUN_NONE;
}

BAT *
BATcalcsub(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcsub", NULL);
	BATcheck(b2, "BATcalcsub", NULL);

	if (checkbats(b1, b2, "BATcalcsub") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = sub_typeswitchloop(Tloc(b1, b1->batFirst), b1->T->type, 1,
				  Tloc(b2, b2->batFirst), b2->T->type, 1,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b1->H->seq,
				  abort_on_error, "BATcalcsub");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcsubcst(BAT *b, const ValRecord *v, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcsubcst", NULL);

	if (checkbats(b, NULL, "BATcalcsubcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = sub_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalcsubcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted */
	bn->T->sorted = (abort_on_error && b->T->sorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && b->T->revsorted && nils == 0) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstsub(const ValRecord *v, BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstsub", NULL);

	if (checkbats(b, NULL, "BATcalccstsub") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = sub_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstsub");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is
	 * sorted in the opposite direction (except that NILs mess
	 * things up */
	bn->T->sorted = (abort_on_error && nils == 0 && b->T->revsorted) ||
		cnt <= 1 || nils == cnt;
	bn->T->revsorted = (abort_on_error && nils == 0 && b->T->sorted) ||
		cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcsub(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	if (sub_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcsub") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

BAT *
BATcalcdecr(BAT *b, BAT *s, int abort_on_error)
{
	return BATcalcincrdecr(b, s, abort_on_error, sub_typeswitchloop,
			       "BATcalcdecr");
}

gdk_return
VARcalcdecr(ValPtr ret, const ValRecord *v, int abort_on_error)
{
	bte one = 1;

	if (sub_typeswitchloop(VALptr(v), v->vtype, 0,
			       &one, TYPE_bte, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcdecr") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* multiplication (any numeric type) */

/* TYPE4 must be a type larger than both TYPE1 and TYPE2 so that
 * multiplying into it doesn't cause overflow */
#define MUL_4TYPE(TYPE1, TYPE2, TYPE3, TYPE4)				\
static BUN								\
mul_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			MUL4_WITH_CHECK(TYPE1, lft[i],			\
					TYPE2, rgt[j],			\
					TYPE3, dst[k],			\
					TYPE4,				\
					ON_OVERFLOW(TYPE1, TYPE2, "*")); \
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#define MUL_3TYPE_enlarge(TYPE1, TYPE2, TYPE3)				\
static BUN								\
mul_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff)	\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) lft[i] * rgt[j];		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#ifdef HAVE_HGE

#define MUL_2TYPE_hge(TYPE1, TYPE2)					\
static BUN								\
mul_##TYPE1##_##TYPE2##_hge(const TYPE1 *lft, int incr1,		\
			    const TYPE2 *rgt, int incr2,		\
			    hge * restrict dst, BUN cnt, BUN start,	\
			    BUN end, const oid *restrict cand,		\
			    const oid *candend, oid candoff,		\
			    int abort_on_error)				\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, hge_nil, 0, start);				\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, hge_nil);			\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = hge_nil;				\
			nils++;						\
		} else {						\
			HGEMUL_CHECK(TYPE1, lft[i],			\
				     TYPE2, rgt[j],			\
				     dst[k],				\
				     ON_OVERFLOW(TYPE1, TYPE2, "*"));	\
		}							\
	}								\
	CANDLOOP(dst, k, hge_nil, end, cnt);				\
	return nils;							\
}

#else

#ifdef HAVE__MUL128
#include <intrin.h>
#pragma intrinsic(_mul128)

#define MUL_2TYPE_lng(TYPE1, TYPE2)					\
static BUN								\
mul_##TYPE1##_##TYPE2##_lng(const TYPE1 *lft, int incr1,		\
			    const TYPE2 *rgt, int incr2,		\
			    lng *dst, BUN cnt, BUN start,		\
			    BUN end, const oid *cand,			\
			    const oid *candend, oid candoff,		\
			    int abort_on_error)				\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
	lng clo, chi;							\
									\
	CANDLOOP(dst, k, lng_nil, 0, start);				\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, lng_nil);			\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = lng_nil;				\
			nils++;						\
		} else {						\
			clo = _mul128((lng) lft[i],			\
				      (lng) rgt[j], &chi);		\
			if ((chi == 0 && clo >= 0) ||			\
			    (chi == -1 && clo < 0 && clo != lng_nil)) {	\
				dst[k] = clo;				\
			} else {					\
				if (abort_on_error)			\
					ON_OVERFLOW(TYPE1, TYPE2, "*");	\
				dst[k] = lng_nil;			\
				nils++;					\
			}						\
		}							\
	}								\
	CANDLOOP(dst, k, lng_nil, end, cnt);				\
	return nils;							\
}
#else
#define MUL_2TYPE_lng(TYPE1, TYPE2)					\
static BUN								\
mul_##TYPE1##_##TYPE2##_lng(const TYPE1 *lft, int incr1,		\
			    const TYPE2 *rgt, int incr2,		\
			    lng *restrict dst, BUN cnt, BUN start,	\
			    BUN end, const oid *restrict cand,		\
			    const oid *candend, oid candoff,		\
			    int abort_on_error)				\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, lng_nil, 0, start);				\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, lng_nil);			\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = lng_nil;				\
			nils++;						\
		} else {						\
			LNGMUL_CHECK(TYPE1, lft[i],			\
				     TYPE2, rgt[j],			\
				     dst[k],				\
				     ON_OVERFLOW(TYPE1, TYPE2, "*"));	\
		}							\
	}								\
	CANDLOOP(dst, k, lng_nil, end, cnt);				\
	return nils;							\
}
#endif

#endif

#define MUL_2TYPE_float(TYPE1, TYPE2, TYPE3)				\
static BUN								\
mul_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			/* only check for overflow, not for underflow */ \
			if (ABSOLUTE(lft[i]) > 1 &&			\
			    GDK_##TYPE3##_max / ABSOLUTE(lft[i]) < ABSOLUTE(rgt[j])) { \
				if (abort_on_error)			\
					ON_OVERFLOW(TYPE1, TYPE2, "*");	\
				dst[k] = TYPE3##_nil;			\
				nils++;					\
			} else {					\
				dst[k] = (TYPE3) lft[i] * rgt[j];	\
			}						\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

MUL_4TYPE(bte, bte, bte, sht)
MUL_3TYPE_enlarge(bte, bte, sht)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(bte, bte, int)
MUL_3TYPE_enlarge(bte, bte, lng)
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(bte, bte, hge)
#endif
MUL_3TYPE_enlarge(bte, bte, flt)
MUL_3TYPE_enlarge(bte, bte, dbl)
#endif
MUL_4TYPE(bte, sht, sht, int)
MUL_3TYPE_enlarge(bte, sht, int)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(bte, sht, lng)
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(bte, sht, hge)
#endif
MUL_3TYPE_enlarge(bte, sht, flt)
MUL_3TYPE_enlarge(bte, sht, dbl)
#endif
MUL_4TYPE(bte, int, int, lng)
MUL_3TYPE_enlarge(bte, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(bte, int, hge)
#endif
MUL_3TYPE_enlarge(bte, int, flt)
MUL_3TYPE_enlarge(bte, int, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(bte, lng, lng, hge)
#else
MUL_2TYPE_lng(bte, lng)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(bte, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(bte, lng, flt)
MUL_3TYPE_enlarge(bte, lng, dbl)
#endif
#ifdef HAVE_HGE
MUL_2TYPE_hge(bte, hge)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(bte, hge, flt)
MUL_3TYPE_enlarge(bte, hge, dbl)
#endif
#endif
MUL_2TYPE_float(bte, flt, flt)
MUL_3TYPE_enlarge(bte, flt, dbl)
MUL_2TYPE_float(bte, dbl, dbl)
MUL_4TYPE(sht, bte, sht, int)
MUL_3TYPE_enlarge(sht, bte, int)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(sht, bte, lng)
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(sht, bte, hge)
#endif
MUL_3TYPE_enlarge(sht, bte, flt)
MUL_3TYPE_enlarge(sht, bte, dbl)
#endif
MUL_4TYPE(sht, sht, sht, int)
MUL_3TYPE_enlarge(sht, sht, int)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(sht, sht, lng)
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(sht, sht, hge)
#endif
MUL_3TYPE_enlarge(sht, sht, flt)
MUL_3TYPE_enlarge(sht, sht, dbl)
#endif
MUL_4TYPE(sht, int, int, lng)
MUL_3TYPE_enlarge(sht, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(sht, int, hge)
#endif
MUL_3TYPE_enlarge(sht, int, flt)
MUL_3TYPE_enlarge(sht, int, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(sht, lng, lng, hge)
#else
MUL_2TYPE_lng(sht, lng)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(sht, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(sht, lng, flt)
MUL_3TYPE_enlarge(sht, lng, dbl)
#endif
#ifdef HAVE_HGE
MUL_2TYPE_hge(sht, hge)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(sht, hge, flt)
MUL_3TYPE_enlarge(sht, hge, dbl)
#endif
#endif
MUL_2TYPE_float(sht, flt, flt)
MUL_3TYPE_enlarge(sht, flt, dbl)
MUL_2TYPE_float(sht, dbl, dbl)
MUL_4TYPE(int, bte, int, lng)
MUL_3TYPE_enlarge(int, bte, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(int, bte, hge)
#endif
MUL_3TYPE_enlarge(int, bte, flt)
MUL_3TYPE_enlarge(int, bte, dbl)
#endif
MUL_4TYPE(int, sht, int, lng)
MUL_3TYPE_enlarge(int, sht, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(int, sht, hge)
#endif
MUL_3TYPE_enlarge(int, sht, flt)
MUL_3TYPE_enlarge(int, sht, dbl)
#endif
MUL_4TYPE(int, int, int, lng)
MUL_3TYPE_enlarge(int, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(int, int, hge)
#endif
MUL_3TYPE_enlarge(int, int, flt)
MUL_3TYPE_enlarge(int, int, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(int, lng, lng, hge)
#else
MUL_2TYPE_lng(int, lng)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(int, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(int, lng, flt)
MUL_3TYPE_enlarge(int, lng, dbl)
#endif
#ifdef HAVE_HGE
MUL_2TYPE_hge(int, hge)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(int, hge, flt)
MUL_3TYPE_enlarge(int, hge, dbl)
#endif
#endif
MUL_2TYPE_float(int, flt, flt)
MUL_3TYPE_enlarge(int, flt, dbl)
MUL_2TYPE_float(int, dbl, dbl)
#ifdef HAVE_HGE
MUL_4TYPE(lng, bte, lng, hge)
#else
MUL_2TYPE_lng(lng, bte)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(lng, bte, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(lng, bte, flt)
MUL_3TYPE_enlarge(lng, bte, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(lng, sht, lng, hge)
#else
MUL_2TYPE_lng(lng, sht)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(lng, sht, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(lng, sht, flt)
MUL_3TYPE_enlarge(lng, sht, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(lng, int, lng, hge)
#else
MUL_2TYPE_lng(lng, int)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(lng, int, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(lng, int, flt)
MUL_3TYPE_enlarge(lng, int, dbl)
#endif
#ifdef HAVE_HGE
MUL_4TYPE(lng, lng, lng, hge)
#else
MUL_2TYPE_lng(lng, lng)
#endif
#ifdef HAVE_HGE
MUL_3TYPE_enlarge(lng, lng, hge)
#endif
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(lng, lng, flt)
MUL_3TYPE_enlarge(lng, lng, dbl)
#endif
#ifdef HAVE_HGE
MUL_2TYPE_hge(lng, hge)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(lng, hge, flt)
MUL_3TYPE_enlarge(lng, hge, dbl)
#endif
#endif
MUL_2TYPE_float(lng, flt, flt)
MUL_3TYPE_enlarge(lng, flt, dbl)
MUL_2TYPE_float(lng, dbl, dbl)
#ifdef HAVE_HGE
MUL_2TYPE_hge(hge, bte)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(hge, bte, flt)
MUL_3TYPE_enlarge(hge, bte, dbl)
#endif
MUL_2TYPE_hge(hge, sht)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(hge, sht, flt)
MUL_3TYPE_enlarge(hge, sht, dbl)
#endif
MUL_2TYPE_hge(hge, int)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(hge, int, flt)
MUL_3TYPE_enlarge(hge, int, dbl)
#endif
MUL_2TYPE_hge(hge, lng)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(hge, lng, flt)
MUL_3TYPE_enlarge(hge, lng, dbl)
#endif
MUL_2TYPE_hge(hge, hge)
#ifdef FULL_IMPLEMENTATION
MUL_3TYPE_enlarge(hge, hge, flt)
MUL_3TYPE_enlarge(hge, hge, dbl)
#endif
MUL_2TYPE_float(hge, flt, flt)
MUL_3TYPE_enlarge(hge, flt, dbl)
MUL_2TYPE_float(hge, dbl, dbl)
#endif
MUL_2TYPE_float(flt, bte, flt)
MUL_3TYPE_enlarge(flt, bte, dbl)
MUL_2TYPE_float(flt, sht, flt)
MUL_3TYPE_enlarge(flt, sht, dbl)
MUL_2TYPE_float(flt, int, flt)
MUL_3TYPE_enlarge(flt, int, dbl)
MUL_2TYPE_float(flt, lng, flt)
MUL_3TYPE_enlarge(flt, lng, dbl)
#ifdef HAVE_HGE
MUL_2TYPE_float(flt, hge, flt)
MUL_3TYPE_enlarge(flt, hge, dbl)
#endif
MUL_2TYPE_float(flt, flt, flt)
MUL_3TYPE_enlarge(flt, flt, dbl)
MUL_2TYPE_float(flt, dbl, dbl)
MUL_2TYPE_float(dbl, bte, dbl)
MUL_2TYPE_float(dbl, sht, dbl)
MUL_2TYPE_float(dbl, int, dbl)
MUL_2TYPE_float(dbl, lng, dbl)
#ifdef HAVE_HGE
MUL_2TYPE_float(dbl, hge, dbl)
#endif
MUL_2TYPE_float(dbl, flt, dbl)
MUL_2TYPE_float(dbl, dbl, dbl)

static BUN
mul_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	tp = ATOMbasetype(tp);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mul_bte_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_sht:
				nils = mul_bte_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mul_bte_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_lng:
				nils = mul_bte_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_bte_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_bte_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_bte_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mul_bte_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mul_bte_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mul_bte_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_bte_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_bte_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_bte_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mul_bte_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mul_bte_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_bte_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_bte_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_bte_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mul_bte_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_bte_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_bte_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_bte_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mul_bte_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_bte_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_bte_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_bte_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_bte_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_bte_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_sht:
				nils = mul_sht_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mul_sht_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mul_sht_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_sht_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_sht_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_sht_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mul_sht_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mul_sht_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mul_sht_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_sht_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_sht_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_sht_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mul_sht_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mul_sht_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_sht_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_sht_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_sht_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mul_sht_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_sht_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_sht_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_sht_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mul_sht_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_sht_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_sht_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_sht_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_sht_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_sht_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_int:
				nils = mul_int_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mul_int_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_int_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_int_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_int_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_int:
				nils = mul_int_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mul_int_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_int_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_int_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_int_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mul_int_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mul_int_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_int_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			case TYPE_flt:
				nils = mul_int_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_int_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mul_int_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_int_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_int_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_int_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mul_int_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_int_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_int_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_int_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_int_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_int_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_lng:
				nils = mul_lng_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_lng_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_lng_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_lng_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_lng:
				nils = mul_lng_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_lng_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_lng_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_lng_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_lng:
				nils = mul_lng_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_lng_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_lng_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_lng_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mul_lng_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mul_lng_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_lng_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_lng_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mul_lng_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_lng_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_lng_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_lng_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_lng_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_lng_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_hge:
				nils = mul_hge_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_hge_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_hge_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_hge:
				nils = mul_hge_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_hge_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_hge_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_hge:
				nils = mul_hge_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_hge_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_hge_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_hge:
				nils = mul_hge_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_hge_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_hge_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mul_hge_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_flt:
				nils = mul_hge_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			case TYPE_dbl:
				nils = mul_hge_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_hge_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_hge_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_hge_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	case TYPE_flt:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mul_flt_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = mul_flt_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_flt_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_dbl:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mul_dbl_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	default:
		goto unsupported;
	}

	return nils;

  unsupported:
	GDKerror("%s: type combination mul(%s,%s)->%s) not supported.\n",
		 func, ATOMname(tp1), ATOMname(tp2), ATOMname(tp));
	return BUN_NONE;
}

static BAT *
BATcalcmuldivmod(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error,
		 BUN (*typeswitchloop)(const void *, int, int, const void *,
				       int, int, void *, int, BUN, BUN, BUN,
				       const oid *restrict, const oid *, oid, int,
				       const char *),
		 const char *func)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, func, NULL);
	BATcheck(b2, func, NULL);

	if (checkbats(b1, b2, func) != GDK_SUCCEED)
		return NULL;

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = (*typeswitchloop)(Tloc(b1, b1->batFirst), b1->T->type, 1,
				 Tloc(b2, b2->batFirst), b2->T->type, 1,
				 Tloc(bn, bn->batFirst), tp,
				 cnt, start, end,
				 cand, candend, b1->H->seq,
				 abort_on_error, func);

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcmul(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error)
{
	return BATcalcmuldivmod(b1, b2, s, tp, abort_on_error,
				mul_typeswitchloop, "BATcalcmul");
}

BAT *
BATcalcmulcst(BAT *b, const ValRecord *v, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcmulcst", NULL);

	if (checkbats(b, NULL, "BATcalcmulcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = mul_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalcmulcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted, or reverse sorted if the constant is negative */
	if (abort_on_error) {
		ValRecord sign;

		VARcalcsign(&sign, v);
		bn->T->sorted = (sign.val.btval >= 0 && b->T->sorted && nils == 0) ||
			(sign.val.btval <= 0 && b->T->revsorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
		bn->T->revsorted = (sign.val.btval >= 0 && b->T->revsorted && nils == 0) ||
			(sign.val.btval <= 0 && b->T->sorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
	} else {
		bn->T->sorted = cnt <= 1 || nils == cnt;
		bn->T->revsorted = cnt <= 1 || nils == cnt;
	}
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstmul(const ValRecord *v, BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstmul", NULL);

	if (checkbats(b, NULL, "BATcalccstmul") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = mul_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstmul");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no overflow occurred (we only
	 * know for sure if abort_on_error is set), the result is also
	 * sorted, or reverse sorted if the constant is negative */
	if (abort_on_error) {
		ValRecord sign;

		VARcalcsign(&sign, v);
		bn->T->sorted = (sign.val.btval >= 0 && b->T->sorted && nils == 0) ||
			(sign.val.btval <= 0 && b->T->revsorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
		bn->T->revsorted = (sign.val.btval >= 0 && b->T->revsorted && nils == 0) ||
			(sign.val.btval <= 0 && b->T->sorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
	} else {
		bn->T->sorted = cnt <= 1 || nils == cnt;
		bn->T->revsorted = cnt <= 1 || nils == cnt;
	}
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcmul(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	if (mul_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcmul") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* division (any numeric type) */

#define DIV_3TYPE(TYPE1, TYPE2, TYPE3)					\
static BUN								\
div_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else if (rgt[j] == 0) {				\
			if (abort_on_error)				\
				return BUN_NONE + 1;			\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) (lft[i] / rgt[j]);		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#define DIV_3TYPE_float(TYPE1, TYPE2, TYPE3)				\
static BUN								\
div_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else if (rgt[j] == 0 ||				\
			   (ABSOLUTE(rgt[j]) < 1 &&			\
			    GDK_##TYPE3##_max * ABSOLUTE(rgt[j]) < lft[i])) { \
			/* only check for overflow, not for underflow */ \
			if (abort_on_error) {				\
				if (rgt[j] == 0)			\
					return BUN_NONE + 1;		\
				ON_OVERFLOW(TYPE1, TYPE2, "/");		\
			}						\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) lft[i] / rgt[j];		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

DIV_3TYPE(bte, bte, bte)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(bte, bte, sht)
DIV_3TYPE(bte, bte, int)
DIV_3TYPE(bte, bte, lng)
#ifdef HAVE_HGE
DIV_3TYPE(bte, bte, hge)
#endif
#endif
DIV_3TYPE(bte, bte, flt)
DIV_3TYPE(bte, bte, dbl)
DIV_3TYPE(bte, sht, bte)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(bte, sht, sht)
DIV_3TYPE(bte, sht, int)
DIV_3TYPE(bte, sht, lng)
#ifdef HAVE_HGE
DIV_3TYPE(bte, sht, hge)
#endif
#endif
DIV_3TYPE(bte, sht, flt)
DIV_3TYPE(bte, sht, dbl)
DIV_3TYPE(bte, int, bte)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(bte, int, sht)
DIV_3TYPE(bte, int, int)
DIV_3TYPE(bte, int, lng)
#ifdef HAVE_HGE
DIV_3TYPE(bte, int, hge)
#endif
#endif
DIV_3TYPE(bte, int, flt)
DIV_3TYPE(bte, int, dbl)
DIV_3TYPE(bte, lng, bte)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(bte, lng, sht)
DIV_3TYPE(bte, lng, int)
DIV_3TYPE(bte, lng, lng)
#ifdef HAVE_HGE
DIV_3TYPE(bte, lng, hge)
#endif
#endif
DIV_3TYPE(bte, lng, flt)
DIV_3TYPE(bte, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(bte, hge, bte)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(bte, hge, sht)
DIV_3TYPE(bte, hge, int)
DIV_3TYPE(bte, hge, lng)
DIV_3TYPE(bte, hge, hge)
#endif
DIV_3TYPE(bte, hge, flt)
DIV_3TYPE(bte, hge, dbl)
#endif
DIV_3TYPE_float(bte, flt, flt)
DIV_3TYPE_float(bte, flt, dbl)
DIV_3TYPE_float(bte, dbl, dbl)
DIV_3TYPE(sht, bte, sht)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(sht, bte, int)
DIV_3TYPE(sht, bte, lng)
#ifdef HAVE_HGE
DIV_3TYPE(sht, bte, hge)
#endif
#endif
DIV_3TYPE(sht, bte, flt)
DIV_3TYPE(sht, bte, dbl)
DIV_3TYPE(sht, sht, sht)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(sht, sht, int)
DIV_3TYPE(sht, sht, lng)
#ifdef HAVE_HGE
DIV_3TYPE(sht, sht, hge)
#endif
#endif
DIV_3TYPE(sht, sht, flt)
DIV_3TYPE(sht, sht, dbl)
DIV_3TYPE(sht, int, sht)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(sht, int, int)
DIV_3TYPE(sht, int, lng)
#ifdef HAVE_HGE
DIV_3TYPE(sht, int, hge)
#endif
#endif
DIV_3TYPE(sht, int, flt)
DIV_3TYPE(sht, int, dbl)
DIV_3TYPE(sht, lng, sht)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(sht, lng, int)
DIV_3TYPE(sht, lng, lng)
#ifdef HAVE_HGE
DIV_3TYPE(sht, lng, hge)
#endif
#endif
DIV_3TYPE(sht, lng, flt)
DIV_3TYPE(sht, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(sht, hge, sht)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(sht, hge, int)
DIV_3TYPE(sht, hge, lng)
DIV_3TYPE(sht, hge, hge)
#endif
DIV_3TYPE(sht, hge, flt)
DIV_3TYPE(sht, hge, dbl)
#endif
DIV_3TYPE_float(sht, flt, flt)
DIV_3TYPE_float(sht, flt, dbl)
DIV_3TYPE_float(sht, dbl, dbl)
DIV_3TYPE(int, bte, int)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(int, bte, lng)
#ifdef HAVE_HGE
DIV_3TYPE(int, bte, hge)
#endif
#endif
DIV_3TYPE(int, bte, flt)
DIV_3TYPE(int, bte, dbl)
DIV_3TYPE(int, sht, int)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(int, sht, lng)
#ifdef HAVE_HGE
DIV_3TYPE(int, sht, hge)
#endif
#endif
DIV_3TYPE(int, sht, flt)
DIV_3TYPE(int, sht, dbl)
DIV_3TYPE(int, int, int)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(int, int, lng)
#ifdef HAVE_HGE
DIV_3TYPE(int, int, hge)
#endif
#endif
DIV_3TYPE(int, int, flt)
DIV_3TYPE(int, int, dbl)
DIV_3TYPE(int, lng, int)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(int, lng, lng)
#ifdef HAVE_HGE
DIV_3TYPE(int, lng, hge)
#endif
#endif
DIV_3TYPE(int, lng, flt)
DIV_3TYPE(int, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(int, hge, int)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(int, hge, lng)
DIV_3TYPE(int, hge, hge)
#endif
DIV_3TYPE(int, hge, flt)
DIV_3TYPE(int, hge, dbl)
#endif
DIV_3TYPE_float(int, flt, flt)
DIV_3TYPE_float(int, flt, dbl)
DIV_3TYPE_float(int, dbl, dbl)
DIV_3TYPE(lng, bte, lng)
#ifdef HAVE_HGE
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(lng, bte, hge)
#endif
#endif
DIV_3TYPE(lng, bte, flt)
DIV_3TYPE(lng, bte, dbl)
DIV_3TYPE(lng, sht, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
DIV_3TYPE(lng, sht, hge)
#endif
#endif
DIV_3TYPE(lng, sht, flt)
DIV_3TYPE(lng, sht, dbl)
DIV_3TYPE(lng, int, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
DIV_3TYPE(lng, int, hge)
#endif
#endif
DIV_3TYPE(lng, int, flt)
DIV_3TYPE(lng, int, dbl)
DIV_3TYPE(lng, lng, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
DIV_3TYPE(lng, lng, hge)
#endif
#endif
DIV_3TYPE(lng, lng, flt)
DIV_3TYPE(lng, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(lng, hge, lng)
#ifdef FULL_IMPLEMENTATION
DIV_3TYPE(lng, hge, hge)
#endif
DIV_3TYPE(lng, hge, flt)
DIV_3TYPE(lng, hge, dbl)
#endif
DIV_3TYPE_float(lng, flt, flt)
DIV_3TYPE_float(lng, flt, dbl)
DIV_3TYPE_float(lng, dbl, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(hge, bte, hge)
DIV_3TYPE(hge, bte, flt)
DIV_3TYPE(hge, bte, dbl)
DIV_3TYPE(hge, sht, hge)
DIV_3TYPE(hge, sht, flt)
DIV_3TYPE(hge, sht, dbl)
DIV_3TYPE(hge, int, hge)
DIV_3TYPE(hge, int, flt)
DIV_3TYPE(hge, int, dbl)
DIV_3TYPE(hge, lng, hge)
DIV_3TYPE(hge, lng, flt)
DIV_3TYPE(hge, lng, dbl)
DIV_3TYPE(hge, hge, hge)
DIV_3TYPE(hge, hge, flt)
DIV_3TYPE(hge, hge, dbl)
DIV_3TYPE_float(hge, flt, flt)
DIV_3TYPE_float(hge, flt, dbl)
DIV_3TYPE_float(hge, dbl, dbl)
#endif
DIV_3TYPE(flt, bte, flt)
DIV_3TYPE(flt, bte, dbl)
DIV_3TYPE(flt, sht, flt)
DIV_3TYPE(flt, sht, dbl)
DIV_3TYPE(flt, int, flt)
DIV_3TYPE(flt, int, dbl)
DIV_3TYPE(flt, lng, flt)
DIV_3TYPE(flt, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(flt, hge, flt)
DIV_3TYPE(flt, hge, dbl)
#endif
DIV_3TYPE_float(flt, flt, flt)
DIV_3TYPE_float(flt, flt, dbl)
DIV_3TYPE_float(flt, dbl, dbl)
DIV_3TYPE(dbl, bte, dbl)
DIV_3TYPE(dbl, sht, dbl)
DIV_3TYPE(dbl, int, dbl)
DIV_3TYPE(dbl, lng, dbl)
#ifdef HAVE_HGE
DIV_3TYPE(dbl, hge, dbl)
#endif
DIV_3TYPE_float(dbl, flt, dbl)
DIV_3TYPE_float(dbl, dbl, dbl)

static BUN
div_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	tp = ATOMbasetype(tp);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = div_bte_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = div_bte_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = div_bte_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_bte_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_bte_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_bte_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_bte:
				nils = div_bte_sht_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = div_bte_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = div_bte_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_bte_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_bte_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_bte_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_bte:
				nils = div_bte_int_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = div_bte_int_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = div_bte_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_bte_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_bte_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_bte_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_bte:
				nils = div_bte_lng_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = div_bte_lng_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = div_bte_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_bte_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_bte_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_bte_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_bte:
				nils = div_bte_hge_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = div_bte_hge_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = div_bte_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_bte_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = div_bte_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			case TYPE_flt:
				nils = div_bte_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_bte_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_bte_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_bte_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_sht:
				nils = div_sht_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = div_sht_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_sht_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_sht_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_sht_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = div_sht_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = div_sht_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_sht_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_sht_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_sht_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_sht:
				nils = div_sht_int_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = div_sht_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_sht_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_sht_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_sht_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_sht:
				nils = div_sht_lng_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = div_sht_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_sht_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_sht_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_sht_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_sht:
				nils = div_sht_hge_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = div_sht_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = div_sht_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = div_sht_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			case TYPE_flt:
				nils = div_sht_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_sht_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_sht_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_sht_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_int:
				nils = div_int_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = div_int_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_int_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_int_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_int:
				nils = div_int_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = div_int_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_int_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_int_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = div_int_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = div_int_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_int_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_int_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_int:
				nils = div_int_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = div_int_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_int_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_int_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_int:
				nils = div_int_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = div_int_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = div_int_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			case TYPE_flt:
				nils = div_int_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_int_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_int_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_int_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_lng:
				nils = div_lng_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_lng_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_lng_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_lng:
				nils = div_lng_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_lng_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_lng_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_lng:
				nils = div_lng_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_lng_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_lng_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = div_lng_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = div_lng_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			case TYPE_flt:
				nils = div_lng_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_lng:
				nils = div_lng_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_hge:
				nils = div_lng_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			case TYPE_flt:
				nils = div_lng_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_lng_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_lng_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_lng_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_hge:
				nils = div_hge_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_flt:
				nils = div_hge_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_hge:
				nils = div_hge_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_flt:
				nils = div_hge_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_hge:
				nils = div_hge_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_flt:
				nils = div_hge_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_hge:
				nils = div_hge_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_flt:
				nils = div_hge_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = div_hge_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_flt:
				nils = div_hge_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_hge_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_hge_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_hge_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	case TYPE_flt:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = div_flt_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_dbl:
				nils = div_flt_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_flt_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_dbl:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = div_dbl_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	default:
		goto unsupported;
	}

	if (nils == BUN_NONE + 1)
		GDKerror("22012!division by zero.\n");

	return nils;

  unsupported:
	GDKerror("%s: type combination (div(%s,%s)->%s) not supported.\n",
		 func, ATOMname(tp1), ATOMname(tp2), ATOMname(tp));
	return BUN_NONE;
}

BAT *
BATcalcdiv(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error)
{
	return BATcalcmuldivmod(b1, b2, s, tp, abort_on_error,
				div_typeswitchloop, "BATcalcdiv");
}

BAT *
BATcalcdivcst(BAT *b, const ValRecord *v, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcdivcst", NULL);

	if (checkbats(b, NULL, "BATcalcdivcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = div_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalcdivcst");

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	/* if the input is sorted, and no zero division occurred (we
	 * only know for sure if abort_on_error is set), the result is
	 * also sorted, or reverse sorted if the constant is
	 * negative */
	if (abort_on_error) {
		ValRecord sign;

		VARcalcsign(&sign, v);
		bn->T->sorted = (sign.val.btval > 0 && b->T->sorted && nils == 0) ||
			(sign.val.btval < 0 && b->T->revsorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
		bn->T->revsorted = (sign.val.btval > 0 && b->T->revsorted && nils == 0) ||
			(sign.val.btval < 0 && b->T->sorted && nils == 0) ||
			cnt <= 1 || nils == cnt;
	} else {
		bn->T->sorted = cnt <= 1 || nils == cnt;
		bn->T->revsorted = cnt <= 1 || nils == cnt;
	}
	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstdiv(const ValRecord *v, BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstdiv", NULL);

	if (checkbats(b, NULL, "BATcalccstdiv") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = div_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstdiv");

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcdiv(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	if (div_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcdiv") >= BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* modulo (any numeric type) */

#define MOD_3TYPE(TYPE1, TYPE2, TYPE3)					\
static BUN								\
mod_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else if (rgt[j] == 0) {				\
			if (abort_on_error)				\
				return BUN_NONE + 1;			\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) lft[i] % rgt[j];		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

#define FMOD_3TYPE(TYPE1, TYPE2, TYPE3, FUNC)				\
static BUN								\
mod_##TYPE1##_##TYPE2##_##TYPE3(const TYPE1 *lft, int incr1,		\
				const TYPE2 *rgt, int incr2,		\
				TYPE3 *restrict dst, BUN cnt, BUN start, \
				BUN end, const oid *restrict cand,	\
				const oid *candend, oid candoff,	\
				int abort_on_error)			\
{									\
	BUN i, j, k;							\
	BUN nils = 0;							\
									\
	CANDLOOP(dst, k, TYPE3##_nil, 0, start);			\
	for (i = start * incr1, j = start * incr2, k = start;		\
	     k < end; i += incr1, j += incr2, k++) {			\
		CHECKCAND(dst, k, candoff, TYPE3##_nil);		\
		if (lft[i] == TYPE1##_nil || rgt[j] == TYPE2##_nil) {	\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else if (rgt[j] == 0) {				\
			if (abort_on_error)				\
				return BUN_NONE + 1;			\
			dst[k] = TYPE3##_nil;				\
			nils++;						\
		} else {						\
			dst[k] = (TYPE3) FUNC((TYPE3) lft[i],		\
					      (TYPE3) rgt[j]);		\
		}							\
	}								\
	CANDLOOP(dst, k, TYPE3##_nil, end, cnt);			\
	return nils;							\
}

MOD_3TYPE(bte, bte, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(bte, bte, sht)
MOD_3TYPE(bte, bte, int)
MOD_3TYPE(bte, bte, lng)
#ifdef HAVE_HGE
MOD_3TYPE(bte, bte, hge)
#endif
#endif
MOD_3TYPE(bte, sht, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(bte, sht, sht)
MOD_3TYPE(bte, sht, int)
MOD_3TYPE(bte, sht, lng)
#ifdef HAVE_HGE
MOD_3TYPE(bte, sht, hge)
#endif
#endif
MOD_3TYPE(bte, int, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(bte, int, sht)
MOD_3TYPE(bte, int, int)
MOD_3TYPE(bte, int, lng)
#ifdef HAVE_HGE
MOD_3TYPE(bte, int, hge)
#endif
#endif
MOD_3TYPE(bte, lng, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(bte, lng, sht)
MOD_3TYPE(bte, lng, int)
MOD_3TYPE(bte, lng, lng)
#ifdef HAVE_HGE
MOD_3TYPE(bte, lng, hge)
#endif
#endif
#ifdef HAVE_HGE
MOD_3TYPE(bte, hge, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(bte, hge, sht)
MOD_3TYPE(bte, hge, int)
MOD_3TYPE(bte, hge, lng)
MOD_3TYPE(bte, hge, hge)
#endif
#endif
MOD_3TYPE(sht, bte, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(sht, bte, sht)
MOD_3TYPE(sht, bte, int)
MOD_3TYPE(sht, bte, lng)
#ifdef HAVE_HGE
MOD_3TYPE(sht, bte, hge)
#endif
#endif
MOD_3TYPE(sht, sht, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(sht, sht, int)
MOD_3TYPE(sht, sht, lng)
#ifdef HAVE_HGE
MOD_3TYPE(sht, sht, hge)
#endif
#endif
MOD_3TYPE(sht, int, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(sht, int, int)
MOD_3TYPE(sht, int, lng)
#ifdef HAVE_HGE
MOD_3TYPE(sht, int, hge)
#endif
#endif
MOD_3TYPE(sht, lng, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(sht, lng, int)
MOD_3TYPE(sht, lng, lng)
#ifdef HAVE_HGE
MOD_3TYPE(sht, lng, hge)
#endif
#endif
#ifdef HAVE_HGE
MOD_3TYPE(sht, hge, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(sht, hge, int)
MOD_3TYPE(sht, hge, lng)
MOD_3TYPE(sht, hge, hge)
#endif
#endif
MOD_3TYPE(int, bte, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(int, bte, sht)
MOD_3TYPE(int, bte, int)
MOD_3TYPE(int, bte, lng)
#ifdef HAVE_HGE
MOD_3TYPE(int, bte, hge)
#endif
#endif
MOD_3TYPE(int, sht, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(int, sht, int)
MOD_3TYPE(int, sht, lng)
#ifdef HAVE_HGE
MOD_3TYPE(int, sht, hge)
#endif
#endif
MOD_3TYPE(int, int, int)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(int, int, lng)
#ifdef HAVE_HGE
MOD_3TYPE(int, int, hge)
#endif
#endif
MOD_3TYPE(int, lng, int)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(int, lng, lng)
#ifdef HAVE_HGE
MOD_3TYPE(int, lng, hge)
#endif
#endif
#ifdef HAVE_HGE
MOD_3TYPE(int, hge, int)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(int, hge, lng)
MOD_3TYPE(int, hge, hge)
#endif
#endif
MOD_3TYPE(lng, bte, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(lng, bte, sht)
MOD_3TYPE(lng, bte, int)
MOD_3TYPE(lng, bte, lng)
#ifdef HAVE_HGE
MOD_3TYPE(lng, bte, hge)
#endif
#endif
MOD_3TYPE(lng, sht, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(lng, sht, int)
MOD_3TYPE(lng, sht, lng)
#ifdef HAVE_HGE
MOD_3TYPE(lng, sht, hge)
#endif
#endif
MOD_3TYPE(lng, int, int)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(lng, int, lng)
#ifdef HAVE_HGE
MOD_3TYPE(lng, int, hge)
#endif
#endif
MOD_3TYPE(lng, lng, lng)
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
MOD_3TYPE(lng, lng, hge)
#endif
#endif
#ifdef HAVE_HGE
MOD_3TYPE(lng, hge, lng)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(lng, hge, hge)
#endif
#endif
#ifdef HAVE_HGE
MOD_3TYPE(hge, bte, bte)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(hge, bte, sht)
MOD_3TYPE(hge, bte, int)
MOD_3TYPE(hge, bte, lng)
MOD_3TYPE(hge, bte, hge)
#endif
MOD_3TYPE(hge, sht, sht)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(hge, sht, int)
MOD_3TYPE(hge, sht, lng)
MOD_3TYPE(hge, sht, hge)
#endif
MOD_3TYPE(hge, int, int)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(hge, int, lng)
MOD_3TYPE(hge, int, hge)
#endif
MOD_3TYPE(hge, lng, lng)
#ifdef FULL_IMPLEMENTATION
MOD_3TYPE(hge, lng, hge)
#endif
MOD_3TYPE(hge, hge, hge)
#endif

FMOD_3TYPE(bte, flt, flt, fmodf)
FMOD_3TYPE(sht, flt, flt, fmodf)
FMOD_3TYPE(int, flt, flt, fmodf)
FMOD_3TYPE(lng, flt, flt, fmodf)
#ifdef HAVE_HGE
FMOD_3TYPE(hge, flt, flt, fmodf)
#endif
FMOD_3TYPE(flt, bte, flt, fmodf)
FMOD_3TYPE(flt, sht, flt, fmodf)
FMOD_3TYPE(flt, int, flt, fmodf)
FMOD_3TYPE(flt, lng, flt, fmodf)
#ifdef HAVE_HGE
FMOD_3TYPE(flt, hge, flt, fmodf)
#endif
FMOD_3TYPE(flt, flt, flt, fmodf)
FMOD_3TYPE(bte, dbl, dbl, fmod)
FMOD_3TYPE(sht, dbl, dbl, fmod)
FMOD_3TYPE(int, dbl, dbl, fmod)
FMOD_3TYPE(lng, dbl, dbl, fmod)
#ifdef HAVE_HGE
FMOD_3TYPE(hge, dbl, dbl, fmod)
#endif
FMOD_3TYPE(flt, dbl, dbl, fmod)
FMOD_3TYPE(dbl, bte, dbl, fmod)
FMOD_3TYPE(dbl, sht, dbl, fmod)
FMOD_3TYPE(dbl, int, dbl, fmod)
FMOD_3TYPE(dbl, lng, dbl, fmod)
#ifdef HAVE_HGE
FMOD_3TYPE(dbl, hge, dbl, fmod)
#endif
FMOD_3TYPE(dbl, flt, dbl, fmod)
FMOD_3TYPE(dbl, dbl, dbl, fmod)

static BUN
mod_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	tp = ATOMbasetype(tp);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mod_bte_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_bte_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_bte_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_bte_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_bte_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_bte:
				nils = mod_bte_sht_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_bte_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_bte_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_bte_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_bte_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_bte:
				nils = mod_bte_int_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_bte_int_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_bte_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_bte_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_bte_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_bte:
				nils = mod_bte_lng_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_bte_lng_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_bte_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_bte_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_bte_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_bte:
				nils = mod_bte_hge_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_bte_hge_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_bte_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_bte_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_bte_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_bte_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_bte_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mod_sht_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_sht_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_sht_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_sht_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_sht_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mod_sht_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_sht_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_sht_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_sht_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_sht:
				nils = mod_sht_int_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_sht_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_sht_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_sht_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_sht:
				nils = mod_sht_lng_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_sht_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_sht_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_sht_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_sht:
				nils = mod_sht_hge_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_sht_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_sht_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_sht_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_sht_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_sht_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mod_int_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_int_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_int_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_int_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_int_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mod_int_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_int_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_int_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_int_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mod_int_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mod_int_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_int_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_int:
				nils = mod_int_lng_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mod_int_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_int_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_int:
				nils = mod_int_hge_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mod_int_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_int_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_int_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_int_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mod_lng_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_lng_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_lng_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_lng_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_lng_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mod_lng_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_lng_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_lng_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_lng_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mod_lng_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mod_lng_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_lng_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mod_lng_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
#ifdef HAVE_HGE
			case TYPE_hge:
				nils = mod_lng_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
#endif
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_lng:
				nils = mod_lng_hge_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_hge:
				nils = mod_lng_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_lng_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_lng_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_bte:
				nils = mod_hge_bte_bte(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_sht:
				nils = mod_hge_bte_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_int:
				nils = mod_hge_bte_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_hge_bte_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_hge_bte_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_sht:
				nils = mod_hge_sht_sht(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_int:
				nils = mod_hge_sht_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_lng:
				nils = mod_hge_sht_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_hge_sht_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_int:
				nils = mod_hge_int_int(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_lng:
				nils = mod_hge_int_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			case TYPE_hge:
				nils = mod_hge_int_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_lng:
				nils = mod_hge_lng_lng(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#ifdef FULL_IMPLEMENTATION
			case TYPE_hge:
				nils = mod_hge_lng_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
#endif
			default:
				goto unsupported;
			}
			break;
		case TYPE_hge:
			switch (tp) {
			case TYPE_hge:
				nils = mod_hge_hge_hge(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_hge_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_hge_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	case TYPE_flt:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_bte_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_sht_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_int_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_lng_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_hge_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_flt:
				nils = mod_flt_flt_flt(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_flt_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	case TYPE_dbl:
		switch (tp2) {
		case TYPE_bte:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_bte_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_sht:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_sht_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_int:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_int_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_lng:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_lng_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_hge_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
#endif
		case TYPE_flt:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_flt_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		case TYPE_dbl:
			switch (tp) {
			case TYPE_dbl:
				nils = mod_dbl_dbl_dbl(lft, incr1, rgt, incr2,
						       dst, cnt, start, end,
						       cand, candend, candoff,
						       abort_on_error);
				break;
			default:
				goto unsupported;
			}
			break;
		default:
			goto unsupported;
		}
		break;
	default:
		goto unsupported;
	}

	if (nils == BUN_NONE + 1)
		GDKerror("22012!division by zero.\n");

	return nils;

  unsupported:
	GDKerror("%s: type combination (mod(%s,%s)->%s) not supported.\n",
		 func, ATOMname(tp1), ATOMname(tp2), ATOMname(tp));
	return BUN_NONE;
}

BAT *
BATcalcmod(BAT *b1, BAT *b2, BAT *s, int tp, int abort_on_error)
{
	return BATcalcmuldivmod(b1, b2, s, tp, abort_on_error,
				mod_typeswitchloop, "BATcalcmod");
}

BAT *
BATcalcmodcst(BAT *b, const ValRecord *v, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcmodcst", NULL);

	if (checkbats(b, NULL, "BATcalcmodcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = mod_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalcmodcst");

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstmod(const ValRecord *v, BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstmod", NULL);

	if (checkbats(b, NULL, "BATcalccstmod") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, tp, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = mod_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst), tp,
				  cnt, start, end,
				  cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstmod");

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcmod(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	if (mod_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), ret->vtype, 1,
			       0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcmod") >= BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* logical (for type bit) or bitwise (for integral types) exclusive OR */

#define XOR(a, b)	((a) ^ (b))
#define XORBIT(a, b)	(((a) == 0) != ((b) == 0))

static BUN
xor_typeswitchloop(const void *lft, int incr1,
		   const void *rgt, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int nonil, const char *func)
{
	BUN nils = 0;
	BUN i, j, k;

	switch (ATOMbasetype(tp)) {
	case TYPE_bte:
		if (tp == TYPE_bit) {
			if (nonil)
				BINARY_3TYPE_FUNC_nonil(bit, bit, bit, XORBIT);
			else
				BINARY_3TYPE_FUNC(bit, bit, bit, XORBIT);
		} else {
			if (nonil)
				BINARY_3TYPE_FUNC_nonil(bte, bte, bte, XOR);
			else
				BINARY_3TYPE_FUNC(bte, bte, bte, XOR);
		}
		break;
	case TYPE_sht:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(sht, sht, sht, XOR);
		else
			BINARY_3TYPE_FUNC(sht, sht, sht, XOR);
		break;
	case TYPE_int:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(int, int, int, XOR);
		else
			BINARY_3TYPE_FUNC(int, int, int, XOR);
		break;
	case TYPE_lng:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(lng, lng, lng, XOR);
		else
			BINARY_3TYPE_FUNC(lng, lng, lng, XOR);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(hge, hge, hge, XOR);
		else
			BINARY_3TYPE_FUNC(hge, hge, hge, XOR);
		break;
#endif
	default:
		GDKerror("%s: bad input type %s.\n", func, ATOMname(tp));
		return BUN_NONE;
	}

	return nils;
}

BAT *
BATcalcxor(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcxor", NULL);
	BATcheck(b2, "BATcalcxor", NULL);

	if (checkbats(b1, b2, "BATcalcxor") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b1->T->type) != ATOMbasetype(b2->T->type)) {
		GDKerror("BATcalcxor: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = xor_typeswitchloop(Tloc(b1, b1->batFirst), 1,
				  Tloc(b2, b2->batFirst), 1,
				  Tloc(bn, bn->batFirst),
				  b1->T->type, cnt,
				  start, end, cand, candend, b1->H->seq,
				  cand == NULL && b1->T->nonil && b2->T->nonil,
				  "BATcalcxor");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcxorcst(BAT *b, const ValRecord *v, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcxorcst", NULL);

	if (checkbats(b, NULL, "BATcalcxorcst") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalcxorcst: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = xor_typeswitchloop(Tloc(b, b->batFirst), 1,
				  VALptr(v), 0,
				  Tloc(bn, bn->batFirst), b->T->type,
				  cnt,
				  start, end, cand, candend, b->H->seq,
				  cand == NULL && b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				  "BATcalcxorcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstxor(const ValRecord *v, BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstxor", NULL);

	if (checkbats(b, NULL, "BATcalccstxor") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalccstxor: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = xor_typeswitchloop(VALptr(v), 0,
				  Tloc(b, b->batFirst), 1,
				  Tloc(bn, bn->batFirst), b->T->type,
				  cnt,
				  start, end, cand, candend, b->H->seq,
				  cand == NULL && b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				  "BATcalccstxor");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcxor(ValPtr ret, const ValRecord *lft, const ValRecord *rgt)
{
	if (ATOMbasetype(lft->vtype) != ATOMbasetype(rgt->vtype)) {
		GDKerror("VARcalccstxor: incompatible input types.\n");
		return GDK_FAIL;
	}

	if (xor_typeswitchloop(VALptr(lft), 0,
			       VALptr(rgt), 0,
			       VALget(ret), lft->vtype,
			       1, 0, 1, NULL, NULL, 0, 0,
			       "VARcalcxor") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* logical (for type bit) or bitwise (for integral types) OR */

#define OR(a, b)	((a) | (b))

static BUN
or_typeswitchloop(const void *lft, int incr1,
		  const void *rgt, int incr2,
		  void *restrict dst, int tp, BUN cnt,
		  BUN start, BUN end, const oid *restrict cand,
		  const oid *candend, oid candoff,
		  int nonil, const char *func)
{
	BUN nils = 0;
	BUN i, j, k;

	switch (ATOMbasetype(tp)) {
	case TYPE_bte:
		if (tp == TYPE_bit) {
			/* implement tri-Boolean algebra */
			CANDLOOP((bit *) dst, k, bit_nil, 0, start);
			for (i = start * incr1, j = start * incr2, k = start;
			     k < end; i += incr1, j += incr2, k++) {
				CHECKCAND((bit *) dst, k, candoff, bit_nil);
				/* note that any value not equal to 0
				 * and not equal to bit_nil (0x80) is
				 * considered true */
				if (((const bit *) lft)[i] & 0x7F ||
				    ((const bit *) rgt)[j] & 0x7F) {
					/* either one is true */
					((bit *) dst)[k] = 1;
				} else if (((const bit *) lft)[i] == 0 &&
					   ((const bit *) rgt)[j] == 0) {
					/* both are false */
					((bit *) dst)[k] = 0;
				} else {
					((bit *) dst)[k] = bit_nil;
					nils++;
				}
			}
			CANDLOOP((bit *) dst, k, bit_nil, end, cnt);
		} else {
			if (nonil)
				BINARY_3TYPE_FUNC_nonil(bte, bte, bte, OR);
			else
				BINARY_3TYPE_FUNC(bte, bte, bte, OR);
		}
		break;
	case TYPE_sht:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(sht, sht, sht, OR);
		else
			BINARY_3TYPE_FUNC(sht, sht, sht, OR);
		break;
	case TYPE_int:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(int, int, int, OR);
		else
			BINARY_3TYPE_FUNC(int, int, int, OR);
		break;
	case TYPE_lng:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(lng, lng, lng, OR);
		else
			BINARY_3TYPE_FUNC(lng, lng, lng, OR);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(hge, hge, hge, OR);
		else
			BINARY_3TYPE_FUNC(hge, hge, hge, OR);
		break;
#endif
	default:
		GDKerror("%s: bad input type %s.\n", func, ATOMname(tp));
		return BUN_NONE;
	}

	return nils;
}

BAT *
BATcalcor(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcor", NULL);
	BATcheck(b2, "BATcalcor", NULL);

	if (checkbats(b1, b2, "BATcalcor") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b1->T->type) != ATOMbasetype(b2->T->type)) {
		GDKerror("BATcalcor: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = or_typeswitchloop(Tloc(b1, b1->batFirst), 1,
				 Tloc(b2, b2->batFirst), 1,
				 Tloc(bn, bn->batFirst),
				 b1->T->type, cnt,
				 start, end, cand, candend, b1->H->seq,
				 b1->T->nonil && b2->T->nonil,
				 "BATcalcor");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcorcst(BAT *b, const ValRecord *v, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcorcst", NULL);

	if (checkbats(b, NULL, "BATcalcorcst") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalcorcst: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = or_typeswitchloop(Tloc(b, b->batFirst), 1,
				 VALptr(v), 0,
				 Tloc(bn, bn->batFirst), b->T->type,
				 cnt,
				 start, end, cand, candend, b->H->seq,
				 cand == NULL && b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				 "BATcalcorcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstor(const ValRecord *v, BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstor", NULL);

	if (checkbats(b, NULL, "BATcalccstor") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalccstor: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = or_typeswitchloop(VALptr(v), 0,
				 Tloc(b, b->batFirst), 1,
				 Tloc(bn, bn->batFirst), b->T->type,
				 cnt,
				 start, end, cand, candend, b->H->seq,
				 cand == NULL && b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				 "BATcalccstor");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcor(ValPtr ret, const ValRecord *lft, const ValRecord *rgt)
{
	if (ATOMbasetype(lft->vtype) != ATOMbasetype(rgt->vtype)) {
		GDKerror("VARcalccstor: incompatible input types.\n");
		return GDK_FAIL;
	}

	if (or_typeswitchloop(VALptr(lft), 0,
			      VALptr(rgt), 0,
			      VALget(ret), lft->vtype,
			      1, 0, 1, NULL, NULL, 0, 0,
			      "VARcalcor") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* logical (for type bit) or bitwise (for integral types) exclusive AND */

#define AND(a, b)	((a) & (b))

static BUN
and_typeswitchloop(const void *lft, int incr1,
		   const void *rgt, int incr2,
		   void *restrict dst, int tp, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int nonil, const char *func)
{
	BUN nils = 0;
	BUN i, j, k;

	switch (ATOMbasetype(tp)) {
	case TYPE_bte:
		if (tp == TYPE_bit) {
			/* implement tri-Boolean algebra */
			CANDLOOP((bit *) dst, k, bit_nil, 0, start);
			for (i = start * incr1, j = start * incr2, k = start;
			     k < end; i += incr1, j += incr2, k++) {
				CHECKCAND((bit *) dst, k, candoff, bit_nil);
				if (((const bit *) lft)[i] == 0 ||
				    ((const bit *) rgt)[j] == 0) {
					/* either one is false */
					((bit *) dst)[k] = 0;
				} else if (((const bit *) lft)[i] != bit_nil &&
					   ((const bit *) rgt)[j] != bit_nil) {
					/* both are true */
					((bit *) dst)[k] = 1;
				} else {
					((bit *) dst)[k] = bit_nil;
					nils++;
				}
			}
			CANDLOOP((bit *) dst, k, bit_nil, end, cnt);
		} else {
			if (nonil)
				BINARY_3TYPE_FUNC_nonil(bte, bte, bte, AND);
			else
				BINARY_3TYPE_FUNC(bte, bte, bte, AND);
		}
		break;
	case TYPE_sht:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(sht, sht, sht, AND);
		else
			BINARY_3TYPE_FUNC(sht, sht, sht, AND);
		break;
	case TYPE_int:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(int, int, int, AND);
		else
			BINARY_3TYPE_FUNC(int, int, int, AND);
		break;
	case TYPE_lng:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(lng, lng, lng, AND);
		else
			BINARY_3TYPE_FUNC(lng, lng, lng, AND);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		if (nonil)
			BINARY_3TYPE_FUNC_nonil(hge, hge, hge, AND);
		else
			BINARY_3TYPE_FUNC(hge, hge, hge, AND);
		break;
#endif
	default:
		GDKerror("%s: bad input type %s.\n", func, ATOMname(tp));
		return BUN_NONE;
	}

	return nils;
}

BAT *
BATcalcand(BAT *b1, BAT *b2, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcand", NULL);
	BATcheck(b2, "BATcalcand", NULL);

	if (checkbats(b1, b2, "BATcalcand") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b1->T->type) != ATOMbasetype(b2->T->type)) {
		GDKerror("BATcalcand: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = and_typeswitchloop(Tloc(b1, b1->batFirst), 1,
				  Tloc(b2, b2->batFirst), 1,
				  Tloc(bn, bn->batFirst),
				  b1->T->type, cnt,
				  start, end, cand, candend, b1->H->seq,
				  b1->T->nonil && b2->T->nonil,
				  "BATcalcand");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcandcst(BAT *b, const ValRecord *v, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcandcst", NULL);

	if (checkbats(b, NULL, "BATcalcandcst") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalcandcst: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = and_typeswitchloop(Tloc(b, b->batFirst), 1,
				  VALptr(v), 0,
				  Tloc(bn, bn->batFirst), b->T->type,
				  cnt, start, end, cand, candend, b->H->seq,
				  b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				  "BATcalcandcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstand(const ValRecord *v, BAT *b, BAT *s)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstand", NULL);

	if (checkbats(b, NULL, "BATcalccstand") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(v->vtype)) {
		GDKerror("BATcalccstand: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = and_typeswitchloop(VALptr(v), 0,
				  Tloc(b, b->batFirst), 1,
				  Tloc(bn, bn->batFirst), b->T->type,
				  cnt, start, end, cand, candend, b->H->seq,
				  b->T->nonil && ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0,
				  "BATcalccstand");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcand(ValPtr ret, const ValRecord *lft, const ValRecord *rgt)
{
	if (ATOMbasetype(lft->vtype) != ATOMbasetype(rgt->vtype)) {
		GDKerror("VARcalccstand: incompatible input types.\n");
		return GDK_FAIL;
	}

	if (and_typeswitchloop(VALptr(lft), 0,
			       VALptr(rgt), 0,
			       VALget(ret), lft->vtype,
			       1, 0, 1, NULL, NULL, 0, 0,
			       "VARcalcand") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* left shift (any integral type) */

#define LSH(a, b)		((a) << (b))

#define SHIFT_CHECK(a, b)	((b) < 0 || (b) >= 8 * (int) sizeof(a))
#define NO_SHIFT_CHECK(a, b)	0

/* In standard C, left shift is undefined if any of the following
 * conditions hold:
 * - right operand is negative or larger or equal to the width of the
 *   left operand;
 * - left operand is negative;
 * - left operand times two-to-the-power of the right operand is not
 *   representable in the (promoted) type of the left operand. */
#define LSH_CHECK(a, b, TYPE)	(SHIFT_CHECK(a, b) || (a) < 0 || (a) > (GDK_##TYPE##_max >> (b)))
#define LSH_CHECK_bte(a, b)	LSH_CHECK(a, b, bte)
#define LSH_CHECK_sht(a, b)	LSH_CHECK(a, b, sht)
#define LSH_CHECK_int(a, b)	LSH_CHECK(a, b, int)
#define LSH_CHECK_lng(a, b)	LSH_CHECK(a, b, lng)

static BUN
lsh_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils = 0;
	BUN i, j, k;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(bte, bte, bte, LSH,
						LSH_CHECK_bte);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(bte, sht, bte, LSH,
						LSH_CHECK_bte);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(bte, int, bte, LSH,
						LSH_CHECK_bte);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(bte, lng, bte, LSH,
						LSH_CHECK_bte);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(bte, hge, bte, LSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(sht, bte, sht, LSH,
						LSH_CHECK_sht);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(sht, sht, sht, LSH,
						LSH_CHECK_sht);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(sht, int, sht, LSH,
						LSH_CHECK_sht);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(sht, lng, sht, LSH,
						LSH_CHECK_sht);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(sht, hge, sht, LSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(int, bte, int, LSH,
						LSH_CHECK_int);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(int, sht, int, LSH,
						LSH_CHECK_int);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(int, int, int, LSH,
						LSH_CHECK_int);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(int, lng, int, LSH,
						LSH_CHECK_int);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(int, hge, int, LSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(lng, bte, lng, LSH,
						LSH_CHECK_lng);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(lng, sht, lng, LSH,
						LSH_CHECK_lng);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(lng, int, lng, LSH,
						LSH_CHECK_lng);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(lng, lng, lng, LSH,
						LSH_CHECK_lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(lng, hge, lng, LSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(hge, bte, hge, LSH,
						NO_SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(hge, sht, hge, LSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(hge, int, hge, LSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(hge, lng, hge, LSH,
						SHIFT_CHECK);
			break;
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(hge, hge, hge, LSH,
						SHIFT_CHECK);
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	default:
		goto unsupported;
	}

	return nils;

  unsupported:
	GDKerror("%s: bad input types %s,%s.\n", func,
		 ATOMname(tp1), ATOMname(tp2));
  checkfail:
	return BUN_NONE;
}

BAT *
BATcalclsh(BAT *b1, BAT *b2, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalclsh", NULL);
	BATcheck(b2, "BATcalclsh", NULL);

	if (checkbats(b1, b2, "BATcalclsh") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = lsh_typeswitchloop(Tloc(b1, b1->batFirst), b1->T->type, 1,
				  Tloc(b2, b2->batFirst), b2->T->type, 1,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b1->H->seq,
				  abort_on_error, "BATcalclsh");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalclshcst(BAT *b, const ValRecord *v, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalclshcst", NULL);

	if (checkbats(b, NULL, "BATcalclshcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = lsh_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b->H->seq,
				  abort_on_error, "BATcalclshcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstlsh(const ValRecord *v, BAT *b, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstlsh", NULL);

	if (checkbats(b, NULL, "BATcalccstlsh") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, v->vtype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = lsh_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstlsh");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalclsh(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	ret->vtype = lft->vtype;
	if (lsh_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), 1, 0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalclsh") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* right shift (any integral type) */

#define RSH(a, b)	((a) >> (b))

static BUN
rsh_typeswitchloop(const void *lft, int tp1, int incr1,
		   const void *rgt, int tp2, int incr2,
		   void *restrict dst, BUN cnt,
		   BUN start, BUN end, const oid *restrict cand,
		   const oid *candend, oid candoff,
		   int abort_on_error, const char *func)
{
	BUN nils = 0;
	BUN i, j, k;

	tp1 = ATOMbasetype(tp1);
	tp2 = ATOMbasetype(tp2);
	switch (tp1) {
	case TYPE_bte:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(bte, bte, bte, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(bte, sht, bte, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(bte, int, bte, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(bte, lng, bte, RSH,
						SHIFT_CHECK);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(bte, hge, bte, RSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_sht:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(sht, bte, sht, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(sht, sht, sht, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(sht, int, sht, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(sht, lng, sht, RSH,
						SHIFT_CHECK);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(sht, hge, sht, RSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_int:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(int, bte, int, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(int, sht, int, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(int, int, int, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(int, lng, int, RSH,
						SHIFT_CHECK);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(int, hge, int, RSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
	case TYPE_lng:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(lng, bte, lng, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(lng, sht, lng, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(lng, int, lng, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(lng, lng, lng, RSH,
						SHIFT_CHECK);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(lng, hge, lng, RSH,
						SHIFT_CHECK);
			break;
#endif
		default:
			goto unsupported;
		}
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (tp2) {
		case TYPE_bte:
			BINARY_3TYPE_FUNC_CHECK(hge, bte, hge, RSH,
						NO_SHIFT_CHECK);
			break;
		case TYPE_sht:
			BINARY_3TYPE_FUNC_CHECK(hge, sht, hge, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_int:
			BINARY_3TYPE_FUNC_CHECK(hge, int, hge, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_lng:
			BINARY_3TYPE_FUNC_CHECK(hge, lng, hge, RSH,
						SHIFT_CHECK);
			break;
		case TYPE_hge:
			BINARY_3TYPE_FUNC_CHECK(hge, hge, hge, RSH,
						SHIFT_CHECK);
			break;
		default:
			goto unsupported;
		}
		break;
#endif
	default:
		goto unsupported;
	}

	return nils;

  unsupported:
	GDKerror("%s: bad input types %s,%s.\n", func,
		 ATOMname(tp1), ATOMname(tp2));
  checkfail:
	return BUN_NONE;
}

BAT *
BATcalcrsh(BAT *b1, BAT *b2, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b1, "BATcalcrsh", NULL);
	BATcheck(b2, "BATcalcrsh", NULL);

	if (checkbats(b1, b2, "BATcalcrsh") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b1, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b1->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = rsh_typeswitchloop(Tloc(b1, b1->batFirst), b1->T->type, 1,
				  Tloc(b2, b2->batFirst), b2->T->type, 1,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b1->H->seq,
				  abort_on_error, "BATcalcrsh");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b1->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcrshcst(BAT *b, const ValRecord *v, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcrshcst", NULL);

	if (checkbats(b, NULL, "BATcalcrshcst") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, b->T->type, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = rsh_typeswitchloop(Tloc(b, b->batFirst), b->T->type, 1,
				  VALptr(v), v->vtype, 0,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b->H->seq,
				  abort_on_error, "BATcalcrshcst");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalccstrsh(const ValRecord *v, BAT *b, BAT *s, int abort_on_error)
{
	BAT *bn;
	BUN nils;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalccstrsh", NULL);

	if (checkbats(b, NULL, "BATcalccstrsh") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATnew(TYPE_void, v->vtype, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	nils = rsh_typeswitchloop(VALptr(v), v->vtype, 0,
				  Tloc(b, b->batFirst), b->T->type, 1,
				  Tloc(bn, bn->batFirst),
				  cnt, start, end, cand, candend, b->H->seq,
				  abort_on_error, "BATcalccstrsh");

	if (nils == BUN_NONE) {
		BBPunfix(bn->batCacheid);
		return NULL;
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

gdk_return
VARcalcrsh(ValPtr ret, const ValRecord *lft, const ValRecord *rgt,
	   int abort_on_error)
{
	ret->vtype = lft->vtype;
	if (rsh_typeswitchloop(VALptr(lft), lft->vtype, 0,
			       VALptr(rgt), rgt->vtype, 0,
			       VALget(ret), 1, 0, 1, NULL, NULL, 0,
			       abort_on_error, "VARcalcrsh") == BUN_NONE)
		return GDK_FAIL;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* less than (any "linear" type) */

/* these three are for all simple comparisons (6 in all) */
#define TYPE_TPE		TYPE_bit
#define TPE			bit
#define TPE_nil			bit_nil

#define OP			LT
#define op_typeswitchloop	lt_typeswitchloop
#define BATcalcop_intern	BATcalclt_intern
#define BATcalcop		BATcalclt
#define BATcalcop_name		"BATcalclt"
#define BATcalcopcst		BATcalcltcst
#define BATcalcopcst_name	"BATcalcltcst"
#define BATcalccstop		BATcalccstlt
#define BATcalccstop_name	"BATcalccstlt"
#define VARcalcop		VARcalclt
#define VARcalcop_name		"VARcalclt"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

/* ---------------------------------------------------------------------- */
/* greater than (any "linear" type) */

#define OP			GT
#define op_typeswitchloop	gt_typeswitchloop
#define BATcalcop_intern	BATcalcgt_intern
#define BATcalcop		BATcalcgt
#define BATcalcop_name		"BATcalcgt"
#define BATcalcopcst		BATcalcgtcst
#define BATcalcopcst_name	"BATcalcgtcst"
#define BATcalccstop		BATcalccstgt
#define BATcalccstop_name	"BATcalccstgt"
#define VARcalcop		VARcalcgt
#define VARcalcop_name		"VARcalclt"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

/* ---------------------------------------------------------------------- */
/* less than or equal (any "linear" type) */

#define LE(a, b)	((bit) ((a) <= (b)))

#define OP			LE
#define op_typeswitchloop	le_typeswitchloop
#define BATcalcop_intern	BATcalcle_intern
#define BATcalcop		BATcalcle
#define BATcalcop_name		"BATcalcle"
#define BATcalcopcst		BATcalclecst
#define BATcalcopcst_name	"BATcalclecst"
#define BATcalccstop		BATcalccstle
#define BATcalccstop_name	"BATcalccstle"
#define VARcalcop		VARcalcle
#define VARcalcop_name		"VARcalcle"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

/* ---------------------------------------------------------------------- */
/* greater than or equal (any "linear" type) */

#define GE(a, b)	((bit) ((a) >= (b)))

#define OP			GE
#define op_typeswitchloop	ge_typeswitchloop
#define BATcalcop_intern	BATcalcge_intern
#define BATcalcop		BATcalcge
#define BATcalcop_name		"BATcalcge"
#define BATcalcopcst		BATcalcgecst
#define BATcalcopcst_name	"BATcalcgecst"
#define BATcalccstop		BATcalccstge
#define BATcalccstop_name	"BATcalccstge"
#define VARcalcop		VARcalcge
#define VARcalcop_name		"VARcalcge"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

/* ---------------------------------------------------------------------- */
/* equal (any type) */

#define EQ(a, b)	((bit) ((a) == (b)))

#define OP			EQ
#define op_typeswitchloop	eq_typeswitchloop
#define BATcalcop_intern	BATcalceq_intern
#define BATcalcop		BATcalceq
#define BATcalcop_name		"BATcalceq"
#define BATcalcopcst		BATcalceqcst
#define BATcalcopcst_name	"BATcalceqcst"
#define BATcalccstop		BATcalccsteq
#define BATcalccstop_name	"BATcalccsteq"
#define VARcalcop		VARcalceq
#define VARcalcop_name		"VARcalceq"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

/* ---------------------------------------------------------------------- */
/* not equal (any type) */

#define NE(a, b)	((bit) ((a) != (b)))

#define OP			NE
#define op_typeswitchloop	ne_typeswitchloop
#define BATcalcop_intern	BATcalcne_intern
#define BATcalcop		BATcalcne
#define BATcalcop_name		"BATcalcne"
#define BATcalcopcst		BATcalcnecst
#define BATcalcopcst_name	"BATcalcnecst"
#define BATcalccstop		BATcalccstne
#define BATcalccstop_name	"BATcalccstne"
#define VARcalcop		VARcalcne
#define VARcalcop_name		"VARcalcne"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

#undef TYPE_TPE
#undef TPE
#undef TPE_nil

/* ---------------------------------------------------------------------- */
/* generic comparison (any "linear" type) */

#define CMP(a, b)	((bte) ((a) < (b) ? -1 : (a) > (b)))

#define TYPE_TPE		TYPE_bte
#define TPE			bte
#define TPE_nil			bte_nil

#define OP			CMP
#define op_typeswitchloop	cmp_typeswitchloop
#define BATcalcop_intern	BATcalccmp_intern
#define BATcalcop		BATcalccmp
#define BATcalcop_name		"BATcalccmp"
#define BATcalcopcst		BATcalccmpcst
#define BATcalcopcst_name	"BATcalccmpcst"
#define BATcalccstop		BATcalccstcmp
#define BATcalccstop_name	"BATcalccstcmp"
#define VARcalcop		VARcalccmp
#define VARcalcop_name		"VARcalccmp"

#include "gdk_calc_compare.h"

#undef OP
#undef op_typeswitchloop
#undef BATcalcop_intern
#undef BATcalcop
#undef BATcalcop_name
#undef BATcalcopcst
#undef BATcalcopcst_name
#undef BATcalccstop
#undef BATcalccstop_name
#undef VARcalcop
#undef VARcalcop_name

#undef TYPE_TPE
#undef TPE
#undef TPE_nil

/* ---------------------------------------------------------------------- */
/* between (any "linear" type) */

#define BETWEEN(v, lo, hi, TYPE)					\
	((v) == TYPE##_nil || (lo) == TYPE##_nil || (hi) == TYPE##_nil ? \
	 (nils++, bit_nil) :						\
	 (bit) (((v) >= (lo) && (v) <= (hi)) ||				\
		(sym && (v) >= (hi) && (v) <= (lo))))

#define BETWEEN_LOOP_TYPE(TYPE)						\
	do {								\
		for (i = start * incr1,					\
			     j = start * incr2,				\
			     k = start * incr3,				\
			     l = start;					\
		     l < end;						\
		     i += incr1, j += incr2, k += incr3, l++) {		\
			CHECKCAND(dst, l, seqbase, bit_nil);		\
			dst[l] = BETWEEN(((const TYPE *) src)[i],	\
					 ((const TYPE *) lo)[j],	\
					 ((const TYPE *) hi)[k],	\
					 TYPE);				\
		}							\
	} while (0)

static BAT *
BATcalcbetween_intern(const void *src, int incr1, const char *hp1, int wd1,
		      const void *lo, int incr2, const char *hp2, int wd2,
		      const void *hi, int incr3, const char *hp3, int wd3,
		      int tp, BUN cnt, BUN start, BUN end, const oid *restrict cand,
		      const oid *candend, oid seqbase, int sym,
		      const char *func)
{
	BAT *bn;
	BUN nils = 0;
	BUN i, j, k, l, soff = 0, loff = 0, hoff = 0;
	bit *restrict dst;
	const void *nil;
	int (*atomcmp)(const void *, const void *);

	bn = BATnew(TYPE_void, TYPE_bit, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	dst = (bit *) Tloc(bn, bn->batFirst);

	CANDLOOP(dst, l, bit_nil, 0, start);

	tp = ATOMbasetype(tp);

	switch (tp) {
	case TYPE_bte:
		BETWEEN_LOOP_TYPE(bte);
		break;
	case TYPE_sht:
		BETWEEN_LOOP_TYPE(sht);
		break;
	case TYPE_int:
		BETWEEN_LOOP_TYPE(int);
		break;
	case TYPE_lng:
		BETWEEN_LOOP_TYPE(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		BETWEEN_LOOP_TYPE(hge);
		break;
#endif
	case TYPE_flt:
		BETWEEN_LOOP_TYPE(flt);
		break;
	case TYPE_dbl:
		BETWEEN_LOOP_TYPE(dbl);
		break;
	default:
		assert(tp != TYPE_oid);
		assert(tp != TYPE_wrd);
		if (!ATOMlinear(tp) ||
		    (atomcmp = ATOMcompare(tp)) == NULL) {
			BBPunfix(bn->batCacheid);
			GDKerror("%s: bad input type %s.\n",
				 func, ATOMname(tp));
			return NULL;
		}
		nil = ATOMnilptr(tp);
		for (i = start * incr1,
			     j = start * incr2,
			     k = start * incr3,
			     l = start;
		     l < end;
		     i += incr1, j += incr2, k += incr3, l++,
			     soff += wd1, loff += wd2, hoff += wd3) {
			const void *p1, *p2, *p3;
			CHECKCAND(dst, l, seqbase, bit_nil);
			p1 = hp1 ? (const void *) (hp1 + VarHeapVal(src, i, wd1)) : (const void *) ((const char *) src + soff);
			p2 = hp2 ? (const void *) (hp2 + VarHeapVal(lo, j, wd2)) : (const void *) ((const char *) lo + loff);
			p3 = hp3 ? (const void *) (hp3 + VarHeapVal(hi, k, wd3)) : (const void *) ((const char *) hi + hoff);
			if (p1 == NULL || p2 == NULL || p3 == NULL ||
			    (*atomcmp)(p1, nil) == 0 ||
			    (*atomcmp)(p2, nil) == 0 ||
			    (*atomcmp)(p3, nil) == 0) {
				nils++;
				dst[l] = bit_nil;
			} else {
				dst[l] = (bit) (((*atomcmp)(p1, p2) >= 0 &&
						 (*atomcmp)(p1, p3) <= 0) ||
						(sym &&
						 (*atomcmp)(p1, p3) >= 0 &&
						 (*atomcmp)(p1, p2) <= 0));
			}
		}
		break;
	}

	CANDLOOP(dst, l, bit_nil, end, cnt);

	BATsetcount(bn, cnt);
	BATseqbase(bn, seqbase);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;

	return bn;
}

BAT *
BATcalcbetween(BAT *b, BAT *lo, BAT *hi, BAT *s, int sym)
{
	BAT *bn;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcbetween", NULL);
	BATcheck(lo, "BATcalcbetween", NULL);
	BATcheck(hi, "BATcalcbetween", NULL);

	if (checkbats(b, lo, "BATcalcbetween") != GDK_SUCCEED)
		return NULL;
	if (checkbats(b, hi, "BATcalcbetween") != GDK_SUCCEED)
		return NULL;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	if (b->T->type == TYPE_void &&
	    lo->T->type == TYPE_void &&
	    hi->T->type == TYPE_void) {
		bit res;

		if (b->T->seq == oid_nil ||
		    lo->T->seq == oid_nil ||
		    hi->T->seq == oid_nil)
			res = bit_nil;
		else
			res = (bit) ((b->T->seq >= lo->T->seq &&
				      b->T->seq <= hi->T->seq) ||
				     (sym &&
				      b->T->seq >= hi->T->seq &&
				      b->T->seq <= lo->T->seq));

		return BATconst(b, TYPE_bit, &res, TRANSIENT);
	}

	bn = BATcalcbetween_intern(Tloc(b, b->batFirst), 1,
				   b->T->vheap ? b->T->vheap->base : NULL,
				   b->T->width,
				   Tloc(lo, lo->batFirst), 1,
				   lo->T->vheap ? lo->T->vheap->base : NULL,
				   lo->T->width,
				   Tloc(hi, hi->batFirst), 1,
				   hi->T->vheap ? hi->T->vheap->base : NULL,
				   hi->T->width,
				   b->T->type, cnt,
				   start, end, cand, candend,
				   b->H->seq, sym, "BATcalcbetween");

	return bn;
}

BAT *
BATcalcbetweencstcst(BAT *b, const ValRecord *lo, const ValRecord *hi, BAT *s, int sym)
{
	BAT *bn;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcbetweencstcst", NULL);

	if (checkbats(b, NULL, "BATcalcbetweencstcst") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(lo->vtype) ||
	    ATOMbasetype(b->T->type) != ATOMbasetype(hi->vtype)) {
		GDKerror("BATcalcbetweencstcst: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATcalcbetween_intern(Tloc(b, b->batFirst), 1,
				   b->T->vheap ? b->T->vheap->base : NULL,
				   b->T->width,
				   VALptr(lo), 0, NULL, 0,
				   VALptr(hi), 0, NULL, 0,
				   b->T->type, cnt,
				   start, end, cand, candend,
				   b->H->seq, sym, "BATcalcbetweencstcst");

	return bn;
}

BAT *
BATcalcbetweenbatcst(BAT *b, BAT *lo, const ValRecord *hi, BAT *s, int sym)
{
	BAT *bn;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcbetweenbatcst", NULL);

	if (checkbats(b, lo, "BATcalcbetweenbatcst") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(hi->vtype)) {
		GDKerror("BATcalcbetweenbatcst: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATcalcbetween_intern(Tloc(b, b->batFirst), 1,
				   b->T->vheap ? b->T->vheap->base : NULL,
				   b->T->width,
				   Tloc(lo, lo->batFirst), 1,
				   lo->T->vheap ? lo->T->vheap->base : NULL,
				   lo->T->width,
				   VALptr(hi), 0, NULL, 0,
				   b->T->type, cnt,
				   start, end, cand, candend,
				   b->H->seq, sym, "BATcalcbetweenbatcst");

	return bn;
}

BAT *
BATcalcbetweencstbat(BAT *b, const ValRecord *lo, BAT *hi, BAT *s, int sym)
{
	BAT *bn;
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATcalcbetweencstbat", NULL);

	if (checkbats(b, hi, "BATcalcbetweencstbat") != GDK_SUCCEED)
		return NULL;

	if (ATOMbasetype(b->T->type) != ATOMbasetype(lo->vtype)) {
		GDKerror("BATcalcbetweencstbat: incompatible input types.\n");
		return NULL;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);

	bn = BATcalcbetween_intern(Tloc(b, b->batFirst), 1,
				   b->T->vheap ? b->T->vheap->base : NULL,
				   b->T->width,
				   VALptr(lo), 0, NULL, 0,
				   Tloc(hi, hi->batFirst), 1,
				   hi->T->vheap ? hi->T->vheap->base : NULL,
				   hi->T->width,
				   b->T->type, cnt,
				   start, end, cand, candend,
				   b->H->seq, sym, "BATcalcbetweencstbat");

	return bn;
}

gdk_return
VARcalcbetween(ValPtr ret, const ValRecord *v, const ValRecord *lo,
	       const ValRecord *hi, int sym)
{
	BUN nils = 0;		/* to make reusing BETWEEN macro easier */
	int t;
	int (*atomcmp)(const void *, const void *);
	const void *nil;

	t = v->vtype;
	if (t != lo->vtype || t != hi->vtype) {
		GDKerror("VARcalcbetween: incompatible input types.\n");
		return GDK_FAIL;
	}
	if (!ATOMlinear(t)) {
		GDKerror("VARcalcbetween: non-linear input type.\n");
		return GDK_FAIL;
	}

	t = ATOMbasetype(t);

	ret->vtype = TYPE_bit;
	switch (t) {
	case TYPE_bte:
		ret->val.btval = BETWEEN(v->val.btval, lo->val.btval, hi->val.btval, bte);
		break;
	case TYPE_sht:
		ret->val.btval = BETWEEN(v->val.shval, lo->val.shval, hi->val.shval, sht);
		break;
	case TYPE_int:
		ret->val.btval = BETWEEN(v->val.ival, lo->val.ival, hi->val.ival, int);
		break;
	case TYPE_lng:
		ret->val.btval = BETWEEN(v->val.lval, lo->val.lval, hi->val.lval, lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ret->val.btval = BETWEEN(v->val.hval, lo->val.hval, hi->val.hval, hge);
		break;
#endif
	case TYPE_flt:
		ret->val.btval = BETWEEN(v->val.fval, lo->val.fval, hi->val.fval, flt);
		break;
	case TYPE_dbl:
		ret->val.btval = BETWEEN(v->val.dval, lo->val.dval, hi->val.dval, dbl);
		break;
	default:
		nil = ATOMnilptr(t);
		atomcmp = ATOMcompare(t);
		if (atomcmp(VALptr(v), nil) == 0 ||
		    atomcmp(VALptr(lo), nil) == 0 ||
		    atomcmp(VALptr(hi), nil) == 0)
			ret->val.btval = bit_nil;
		else
			ret->val.btval =
				(bit) ((atomcmp(VALptr(v), VALptr(lo)) >= 0 &&
					atomcmp(VALptr(v), VALptr(hi)) <= 0) ||
				       (sym &&
					atomcmp(VALptr(v), VALptr(hi)) >= 0 &&
					atomcmp(VALptr(v), VALptr(lo)) <= 0));
		break;
	}
	(void) nils;
	return GDK_SUCCEED;
}

/* ---------------------------------------------------------------------- */
/* if-then-else (any type) */

#define IFTHENELSELOOP(TYPE)						\
	do {								\
		for (i = 0; i < cnt; i++) {				\
			if (src[i] == bit_nil) {			\
				((TYPE *) dst)[i] = * (TYPE *) nil;	\
				nils++;					\
			} else if (src[i]) {				\
				((TYPE *) dst)[i] = ((TYPE *) col1)[k]; \
			} else {					\
				((TYPE *) dst)[i] = ((TYPE *) col2)[l]; \
			}						\
			k += incr1;					\
			l += incr2;					\
		}							\
	} while (0)

static BAT *
BATcalcifthenelse_intern(BAT *b,
			 const void *col1, int incr1, const char *heap1,
			 int width1, int nonil1,
			 const void *col2, int incr2, const char *heap2,
			 int width2, int nonil2,
			 int tpe)
{
	BAT *bn;
	void *restrict dst;
	BUN i, k, l;
	BUN nils = 0;
	const void *nil;
	const void *p;
	const bit *src;
	BUN cnt = b->batCount;

	assert(col2 != NULL);

	bn = BATnew(TYPE_void, tpe, cnt, TRANSIENT);
	if (bn == NULL)
		return NULL;

	src = (const bit *) Tloc(b, b->batFirst);

	nil = ATOMnilptr(tpe);
	dst = (void *) Tloc(bn, bn->batFirst);
	k = l = 0;
	if (bn->T->varsized) {
		assert((heap1 != NULL && width1 > 0) || (width1 == 0 && incr1 == 0));
		assert((heap2 != NULL && width2 > 0) || (width2 == 0 && incr2 == 0));
		for (i = 0; i < cnt; i++) {
			if (src[i] == bit_nil) {
				p = nil;
				nils++;
			} else if (src[i]) {
				if (heap1)
					p = heap1 + VarHeapVal(col1, k, width1);
				else
					p = col1;
			} else {
				if (heap2)
					p = heap2 + VarHeapVal(col2, l, width2);
				else
					p = col2;
			}
			tfastins_nocheck(bn, i, p, Tsize(bn));
			k += incr1;
			l += incr2;
		}
	} else {
		assert(heap1 == NULL);
		assert(heap2 == NULL);
		switch (bn->T->width) {
		case 1:
			IFTHENELSELOOP(bte);
			break;
		case 2:
			IFTHENELSELOOP(sht);
			break;
		case 4:
			IFTHENELSELOOP(int);
			break;
		case 8:
			IFTHENELSELOOP(lng);
			break;
#ifdef HAVE_HGE
		case 16:
			IFTHENELSELOOP(hge);
			break;
#endif
		default:
			for (i = 0; i < cnt; i++) {
				if (src[i] == bit_nil) {
					p = nil;
					nils++;
				} else if (src[i]) {
					p = ((const char *) col1) + k * width1;
				} else {
					p = ((const char *) col2) + l * width2;
				}
				memcpy(dst, p, bn->T->width);
				dst = (void *) ((char *) dst + bn->T->width);
				k += incr1;
				l += incr2;
			}
		}
	}

	BATsetcount(bn, cnt);
	BATseqbase(bn, b->H->seq);

	bn->T->sorted = cnt <= 1 || nils == cnt;
	bn->T->revsorted = cnt <= 1 || nils == cnt;
	bn->T->key = cnt <= 1;
	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0 && nonil1 && nonil2;

	return bn;
  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

BAT *
BATcalcifthenelse(BAT *b, BAT *b1, BAT *b2)
{
	BATcheck(b, "BATcalcifthenelse", NULL);
	BATcheck(b1, "BATcalcifthenelse", NULL);
	BATcheck(b2, "BATcalcifthenelse", NULL);

	if (checkbats(b, b1, "BATcalcifthenelse") != GDK_SUCCEED)
		return NULL;
	if (checkbats(b, b2, "BATcalcifthenelse") != GDK_SUCCEED)
		return NULL;
	if (b->T->type != TYPE_bit || b1->T->type != b2->T->type) {
		GDKerror("BATcalcifthenelse: \"then\" and \"else\" BATs have different types.\n");
		return NULL;
	}
	return BATcalcifthenelse_intern(b,
					Tloc(b1, b1->batFirst), 1, b1->T->vheap ? b1->T->vheap->base : NULL, b1->T->width, b1->T->nonil,
					Tloc(b2, b2->batFirst), 1, b2->T->vheap ? b2->T->vheap->base : NULL, b2->T->width, b2->T->nonil,
					b1->T->type);
}

BAT *
BATcalcifthenelsecst(BAT *b, BAT *b1, const ValRecord *c2)
{
	BATcheck(b, "BATcalcifthenelsecst", NULL);
	BATcheck(b1, "BATcalcifthenelsecst", NULL);
	BATcheck(c2, "BATcalcifthenelsecst", NULL);

	if (checkbats(b, b1, "BATcalcifthenelse") != GDK_SUCCEED)
		return NULL;
	if (b->T->type != TYPE_bit || b1->T->type != c2->vtype) {
		GDKerror("BATcalcifthenelsecst: \"then\" and \"else\" BATs have different types.\n");
		return NULL;
	}
	return BATcalcifthenelse_intern(b,
					Tloc(b1, b1->batFirst), 1, b1->T->vheap ? b1->T->vheap->base : NULL, b1->T->width, b1->T->nonil,
					VALptr(c2), 0, NULL, 0, !VALisnil(c2),
					b1->T->type);
}

BAT *
BATcalcifthencstelse(BAT *b, const ValRecord *c1, BAT *b2)
{
	BATcheck(b, "BATcalcifthenelsecst", NULL);
	BATcheck(c1, "BATcalcifthenelsecst", NULL);
	BATcheck(b2, "BATcalcifthenelsecst", NULL);

	if (checkbats(b, b2, "BATcalcifthenelse") != GDK_SUCCEED)
		return NULL;
	if (b->T->type != TYPE_bit || b2->T->type != c1->vtype) {
		GDKerror("BATcalcifthencstelse: \"then\" and \"else\" BATs have different types.\n");
		return NULL;
	}
	return BATcalcifthenelse_intern(b,
					VALptr(c1), 0, NULL, 0, !VALisnil(c1),
					Tloc(b2, b2->batFirst), 1, b2->T->vheap ? b2->T->vheap->base : NULL, b2->T->width, b2->T->nonil,
					c1->vtype);
}

BAT *
BATcalcifthencstelsecst(BAT *b, const ValRecord *c1, const ValRecord *c2)
{
	BATcheck(b, "BATcalcifthenelsecst", NULL);
	BATcheck(c1, "BATcalcifthenelsecst", NULL);
	BATcheck(c2, "BATcalcifthenelsecst", NULL);

	if (checkbats(b, NULL, "BATcalcifthenelse") != GDK_SUCCEED)
		return NULL;
	if (b->T->type != TYPE_bit || c1->vtype != c2->vtype) {
		GDKerror("BATcalcifthencstelsecst: \"then\" and \"else\" BATs have different types.\n");
		return NULL;
	}
	return BATcalcifthenelse_intern(b,
					VALptr(c1), 0, NULL, 0, !VALisnil(c1),
					VALptr(c2), 0, NULL, 0, !VALisnil(c2),
					c1->vtype);
}

/* ---------------------------------------------------------------------- */
/* type conversion (cast) */

/* a note on the return values from the internal conversion functions:
 *
 * the functions return the number of NIL values produced (or at
 * least, 0 if no NIL, and != 0 if there were any;
 * the return value is BUN_NONE if there was overflow and a message
 * was generated;
 * the return value is BUN_NONE + 1 if the types were not compatible;
 * the return value is BUN_NONE + 2 if inserting a value into a BAT
 * failed (only happens for conversion to str).
 */

#define convertimpl_copy(TYPE)						\
static BUN								\
convert_##TYPE##_##TYPE(const TYPE *src, TYPE *restrict dst, BUN cnt,	\
			BUN start, BUN end, const oid *restrict cand,	\
			const oid *candend, oid candoff)		\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, TYPE##_nil, 0, start);				\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, TYPE##_nil);			\
		nils += src[i] == TYPE##_nil;				\
		dst[i] = src[i];					\
	}								\
	CANDLOOP(dst, i, TYPE##_nil, end, cnt);				\
	return nils;							\
}

#define convertimpl_enlarge(TYPE1, TYPE2)				\
static BUN								\
convert_##TYPE1##_##TYPE2(const TYPE1 *src, TYPE2 *restrict dst, BUN cnt, \
			  BUN start, BUN end, const oid *restrict cand,	\
			  const oid *candend, oid candoff)		\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, TYPE2##_nil, 0, start);			\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, TYPE2##_nil);		\
		if (src[i] == TYPE1##_nil) {				\
			dst[i] = TYPE2##_nil;				\
			nils++;						\
		} else							\
			dst[i] = (TYPE2) src[i];			\
	}								\
	CANDLOOP(dst, i, TYPE2##_nil, end, cnt);			\
	return nils;							\
}

#define CONV_OVERFLOW(TYPE1, TYPE2, value)				\
	do {								\
		GDKerror("22003!overflow in conversion of "		\
			 FMT##TYPE1 " to %s.\n", CST##TYPE1 (value), TYPE2); \
		return BUN_NONE;					\
	} while (0)

#define convertimpl_oid_enlarge(TYPE1)					\
static BUN								\
convert_##TYPE1##_oid(const TYPE1 *src, oid *restrict dst, BUN cnt,	\
		      BUN start, BUN end, const oid *restrict cand,	\
		      const oid *candend, oid candoff,			\
		      int abort_on_error)				\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, oid_nil, 0, start);				\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, oid_nil);			\
		if (src[i] == TYPE1##_nil) {				\
			dst[i] = oid_nil;				\
			nils++;						\
		} else if (src[i] < 0) {				\
			if (abort_on_error)				\
				CONV_OVERFLOW(TYPE1, "oid", src[i]);	\
			dst[i] = oid_nil;				\
			nils++;						\
		} else if ((dst[i] = (oid) src[i]) == oid_nil &&	\
			   abort_on_error)				\
			CONV_OVERFLOW(TYPE1, "oid", src[i]);		\
	}								\
	CANDLOOP(dst, i, oid_nil, end, cnt);				\
	return nils;							\
}

#define convertimpl_oid_reduce(TYPE1)					\
static BUN								\
convert_##TYPE1##_oid(const TYPE1 *src, oid *restrict dst, BUN cnt,	\
		      BUN start, BUN end, const oid *restrict cand,	\
		      const oid *candend, oid candoff,			\
		      int abort_on_error)				\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, oid_nil, 0, start);				\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, oid_nil);			\
		if (src[i] == TYPE1##_nil) {				\
			dst[i] = oid_nil;				\
			nils++;						\
		} else if (src[i] < 0 ||				\
			   src[i] > (TYPE1) GDK_oid_max) {		\
			if (abort_on_error)				\
				CONV_OVERFLOW(TYPE1, "oid", src[i]);	\
			dst[i] = oid_nil;				\
			nils++;						\
		} else if ((dst[i] = (oid) src[i]) == oid_nil &&	\
			   abort_on_error)				\
			CONV_OVERFLOW(TYPE1, "oid", src[i]);		\
	}								\
	CANDLOOP(dst, i, oid_nil, end, cnt);				\
	return nils;							\
}

#define convertimpl_reduce(TYPE1, TYPE2)				\
static BUN								\
convert_##TYPE1##_##TYPE2(const TYPE1 *src, TYPE2 *restrict dst, BUN cnt, \
			  BUN start, BUN end, const oid *restrict cand,	\
			  const oid *candend, oid candoff,		\
			  int abort_on_error)				\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, TYPE2##_nil, 0, start);			\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, TYPE2##_nil);		\
		if (src[i] == TYPE1##_nil) {				\
			dst[i] = TYPE2##_nil;				\
				nils++;					\
		} else if (src[i] <= (TYPE1) GDK_##TYPE2##_min ||	\
			   src[i] > (TYPE1) GDK_##TYPE2##_max) {	\
			if (abort_on_error)				\
				CONV_OVERFLOW(TYPE1, #TYPE2, src[i]);	\
			dst[i] = TYPE2##_nil;				\
			nils++;						\
		} else							\
			dst[i] = (TYPE2) src[i];			\
	}								\
	CANDLOOP(dst, i, TYPE2##_nil, end, cnt);			\
	return nils;							\
}

/* Special version of the above for converting from floating point.
 * The final assignment rounds the value which can still come out to
 * the NIL representation, so we need to check for that. */
#define convertimpl_reduce_float(TYPE1, TYPE2)				\
static BUN								\
convert_##TYPE1##_##TYPE2(const TYPE1 *src, TYPE2 *restrict dst, BUN cnt, \
			  BUN start, BUN end, const oid *restrict cand,	\
			  const oid *candend, oid candoff,		\
			  int abort_on_error)				\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, TYPE2##_nil, 0, start);			\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, TYPE2##_nil);		\
		if (src[i] == TYPE1##_nil) {				\
			dst[i] = TYPE2##_nil;				\
			nils++;						\
		} else if (src[i] <= (TYPE1) GDK_##TYPE2##_min ||	\
			   src[i] > (TYPE1) GDK_##TYPE2##_max) {	\
			if (abort_on_error)				\
				CONV_OVERFLOW(TYPE1, #TYPE2, src[i]);	\
			dst[i] = TYPE2##_nil;				\
			nils++;						\
		} else if ((dst[i] = (TYPE2) src[i]) == TYPE2##_nil &&	\
			   abort_on_error)				\
			CONV_OVERFLOW(TYPE1, #TYPE2, src[i]);		\
	}								\
	CANDLOOP(dst, i, TYPE2##_nil, end, cnt);			\
	return nils;							\
}

#define convert2bit_impl(TYPE)						\
static BUN								\
convert_##TYPE##_bit(const TYPE *src, bit *restrict dst, BUN cnt,	\
		     BUN start, BUN end, const oid *restrict cand,	\
		     const oid *candend, oid candoff)			\
{									\
	BUN i, nils = 0;						\
									\
	CANDLOOP(dst, i, bit_nil, 0, start);				\
	for (i = start; i < end; i++) {					\
		CHECKCAND(dst, i, candoff, bit_nil);			\
		if (src[i] == TYPE##_nil) {				\
			dst[i] = bit_nil;				\
			nils++;						\
		} else							\
			dst[i] = (bit) (src[i] != 0);			\
	}								\
	CANDLOOP(dst, i, bit_nil, end, cnt);				\
	return nils;							\
}

convertimpl_copy(bte)
convertimpl_enlarge(bte, sht)
convertimpl_enlarge(bte, int)
convertimpl_oid_enlarge(bte)
convertimpl_enlarge(bte, lng)
#ifdef HAVE_HGE
convertimpl_enlarge(bte, hge)
#endif
convertimpl_enlarge(bte, flt)
convertimpl_enlarge(bte, dbl)

convertimpl_reduce(sht, bte)
convertimpl_copy(sht)
convertimpl_enlarge(sht, int)
convertimpl_oid_enlarge(sht)
convertimpl_enlarge(sht, lng)
#ifdef HAVE_HGE
convertimpl_enlarge(sht, hge)
#endif
convertimpl_enlarge(sht, flt)
convertimpl_enlarge(sht, dbl)

convertimpl_reduce(int, bte)
convertimpl_reduce(int, sht)
convertimpl_copy(int)
convertimpl_oid_enlarge(int)
convertimpl_enlarge(int, lng)
#ifdef HAVE_HGE
convertimpl_enlarge(int, hge)
#endif
convertimpl_enlarge(int, flt)
convertimpl_enlarge(int, dbl)

convertimpl_reduce(lng, bte)
convertimpl_reduce(lng, sht)
convertimpl_reduce(lng, int)
#if SIZEOF_OID == SIZEOF_LNG
convertimpl_oid_enlarge(lng)
#else
convertimpl_oid_reduce(lng)
#endif
convertimpl_copy(lng)
#ifdef HAVE_HGE
convertimpl_enlarge(lng, hge)
#endif
convertimpl_enlarge(lng, flt)
convertimpl_enlarge(lng, dbl)

#ifdef HAVE_HGE
convertimpl_reduce(hge, bte)
convertimpl_reduce(hge, sht)
convertimpl_reduce(hge, int)
convertimpl_oid_reduce(hge)
convertimpl_reduce(hge, lng)
convertimpl_copy(hge)
convertimpl_enlarge(hge, flt)
convertimpl_enlarge(hge, dbl)
#endif

convertimpl_reduce_float(flt, bte)
convertimpl_reduce_float(flt, sht)
convertimpl_reduce_float(flt, int)
convertimpl_oid_reduce(flt)
convertimpl_reduce_float(flt, lng)
#ifdef HAVE_HGE
convertimpl_reduce_float(flt, hge)
#endif
convertimpl_copy(flt)
convertimpl_enlarge(flt, dbl)

convertimpl_reduce_float(dbl, bte)
convertimpl_reduce_float(dbl, sht)
convertimpl_reduce_float(dbl, int)
convertimpl_oid_reduce(dbl)
convertimpl_reduce_float(dbl, lng)
#ifdef HAVE_HGE
convertimpl_reduce_float(dbl, hge)
#endif
convertimpl_reduce_float(dbl, flt)
convertimpl_copy(dbl)

convert2bit_impl(bte)
convert2bit_impl(sht)
convert2bit_impl(int)
convert2bit_impl(lng)
#ifdef HAVE_HGE
convert2bit_impl(hge)
#endif
convert2bit_impl(flt)
convert2bit_impl(dbl)

static BUN
convert_any_str(int tp, const void *src, BAT *bn, BUN cnt,
		BUN start, BUN end, const oid *restrict cand,
		const oid *candend, oid candoff)
{
	str dst = 0;
	int len = 0;
	BUN nils = 0;
	BUN i;
	void *nil = ATOMnilptr(tp);
	int (*atomtostr)(str *, int *, const void *) = BATatoms[tp].atomToStr;
	int size = ATOMsize(tp);

	for (i = 0; i < start; i++)
		tfastins_nocheck(bn, i, str_nil, bn->T->width);
	for (i = start; i < end; i++) {
		if (cand) {
			if (i < *cand - candoff) {
				nils++;
				tfastins_nocheck(bn, i, str_nil, bn->T->width);
				continue;
			}
			assert(i == *cand - candoff);
			if (++cand == candend)
				end = i + 1;
		}
		(*atomtostr)(&dst, &len, src);
		if (ATOMcmp(tp, src, nil) == 0)
			nils++;
		tfastins_nocheck(bn, i, dst, bn->T->width);
		src = (const void *) ((const char *) src + size);
	}
	for (i = end; i < cnt; i++)
		tfastins_nocheck(bn, i, str_nil, bn->T->width);
	BATsetcount(bn, cnt);
	if (dst)
		GDKfree(dst);
	return nils;
  bunins_failed:
	if (dst)
		GDKfree(dst);
	return BUN_NONE + 2;
}

static BUN
convert_str_any(BAT *b, int tp, void *restrict dst,
		BUN start, BUN end, const oid *restrict cand,
		const oid *candend, oid candoff, int abort_on_error)
{
	BUN i, cnt = BATcount(b);
	BUN nils = 0;
	const void *nil = ATOMnilptr(tp);
	char *s;
	void *d;
	int len = ATOMsize(tp);
	int l;
	int (*atomfromstr)(const char *, int *, ptr *) = BATatoms[tp].atomFromStr;
	BATiter bi = bat_iterator(b);

	for (i = 0; i < start; i++) {
		memcpy(dst, nil, len);
		dst = (void *) ((char *) dst + len);
	}
	nils += start;
	for (i = start; i < end; i++, dst = (void *) ((char *) dst + len)) {
		if (cand) {
			if (i < *cand - candoff) {
				nils++;
				memcpy(dst, nil, len);
				continue;
			}
			assert(i == *cand - candoff);
			if (++cand == candend)
				end = i + 1;
		}
		s = BUNtail(bi, i);
		if (s[0] == str_nil[0] && s[1] == str_nil[1]) {
			memcpy(dst, nil, len);
			nils++;
		} else {
			d = dst;
			if ((l = (*atomfromstr)(s, &len, &d)) <= 0 ||
			    l < (int) strlen(s)) {
				if (abort_on_error) {
					GDKerror("22018!conversion of string "
						 "'%s' to type %s failed.\n",
						 s, ATOMname(tp));
					return BUN_NONE;
				}
				memcpy(dst, nil, len);
			}
			assert(len == ATOMsize(tp));
			if (ATOMcmp(tp, dst, nil) == 0)
				nils++;
		}
	}
	for (i = end; i < cnt; i++) {
		memcpy(dst, nil, len);
		dst = (void *) ((char *) dst + len);
	}
	nils += cnt - end;
	return nils;
}

static BUN
convert_void_any(oid seq, BUN cnt, BAT *bn,
		 BUN start, BUN end, const oid *restrict cand,
		 const oid *candend, oid candoff, int abort_on_error)
{
	BUN nils = 0;
	BUN i = 0;
	int tp = bn->T->type;
	void *restrict dst = Tloc(bn, bn->batFirst);
	int (*atomtostr)(str *, int *, const void *) = BATatoms[TYPE_oid].atomToStr;
	str s = 0;
	int len = 0;

	if (seq == oid_nil) {
		start = end = 0;
	} else {
		/* use nils temporarily as scratch variable */
		if (ATOMsize(tp) < ATOMsize(TYPE_oid) &&
		    seq + cnt >= (oid) 1 << (8 * ATOMsize(tp) - 1)) {
			/* overflow */
			if (abort_on_error)
				CONV_OVERFLOW(oid, ATOMname(tp), seq + cnt);
			nils = ((oid) 1 << (8 * ATOMsize(tp) - 1)) - seq;
		} else {
			nils = cnt;
		}
		if (nils < end)
			end = nils;
		/* start using nils normally */
		nils = 0;
		switch (ATOMbasetype(tp)) {
		case TYPE_bte:
			CANDLOOP((bte *) dst, i, bte_nil, 0, start);
			if (tp == TYPE_bit) {
				/* only the first one could be 0 */
				if (i == 0 && i < end && seq == 0 &&
				    (cand == NULL || *cand != candoff)) {
					((bte *) dst)[0] = 0;
					i++;
				}
				for (; i < end; i++) {
					CHECKCAND((bte *) dst, i, candoff,
						  bte_nil);
					((bte *) dst)[i] = 1;
				}
			} else {
				for (i = 0; i < end; i++, seq++) {
					CHECKCAND((bte *) dst, i, candoff,
						  bte_nil);
					((bte *) dst)[i] = (bte) seq;
				}
			}
			break;
		case TYPE_sht:
			CANDLOOP((sht *) dst, i, sht_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((sht *) dst, i, candoff, sht_nil);
				((sht *) dst)[i] = (sht) seq;
			}
			break;
		case TYPE_int:
			CANDLOOP((int *) dst, i, int_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((int *) dst, i, candoff, int_nil);
				((int *) dst)[i] = (int) seq;
			}
			break;
		case TYPE_lng:
			CANDLOOP((lng *) dst, i, lng_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((lng *) dst, i, candoff, lng_nil);
				((lng *) dst)[i] = (lng) seq;
			}
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			CANDLOOP((hge *) dst, i, hge_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((hge *) dst, i, candoff, hge_nil);
				((hge *) dst)[i] = (hge) seq;
			}
			break;
#endif
		case TYPE_flt:
			CANDLOOP((flt *) dst, i, flt_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((flt *) dst, i, candoff, flt_nil);
				((flt *) dst)[i] = (flt) seq;
			}
			break;
		case TYPE_dbl:
			CANDLOOP((dbl *) dst, i, dbl_nil, 0, start);
			for (i = start; i < end; i++, seq++) {
				CHECKCAND((dbl *) dst, i, candoff, dbl_nil);
				((dbl *) dst)[i] = (dbl) seq;
			}
			break;
		case TYPE_str:
			for (i = 0; i < start; i++)
				tfastins_nocheck(bn, i, str_nil, bn->T->width);
			for (i = 0; i < end; i++) {
				if (cand) {
					if (i < *cand - candoff) {
						nils++;
						tfastins_nocheck(bn, i, str_nil,
								 bn->T->width);
						continue;
					}
					assert(i == *cand - candoff);
					if (++cand == candend)
						end = i + 1;
				}
				(*atomtostr)(&s, &len, &seq);
				tfastins_nocheck(bn, i, s, bn->T->width);
				seq++;
			}
			break;
		default:
			/* dealt with below */
			break;
		}
	}
	switch (ATOMbasetype(tp)) {
	case TYPE_bte:
		for (; i < cnt; i++)
			((bte *) dst)[i] = bte_nil;
		break;
	case TYPE_sht:
		for (; i < cnt; i++)
			((sht *) dst)[i] = sht_nil;
		break;
	case TYPE_int:
		for (; i < cnt; i++)
			((int *) dst)[i] = int_nil;
		break;
	case TYPE_lng:
		for (; i < cnt; i++)
			((lng *) dst)[i] = lng_nil;
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		for (; i < cnt; i++)
			((hge *) dst)[i] = hge_nil;
		break;
#endif
	case TYPE_flt:
		for (; i < cnt; i++)
			((flt *) dst)[i] = flt_nil;
		break;
	case TYPE_dbl:
		for (; i < cnt; i++)
			((dbl *) dst)[i] = dbl_nil;
		break;
	case TYPE_str:
		seq = oid_nil;
		(*atomtostr)(&s, &len, &seq);
		for (; i < cnt; i++) {
			tfastins_nocheck(bn, i, s, bn->T->width);
		}
		break;
	default:
		return BUN_NONE + 1;
	}
	nils += cnt - end;
	if (s)
		GDKfree(s);
	return nils;

  bunins_failed:
	if (s)
		GDKfree(s);
	return BUN_NONE + 2;
}

static BUN
convert_typeswitchloop(const void *src, int stp, void *restrict dst, int dtp,
		       BUN cnt, BUN start, BUN end, const oid *restrict cand,
		       const oid *candend, oid candoff, int abort_on_error)
{
	switch (ATOMbasetype(stp)) {
	case TYPE_bte:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit)
				return convert_bte_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			return convert_bte_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_sht:
			return convert_bte_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_bte_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_bte_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_bte_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_bte_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_bte_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#endif
		case TYPE_flt:
			return convert_bte_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_bte_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
	case TYPE_sht:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit)
				return convert_sht_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			return convert_sht_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_sht_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_sht_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_sht_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_sht_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_sht_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_sht_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#endif
		case TYPE_flt:
			return convert_sht_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_sht_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
	case TYPE_int:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit) {
				return convert_int_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			}
			return convert_int_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_int_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_int_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_int_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_int_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_int_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_int_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#endif
		case TYPE_flt:
			return convert_int_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_int_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
	case TYPE_lng:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit) {
				return convert_lng_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			}
			return convert_lng_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_lng_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_lng_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_lng_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_lng_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_lng_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_lng_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
#endif
		case TYPE_flt:
			return convert_lng_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_lng_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
#ifdef HAVE_HGE
	case TYPE_hge:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit) {
				return convert_hge_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			}
			return convert_hge_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_hge_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_int:
			return convert_hge_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_lng:
			return convert_hge_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_hge:
			return convert_hge_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_oid:
			return convert_hge_oid(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_flt:
			return convert_hge_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_hge_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
#endif
	case TYPE_flt:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit) {
				return convert_flt_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			}
			return convert_flt_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_flt_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_flt_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_flt_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_flt_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_flt_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_flt_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
#endif
		case TYPE_flt:
			return convert_flt_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		case TYPE_dbl:
			return convert_flt_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
	case TYPE_dbl:
		switch (ATOMbasetype(dtp)) {
		case TYPE_bte:
			if (dtp == TYPE_bit) {
				return convert_dbl_bit(src, dst, cnt,
						       start, end, cand,
						       candend, candoff);
			}
			return convert_dbl_bte(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_sht:
			return convert_dbl_sht(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_int:
#if SIZEOF_OID == SIZEOF_INT
			if (dtp == TYPE_oid)
				return convert_dbl_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_dbl_int(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_lng:
#if SIZEOF_OID == SIZEOF_LNG
			if (dtp == TYPE_oid)
				return convert_dbl_oid(src, dst, cnt,
						       start, end, cand,
						       candend, candoff,
						       abort_on_error);
#endif
			return convert_dbl_lng(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
#ifdef HAVE_HGE
		case TYPE_hge:
			return convert_dbl_hge(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
#endif
		case TYPE_flt:
			return convert_dbl_flt(src, dst, cnt,
					       start, end, cand,
					       candend, candoff,
					       abort_on_error);
		case TYPE_dbl:
			return convert_dbl_dbl(src, dst, cnt,
					       start, end, cand,
					       candend, candoff);
		default:
			return BUN_NONE + 1;
		}
	default:
		return BUN_NONE + 1;
	}
}

BAT *
BATconvert(BAT *b, BAT *s, int tp, int abort_on_error)
{
	BAT *bn;
	BUN nils = 0;	/* in case no conversion defined */
	BUN start, end, cnt;
	const oid *restrict cand = NULL, *candend = NULL;

	BATcheck(b, "BATconvert", NULL);
	if (tp == TYPE_void)
		tp = TYPE_oid;

	CANDINIT(b, s, start, end, cnt, cand, candend);

	if (s == NULL && tp != TYPE_bit && ATOMbasetype(b->T->type) == ATOMbasetype(tp))
		return BATcopy(b, b->H->type, tp, 0, TRANSIENT);

	bn = BATnew(TYPE_void, tp, b->batCount, TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (b->T->type == TYPE_void)
		nils = convert_void_any(b->T->seq, b->batCount, bn,
					start, end, cand, candend, b->H->seq,
					abort_on_error);
	else if (tp == TYPE_str)
		nils = convert_any_str(b->T->type, Tloc(b, b->batFirst), bn,
				       cnt, start, end, cand, candend,
				       b->H->seq);
	else if (b->T->type == TYPE_str)
		nils = convert_str_any(b, tp, Tloc(bn, bn->batFirst),
				       start, end, cand, candend, b->H->seq,
				       abort_on_error);
	else
		nils = convert_typeswitchloop(Tloc(b, b->batFirst), b->T->type,
					      Tloc(bn, bn->batFirst), tp,
					      b->batCount, start, end,
					      cand, candend, b->H->seq,
					      abort_on_error);

	if (nils >= BUN_NONE) {
		BBPunfix(bn->batCacheid);
		if (nils == BUN_NONE + 1) {
			GDKerror("BATconvert: type combination (convert(%s)->%s) "
				 "not supported.\n",
				 ATOMname(b->T->type), ATOMname(tp));
		} else if (nils == BUN_NONE + 2) {
			GDKerror("BATconvert: could not insert value into BAT.\n");
		}
		return NULL;
	}

	BATsetcount(bn, b->batCount);
	BATseqbase(bn, b->H->seq);

	bn->T->nil = nils != 0;
	bn->T->nonil = nils == 0;
	if ((bn->T->type != TYPE_bit && b->T->type != TYPE_str) ||
	    BATcount(bn) < 2) {
		bn->T->sorted = nils == 0 && b->T->sorted;
		bn->T->revsorted = nils == 0 && b->T->revsorted;
	} else {
		bn->T->sorted = 0;
		bn->T->revsorted = 0;
	}
	if (bn->T->type != TYPE_bit || BATcount(bn) < 2)
		bn->T->key = (b->T->key & 1) && nils <= 1;
	else
		bn->T->key = 0;

	return bn;
}

gdk_return
VARconvert(ValPtr ret, const ValRecord *v, int abort_on_error)
{
	ptr p;
	BUN nils = 0;

	if (ret->vtype == TYPE_str) {
		if (v->vtype == TYPE_void ||
		    (*ATOMcompare(v->vtype))(VALptr(v),
						  ATOMnilptr(v->vtype)) == 0) {
			ret->val.sval = GDKstrdup(str_nil);
		} else if (v->vtype == TYPE_str) {
			ret->val.sval = GDKstrdup(v->val.sval);
		} else {
			ret->val.sval = NULL;
			(*BATatoms[v->vtype].atomToStr)(&ret->val.sval,
							&ret->len,
							VALptr(v));
		}
	} else if (ret->vtype == TYPE_void) {
		if (abort_on_error &&
		    ATOMcmp(v->vtype, VALptr(v), ATOMnilptr(v->vtype)) != 0) {
			GDKerror("22003!cannot convert non-nil to void.\n");
			nils = BUN_NONE;
		}
		ret->val.oval = oid_nil;
	} else if (v->vtype == TYPE_void) {
		nils = convert_typeswitchloop(&oid_nil, TYPE_oid,
					      VALget(ret), ret->vtype,
					      1, 0, 1, NULL, NULL, 0,
					      abort_on_error);
	} else if (v->vtype == TYPE_str) {
		if (v->val.sval == NULL || strcmp(v->val.sval, str_nil) == 0) {
			nils = convert_typeswitchloop(&bte_nil, TYPE_bte,
						      VALget(ret), ret->vtype,
						      1, 0, 1, NULL, NULL, 0,
						      abort_on_error);
		} else {
			int len;
			p = VALget(ret);
			ret->len = ATOMsize(ret->vtype);
			if ((len = (*BATatoms[ret->vtype].atomFromStr)(
				     v->val.sval, &ret->len, &p)) <= 0 ||
			    len < (int) strlen(v->val.sval)) {
				GDKerror("22018!conversion of string "
					 "'%s' to type %s failed.\n",
					 v->val.sval, ATOMname(ret->vtype));
				nils = BUN_NONE;
			}
			assert(p == VALget(ret));
		}
	} else {
		nils = convert_typeswitchloop(VALptr(v), v->vtype,
					      VALget(ret), ret->vtype,
					      1, 0, 1, NULL, NULL, 0,
					      abort_on_error);
	}
	if (nils == BUN_NONE + 1) {
		GDKerror("VARconvert: conversion from type %s to type %s "
			 "unsupported.\n",
			 ATOMname(v->vtype), ATOMname(ret->vtype));
		return GDK_FAIL;
	}
	return nils == BUN_NONE ? GDK_FAIL : GDK_SUCCEED;
}
