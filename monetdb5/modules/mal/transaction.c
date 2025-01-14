/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/*
 * @a M.L. Kersten, P. Boncz
 * @+ Transaction management
 * In the philosophy of Monet, transaction management overhead should only
 * be paid when necessary. Transaction management is for this purpose
 * implemented as a module.
 * This code base is largely absolute and should be re-considered when
 * serious OLTP is being supported.
 * Note, however, the SQL front-end obeys transaction semantics.
 *
 */
#include "monetdb_config.h"
#include "gdk.h"
#include "mal.h"
#include "mal_interpreter.h"
#include "bat5.h"

#ifdef WIN32
#define transaction_export extern __declspec(dllexport)
#else
#define transaction_export extern
#endif

transaction_export str TRNglobal_sync(bit *ret);
transaction_export str TRNglobal_abort(bit *ret);
transaction_export str TRNglobal_commit(bit *ret);
transaction_export str TRNtrans_clean(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p);
transaction_export str TRNtrans_abort(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p);
transaction_export str TRNtrans_commit(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p);
transaction_export str TRNsubcommit(bit *ret, bat *bid);
transaction_export str TRNtrans_prev(bat *ret, bat *bid);

#include "mal_exception.h"
str
TRNglobal_sync(bit *ret)
{
	*ret = BBPsync(getBBPsize(), NULL) == GDK_SUCCEED;
	return MAL_SUCCEED;
}

str
TRNglobal_abort(bit *ret)
{
	*ret = TMabort() == GDK_SUCCEED;
	return MAL_SUCCEED;
}

str
TRNglobal_commit(bit *ret)
{
	*ret = TMcommit()?FALSE:TRUE;
	return MAL_SUCCEED;
}
str
TRNsubcommit(bit *ret, bat *bid)
{
	BAT *b;
	b= BATdescriptor(*bid);
	if( b == NULL)
		throw(MAL, "transaction.subcommit", RUNTIME_OBJECT_MISSING);
	*ret = TMsubcommit(b) == GDK_SUCCEED;
	BBPunfix(b->batCacheid);
	return MAL_SUCCEED;
}

str
TRNtrans_clean(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p)
{
	int i;
	bat *bid;
	BAT *b;

	(void) cntxt;
	(void) mb;
	for (i = p->retc; i < p->argc; i++) {
		bid = getArgReference_bat(stk, p, i);
		if ((b = BATdescriptor(*bid)) == NULL) {
			throw(MAL, "transaction.commit",  RUNTIME_OBJECT_MISSING);
		}

		BATfakeCommit(b);
		BBPunfix(b->batCacheid);
	}
	return MAL_SUCCEED;
}

str
TRNtrans_abort(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p)
{
	int i;
	bat *bid;
	BAT *b;

	(void) cntxt;
	(void) mb;
	for (i = p->retc; i < p->argc; i++) {
		bid = getArgReference_bat(stk, p, i);
		if ((b = BATdescriptor(*bid)) == NULL) {
			throw(MAL, "transaction.abort",  RUNTIME_OBJECT_MISSING);
		}
		BATundo(b);
		BBPunfix(b->batCacheid);
	}
	return MAL_SUCCEED;
}

str
TRNtrans_commit(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p)
{
	int i;
	bat *bid;
	BAT *b;

	(void) cntxt;
	(void) mb;
	for (i = p->retc; i < p->argc; i++) {
		bid = getArgReference_bat(stk, p, i);
		if ((b = BATdescriptor(*bid)) == NULL) {
			throw(MAL, "transaction.commit",  RUNTIME_OBJECT_MISSING);
		}
		BATcommit(b);
		BBPunfix(b->batCacheid);
	}
	return MAL_SUCCEED;
}

str
TRNtrans_prev(bat *ret, bat *bid)
{
	BAT *b,*bn= NULL;
	b= BATdescriptor(*bid);
	if (b  == NULL)
		throw(MAL, "transaction.prev", RUNTIME_OBJECT_MISSING);
	bn = BATprev(b);
	BBPkeepref(*ret = bn->batCacheid);
	BBPunfix(b->batCacheid);
	return MAL_SUCCEED;
}
