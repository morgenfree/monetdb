/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/* This file is included multiple times (from sql_cast.c).
 * We expect the tokens TP1 & TP2 to be defined by the including file.
 */

/* ! ENSURE THAT THESE LOCAL MACROS ARE UNDEFINED AT THE END OF THIS FILE ! */

/* stringify token */
#define _STRNG_(s) #s
#define STRNG(t) _STRNG_(t)

/* concatenate two or four tokens */
#define CONCAT_2(a,b)     a##b
#define CONCAT_4(a,b,c,d) a##b##c##d

#define NIL(t)       CONCAT_2(t,_nil)
#define TPE(t)       CONCAT_2(TYPE_,t)
#define FUN(a,b,c,d) CONCAT_4(a,b,c,d)


str
FUN(,TP1,_dec2_,TP2) (TP2 *res, const int *s1, const TP1 *v)
{
	int scale = *s1;
	TP2 r;

	/* shortcut nil */
	if (*v == NIL(TP1)) {
		*res = NIL(TP2);
		return (MAL_SUCCEED);
	}

	/* since the TP2 type is bigger than or equal to the TP1 type, it will
	   always fit */
	r = (TP2) *v;
	if (scale)
		r /= scales[scale];
	*res = r;
	return MAL_SUCCEED;
}

str
FUN(,TP1,_dec2dec_,TP2) (TP2 *res, const int *S1, const TP1 *v, const int *d2, const int *S2)
{
	int p = *d2, inlen = 1;
	TP1 cpyval = *v;
	int s1 = *S1, s2 = *S2;
	TP2 r;

	/* shortcut nil */
	if (*v == NIL(TP1)) {
		*res = NIL(TP2);
		return (MAL_SUCCEED);
	}

	/* count the number of digits in the input */
	while (cpyval /= 10)
		inlen++;
	/* rounding is allowed */
	inlen += (s2 - s1);
	if (p && inlen > p) {
		throw(SQL, "convert", "22003!too many digits (%d > %d)", inlen, p);
	}

	/* since the TP2 type is bigger than or equal to the TP1 type, it will
	   always fit */
	r = (TP2) *v;
	if (s2 > s1)
		r *= scales[s2 - s1];
	else if (s2 != s1)
		r /= scales[s1 - s2];
	*res = r;
	return MAL_SUCCEED;
}

str
FUN(,TP1,_num2dec_,TP2) (TP2 *res, const TP1 *v, const int *d2, const int *s2)
{
	int zero = 0;
	return FUN(,TP1,_dec2dec_,TP2)(res, &zero, v, d2, s2);
}

str
FUN(bat,TP1,_dec2_,TP2) (int *res, const int *s1, const int *bid)
{
	BAT *b, *bn;
	TP1 *p, *q;
	char *msg = NULL;
	int scale = *s1;
	TP2 *o;

	if ((b = BATdescriptor(*bid)) == NULL) {
		throw(SQL, "batcalc."STRNG(FUN(,TP1,_dec2_,TP2)), "Cannot access descriptor");
	}
	bn = BATnew(TYPE_void, TPE(TP2), BATcount(b), TRANSIENT);
	if (bn == NULL) {
		BBPunfix(b->batCacheid);
		throw(SQL, "sql."STRNG(FUN(,TP1,_dec2_,TP2)), MAL_MALLOC_FAIL);
	}
	bn->hsorted = b->hsorted;
	bn->hrevsorted = b->hrevsorted;
	BATseqbase(bn, b->hseqbase);
	o = (TP2 *) Tloc(bn, BUNfirst(bn));
	p = (TP1 *) Tloc(b, BUNfirst(b));
	q = (TP1 *) Tloc(b, BUNlast(b));
	bn->T->nonil = 1;
	if (b->T->nonil) {
		for (; p < q; p++, o++)
			*o = (((TP2) *p) / scales[scale]);
	} else {
		for (; p < q; p++, o++) {
			if (*p == NIL(TP1)) {
				*o = NIL(TP2);
				bn->T->nonil = FALSE;
			} else
				*o = (((TP2) *p) / scales[scale]);
		}
	}
	BATsetcount(bn, BATcount(b));
	bn->hrevsorted = bn->batCount <= 1;
	bn->tsorted = 0;
	bn->trevsorted = 0;
	BATkey(BATmirror(bn), FALSE);

	if (!(bn->batDirty & 2))
		BATsetaccess(bn, BAT_READ);

	if (b->htype != bn->htype) {
		BAT *r = VIEWcreate(b, bn);

		BBPkeepref(*res = r->batCacheid);
		BBPunfix(bn->batCacheid);
		BBPunfix(b->batCacheid);
		return msg;
	}
	BBPkeepref(*res = bn->batCacheid);
	BBPunfix(b->batCacheid);
	return msg;
}

str
FUN(bat,TP1,_dec2dec_,TP2) (int *res, const int *S1, const int *bid, const int *d2, const int *S2)
{
	BAT *b, *dst;
	BATiter bi;
	BUN p, q;
	char *msg = NULL;

	if ((b = BATdescriptor(*bid)) == NULL) {
		throw(SQL, "batcalc."STRNG(FUN(,TP1,_dec2dec_,TP2)), "Cannot access descriptor");
	}
	bi = bat_iterator(b);
	dst = BATnew(b->htype, TPE(TP2), BATcount(b), TRANSIENT);
	if (dst == NULL) {
		BBPunfix(b->batCacheid);
		throw(SQL, "sql."STRNG(FUN(,TP1,_dec2dec_,TP2)), MAL_MALLOC_FAIL);
	}
	BATseqbase(dst, b->hseqbase);
	BATloop(b, p, q) {
		TP1 *v = (TP1 *) BUNtail(bi, p);
		TP2 r;
		msg = FUN(,TP1,_dec2dec_,TP2)(&r, S1, v, d2, S2);
		if (msg)
			break;
		BUNins(dst, BUNhead(bi, p), &r, FALSE);
	}
	BBPkeepref(*res = dst->batCacheid);
	BBPunfix(b->batCacheid);
	return msg;
}

str
FUN(bat,TP1,_num2dec_,TP2) (int *res, const int *bid, const int *d2, const int *s2)
{
	BAT *b, *dst;
	BATiter bi;
	BUN p, q;
	char *msg = NULL;

	if ((b = BATdescriptor(*bid)) == NULL) {
		throw(SQL, "batcalc."STRNG(FUN(,TP1,_num2dec_,TP2)), "Cannot access descriptor");
	}
	bi = bat_iterator(b);
	dst = BATnew(b->htype, TPE(TP2), BATcount(b), TRANSIENT);
	if (dst == NULL) {
		BBPunfix(b->batCacheid);
		throw(SQL, "sql."STRNG(FUN(,TP1,_num2dec_,TP2)), MAL_MALLOC_FAIL);
	}
	BATseqbase(dst, b->hseqbase);
	BATloop(b, p, q) {
		TP1 *v = (TP1 *) BUNtail(bi, p);
		TP2 r;
		msg = FUN(,TP1,_num2dec_,TP2)(&r, v, d2, s2);
		if (msg)
			break;
		BUNins(dst, BUNhead(bi, p), &r, FALSE);
	}
	BBPkeepref(*res = dst->batCacheid);
	BBPunfix(b->batCacheid);
	return msg;
}


/* undo local defines */
#undef FUN
#undef NIL
#undef TPE
#undef CONCAT_2
#undef CONCAT_4
#undef STRNG
#undef _STRNG_

