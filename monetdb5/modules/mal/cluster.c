/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/*
 * @a Martin Kersten, Niels Nes
 * @v 1.0
 * @t Hash cluster Algorithms
 *
 * @* Introduction
 * Hash structures in MonetDB are optimized to build in a single scan.
 * Also their use is limited to a single bat. Read optimized hashes (or clusters)
 * could be used in many algorithms, such as select, join, group by and
 * distinct checking. This calls for physical locality of elements
 * hashing to the same element. Preferrable the elements in a collision
 * list are physically close, as are the lists of subsequent lists.
 *
 * This module extends the built in hashing scheme with a method
 * to reorganize BATs based on their hash key. It is a linear-time,
 * near-optimal N-way reclustering based on hash key ranges.
 *
 * We start with collecting all hash keys from the underlying
 * table into the table H[:oid,:oid].
 * The next step is a reclustering step to bring
 * elements together based on the hash key. This step is based
 * on the assumption that values in the hash-key are uniformly
 * distributed. We create as many buckets as we  consider
 * justified in terms of the IO. The tuples are 'thrown' into
 * their bucket until they become full or can be extended
 * by harvesting free space from its direct neighor buckets.
 * If there is no free space left, we circularly look for a bucket
 * with space, partly  polluting the clustering objective.
 *
 * The result is a void-oid table that represents an IO
 * 'optimal' sequence of tuples. IO optimal, because
 * with determining N we have 2N read/write pointers
 * in the table. Tuples are reclustered amongst those
 * using an ordinary join operation.
 * The outerloop touches each tuple once, causing
 * the order in the oid list to represent the IO
 * activity too. This means we can use it directly
 * as a driver for redistributing value columns.
 *
 * The remaining step is to perform this in parallel
 * for all BATs comprising a relational table.
 */
#include "monetdb_config.h"
#include "cluster.h"
#include <mal_exception.h>
#include "algebra.h"

#define CLUSTERKEY(TYPE)						\
static void										\
CLUSTER_key_##TYPE( BAT *map, BAT *b)			\
{												\
	TYPE *bt, *be;								\
	oid *o;										\
												\
	assert(BUNfirst(map) == 0);					\
	assert(BUNfirst(b) == 0);					\
	o = (oid*)Tloc(map, 0);						\
	bt = (TYPE*)Tloc(b, 0);						\
	be = bt + BATcount(b);						\
	for ( ; bt < be; bt++){						\
		BUN h = hash_##TYPE(b->T->hash,bt);		\
		*o++= h;								\
	}											\
}

CLUSTERKEY(bte)
CLUSTERKEY(sht)
CLUSTERKEY(int)
CLUSTERKEY(wrd)
CLUSTERKEY(lng)
#ifdef HAVE_HGE
CLUSTERKEY(hge)
#endif
CLUSTERKEY(oid)
CLUSTERKEY(flt)
CLUSTERKEY(dbl)

static void
CLUSTER_key_str( BAT *map, BAT *b)
{
	char *bt, *be;
	oid *o;
	BUN h;

	assert(BUNfirst(b) == 0);
	assert(BUNfirst(map) == 0);
	o = (oid*)Tloc(map, 0);
	bt = (char *)Tloc(b, 0);
	be = bt + (BATcount(b) << b->T->width);
	switch (b->T->width) {
	case 1:
		for ( ; bt < be; bt += 1){
		/* hash on the string reference */
			h = hash_bte(b->T->hash,bt);
			*o++= h;
		}
		break;
	case 2:
		for ( ; bt < be; bt += 2){
		/* hash on the string reference */
			h = hash_sht(b->T->hash,bt);
			*o++= h;
		}
		break;
#if SIZEOF_VAR_T == 8
	case 4:
		for ( ; bt < be; bt += 4){
		/* hash on the string reference */
			h = hash_int(b->T->hash,bt);
			*o++= h;
		}
		break;
#endif
	default:
		for ( ; bt < be; bt += 8){
		/* hash on the string reference */
			h = hash_lng(b->T->hash,bt);
			*o++= h;
		}
		break;
	}
}
static str
CLUSTER_column_any(BAT *nb, BAT *b, BAT *cmap)
{
	oid *ct, *ce, o = 0;
	BATiter bi= bat_iterator(b);

	ct = (oid *)Tloc(cmap, 0);
	ce = ct + BATcount(cmap);
	nb->H->heap.dirty = nb->T->heap.dirty= TRUE;
	for ( ; ct < ce; ct++){
		BUNfastins(nb, &o, BUNtail(bi, (BUN) *ct));
		o++;
		if ( (o % 1000000) == 0){
			BATsave(nb);
			nb->H->heap.dirty = nb->T->heap.dirty= TRUE;
		}
	}
	BATsetcount(nb, BATcount(b));
	BATderiveProps(nb, 0);
	if (!(nb->batDirty&2))
		BATsetaccess(nb, BAT_READ);
	return MAL_SUCCEED;
}
/*
 * The hash key and the oid are materialized to prepare for reclustering.
 */
str
CLUSTER_key( bat *M, const bat *B){
	BAT *map, *b;

	if ((b = BATdescriptor(*B)) == NULL)
		throw(MAL, "cluster.key", INTERNAL_BAT_ACCESS);
	(void) BAThash(b, 0);		/* only produce the hash structure! */

	if ((map = BATnew(TYPE_void, TYPE_oid, BATcount(b)+1, TRANSIENT)) == NULL) {
		BBPunfix(*B);
		throw(MAL, "cluster.key", MAL_MALLOC_FAIL);
	}
	BATsetcount(map, BATcount(b));
	BATseqbase(map, 0);
	map->tsorted= FALSE;
	map->trevsorted= FALSE;
	map->tdense= FALSE;
	map->H->nonil = b->H->nonil;
	map->T->nonil = b->T->nonil;

	switch(ATOMstorage(b->ttype)) {
		case TYPE_bte: CLUSTER_key_bte(map,b); break;
		case TYPE_sht: CLUSTER_key_sht(map,b); break;
		case TYPE_oid: CLUSTER_key_oid(map,b); break;
		case TYPE_wrd: CLUSTER_key_wrd(map,b); break;
		case TYPE_int: CLUSTER_key_int(map,b); break;
		case TYPE_lng: CLUSTER_key_lng(map,b); break;
#ifdef HAVE_HGE
		case TYPE_hge: CLUSTER_key_hge(map,b); break;
#endif
		case TYPE_flt: CLUSTER_key_flt(map,b); break;
		case TYPE_dbl: CLUSTER_key_dbl(map,b); break;
		case TYPE_str: CLUSTER_key_str(map,b); break;
		default:
			throw(MAL, "cluster.key", MAL_MALLOC_FAIL);

	}
	BATsave(map);	/* dump dirty pages from memory */
	BBPunfix(*B);
	BBPkeepref(*M = map->batCacheid);
	return MAL_SUCCEED;
}
/*
 * Recluster the hash <oid,oid> table into a number of buckets
 * on the high order bits,
 * If the baskets are full before we have moved everything
 * in place, we seek forward for a bucket to dump the elements.
 *
 * The self-organizing version should determine the optimal
 * number of buckets. Thereafter it can just call the
 * remapping;
 */
typedef struct{
		BUN base,limit,nxt;
} Basket;

str
CLUSTER_map(bat *RB, const bat *B)
{
	BUN rng,bsize, bnr=0, h, N= 2; /* number of buckets */
	BAT *b, *map;
	BUN p,q;
	oid *mp, idx = 0, *bp;
	int i;
	Basket *basket;
	(void) RB;

	if ( (b = BATdescriptor(*B)) == NULL)
		throw(MAL, "cluster.new", INTERNAL_BAT_ACCESS);

	if ((map = BATnew(TYPE_void, TYPE_oid, BATcount(b)+1, TRANSIENT)) == NULL) {
		BBPunfix(*B);
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);
	}
	BATsetcount(map, BATcount(b));
	BATseqbase(map, 0);
	BATkey(map, TRUE);
	BATkey(map,TRUE);
	map->hsorted= TRUE;
	map->hrevsorted= FALSE;
	map->hdense= TRUE;
	map->tsorted= FALSE;
	map->trevsorted= FALSE;
	map->tdense= FALSE;
	map->H->nonil = b->H->nonil;
	map->T->nonil = TRUE;
	BATmax(b, (ptr) &rng); /* get the maximum hash key , could use mask !*/
	rng++;
	/*
	 * The key challenge is to determine the number of clusters.
	 * A large number of clusters benefits subsequent performance,
	 * but also challenges the prepare phase. The clustering should
	 * work both for relatively small tables and those that do not
	 * fit in memory.
	 *
	 * The bottomline is the number of elements that fit in a single
	 * diskblock.
	 */
	N= (BUN)MT_npages() /10;
	bsize= (BUN) (MT_pagesize()/sizeof(lng));
	if (N > (rng / bsize))
		N = rng / bsize;
	if ( N ==0) N++;
	bsize= (rng+N-1) / N;
#ifdef _CLUSTER_DEBUG
	N=2; /* for debugging only */
	mnstr_printf(GDKout,"bucket pages %d size %d max %d  N %d\n",
		(int)MT_npages(), (int)bsize, (int)rng, (int)N);
#endif
	basket = (Basket*) GDKzalloc((N+1) * sizeof(Basket));
	if (basket==NULL)
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);

	/* prepare buffers */
	basket[0].base = 0;
	basket[0].limit = BATcount(b) / N;
	basket[0].nxt = BUN_NONE;
	for (h=1; h < N; h++){
		basket[h].base= basket[h-1].limit;
		basket[h].limit= basket[h-1].limit + basket[0].limit;
		basket[h].nxt= BUN_NONE;
	}
	basket[N-1].limit= BATcount(b); /* last piece */

	mp = (oid*) Tloc(map, 0);
	bp = (oid*) Tloc(b, 0);
	BATloop(b,p,q){
		oid ocur = bp[p];

		bnr = ocur/bsize;
		assert(bnr<N);
		if (basket[bnr].base == basket[bnr].limit){ /* full */
			if (basket[bnr].nxt == BUN_NONE ||
				basket[basket[bnr].nxt].base == basket[basket[bnr].nxt].limit){
				/* find a maximal empty slot somewhere*/
				BUN nr, max= (bnr+1) % N;
				nr= bnr;
				i= (int) N;
				do {
					nr= (nr+1) % N;
					if (basket[nr].limit-basket[nr].base >
						basket[max].limit-basket[max].base)
						max = nr;
				} while( --i >=0);
				/* last basket bounds */
				basket[bnr].nxt = max;
				bnr= max;
			} else bnr = basket[bnr].nxt;
		}
		mp[basket[bnr].base] = idx++;
		basket[bnr].base++;
	}
	BBPunfix(*B);
	BBPkeepref(*RB= map->batCacheid);
	GDKfree(basket);
	return MAL_SUCCEED;
}
/*
 * The order of the tuples in the cluster map
 * represent the read/write order. Under the assumption
 * that those read/writes are already localized, it becomes
 * opportune to simply rebuild the clustered column by
 * probing.
 *
 * Extend this operation to accept a sequence of BATs.
 * We change the BAT in place using a temporary copy
 * to guide the move.
 */
str
CLUSTER_apply(bat *bid, BAT *b, BAT *cmap)
{
	BAT *nb;
	assert(b->htype==TYPE_void);
	nb= BATnew(TYPE_void, b->ttype, BATcapacity(b), TRANSIENT);
	if (nb == NULL)
		throw(MAL, "CLUSTER_apply", MAL_MALLOC_FAIL);
	BATseqbase(nb,0);
	nb->hrevsorted = 0;
	nb->tsorted= FALSE;
	nb->trevsorted= FALSE;
	nb->tdense= FALSE;

	/* determine the work for all threads */
	/* to be done, first assume that we can remap in one go */
	assert(BATcount(b)==BATcount(cmap));

	switch(ATOMstorage(b->ttype)) {
/*
	case TYPE_bte: CLUSTER_column_bte(nb, b, cmap);break;
	case TYPE_sht: CLUSTER_column_sht(nb, b, cmap);break;
	case TYPE_oid: CLUSTER_column_oid(nb, b, cmap);break;
	case TYPE_wrd: CLUSTER_column_wrd(nb, b, cmap);break;
	case TYPE_int: CLUSTER_column_int(nb, b, cmap);break;
	case TYPE_lng: CLUSTER_column_lng(nb, b, cmap);break;
#ifdef HAVE_HGE
	case TYPE_hge: CLUSTER_column_hge(nb, b, cmap);break;
#endif
	case TYPE_flt: CLUSTER_column_flt(nb, b, cmap);break;
	case TYPE_dbl: CLUSTER_column_dbl(nb, b, cmap);break;
*/
	default:
		CLUSTER_column_any(nb, b, cmap);
	}
	BBPkeepref(*bid= nb->batCacheid);
	return MAL_SUCCEED;
}

str
CLUSTER_column( Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	bat *res =getArgReference_bat(stk, pci, 0);
	const bat *CMAP =getArgReference_bat(stk, pci, 1);
	const bat *B =getArgReference_bat(stk, pci, 2);
	BAT *cmap = NULL, *b = NULL;
	str msg= MAL_SUCCEED;

	(void) cntxt;
	(void) mb;
	if ( (cmap = BATdescriptor(*CMAP)) == NULL )
		throw(MAL, "cluster.column", INTERNAL_BAT_ACCESS);
	if ( (b = BATdescriptor(*B)) == NULL ){
		BBPunfix(*CMAP);
		throw(MAL, "cluster.column", INTERNAL_BAT_ACCESS);
	}

	msg = CLUSTER_apply(res, b,cmap);
	BBPunfix(*CMAP);
	BBPunfix(b->batCacheid);
	return msg;
}

str
CLUSTER_table( Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	BAT *map,*b;
	bat *res, hid, mid;
	const bat *bid;
	int i;
	str msg= MAL_SUCCEED;
	(void) cntxt;
	(void) mb;

	res =getArgReference_bat(stk, pci, 0);
	bid = getArgReference_bat(stk,pci,pci->retc);
	msg = CLUSTER_key(&hid,bid);
	if (msg)
		return msg;
	msg = CLUSTER_map(&mid,&hid);
	if (msg)
		return msg;
	map = BATdescriptor(mid);
	if (map== NULL)
		throw(MAL,"cluster.table",INTERNAL_BAT_ACCESS);

	for ( i=pci->retc; i<pci->argc; i++){
		bid = getArgReference_bat(stk,pci,i);
		b = BATdescriptor(*bid);
		if ( b== NULL)
			throw(MAL,"cluster.table",INTERNAL_BAT_ACCESS);
		msg = CLUSTER_apply(res, b,map);
		BBPunfix(b->batCacheid);
	}
	*res= mid;
	return msg;
}


#include "cluster.h"
#include <mal_exception.h>

#define CLUSTERCREATE(TYPE)												\
str																		\
CLS_create_##TYPE(bat *rpsum, bat *rcmap, bat *B, int *Bits, int *offset) \
{																		\
	BAT *psum, *cmap, *b;												\
	int i, mask = 0, off = *offset;										\
	int bits = *Bits;													\
	TYPE *bt, *be;														\
	wrd *cnt, *pos, sum, *m;											\
																		\
	if (off < 0)														\
		off = 0;														\
	if (bits >= (int)sizeof(int)*8 || bits < 0)							\
		throw(MAL, "cluster.new", TOO_MANY_BITS);						\
																		\
	if ((bits) != 0)													\
		bits--;															\
	mask = (1<<bits) - 1;												\
	if ((b = BATdescriptor(*B)) == NULL)								\
		throw(MAL, "cluster.new", INTERNAL_BAT_ACCESS);					\
																		\
	if ((psum = BATnew(TYPE_void, TYPE_wrd, mask+1, TRANSIENT)) == NULL) { \
		BBPunfix(*B);													\
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);						\
	}																	\
	BATsetcount(psum, mask+1);											\
	BATseqbase(psum,0);													\
	psum->tsorted= TRUE;												\
	psum->trevsorted= FALSE;											\
	psum->tdense= FALSE;												\
	cnt = (wrd*)Tloc(psum, BUNfirst(psum));								\
	for (i=0 ; i <= mask; i++)											\
		cnt[i] = 0;														\
																		\
	bt = (TYPE*)Tloc(b, BUNfirst(b));									\
	be = bt + BATcount(b);												\
	/* First make a histogram */										\
	for ( ; bt < be; bt++) {											\
		int h = (((int)(*bt)) >> off) & mask;							\
		cnt[h]++;														\
	}																	\
																		\
	/* convert histogram into prefix sum */								\
	pos = (wrd*)GDKzalloc(sizeof(wrd) * (mask+1));						\
	if( pos == NULL){													\
		BBPunfix(*B);													\
		BBPunfix(psum->batCacheid);										\
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);						\
	}																	\
	for (sum = 0, i=0 ; i <= mask; i++) {								\
		wrd psum = sum;													\
																		\
		sum += cnt[i];													\
		pos[i] = cnt[i] = psum;											\
	}																	\
																		\
	/* time to create the cluster map */								\
	if ((cmap = BATnew(TYPE_void, TYPE_wrd, BATcount(b), TRANSIENT)) == NULL) {	\
		BBPunfix(*B);													\
		BBPunfix(psum->batCacheid);										\
		GDKfree(pos);													\
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);						\
	}																	\
	BATsetcount(cmap, BATcount(b));										\
	BATseqbase(cmap, b->H->seq);										\
	cmap->tsorted= FALSE;												\
	cmap->trevsorted= FALSE;											\
	cmap->tdense= FALSE;												\
	m = (wrd*)Tloc(cmap, BUNfirst(cmap));								\
																		\
	bt = (TYPE*)Tloc(b, BUNfirst(b));									\
	be = bt + BATcount(b);												\
	for ( ; bt < be; ) {												\
		int h = (((int)(*bt++)) >> off) & mask;							\
		*m++ = pos[h]++;												\
	}																	\
																		\
	GDKfree(pos);														\
	BBPunfix(*B);														\
	BBPkeepref(*rpsum = psum->batCacheid);								\
	BBPkeepref(*rcmap = cmap->batCacheid);								\
	BATsetaccess(psum, BAT_READ);										\
	BATsetaccess(cmap, BAT_READ);										\
	return MAL_SUCCEED;													\
}

CLUSTERCREATE(bte)
CLUSTERCREATE(sht)
CLUSTERCREATE(int)
CLUSTERCREATE(wrd)
CLUSTERCREATE(lng)
#ifdef HAVE_HGE
CLUSTERCREATE(hge)
#endif
CLUSTERCREATE(flt)
CLUSTERCREATE(dbl)

#define CLUSTERCREATE2(TYPE)											\
str																		\
CLS_create2_##TYPE(bat *rpsum, bat *rcmap, bat *B, int *Bits, int *offset, bit *order) \
{																		\
	BAT *psum, *cmap, *b;												\
	int i, mask = 0, off = *offset;										\
	int bits = *Bits;													\
	TYPE *bt, *be, *bs;													\
	wrd *cnt, sum;														\
																		\
	if (off < 0)														\
		off = 0;														\
	if (bits >= (int)sizeof(int)*8 || bits < 0)							\
		throw(MAL, "cluster.new", TOO_MANY_BITS);						\
																		\
	if (bits != 0)														\
		bits--;															\
	mask = (1<<bits) - 1;												\
	if ((b = BATdescriptor(*B)) == NULL)								\
		throw(MAL, "cluster.new", INTERNAL_BAT_ACCESS);					\
																		\
	if ((psum = BATnew(TYPE_void, TYPE_wrd, mask+1, TRANSIENT)) == NULL) { \
		BBPunfix(*B);													\
		throw(MAL, "cluster.new", MAL_MALLOC_FAIL);						\
	}																	\
	BATsetcount(psum, mask+1);											\
	BATseqbase(psum,0);													\
	psum->tsorted= TRUE;												\
	psum->trevsorted= FALSE;											\
	psum->tdense= FALSE;												\
	cnt = (wrd*)Tloc(psum, BUNfirst(psum));								\
	for (i=0 ; i <= mask; i++)											\
		cnt[i] = 0;														\
																		\
	bs = bt = (TYPE*)Tloc(b, BUNfirst(b));								\
	be = bt + BATcount(b);												\
																		\
	/* Make a histogram and fill the cluster map */						\
	if (b->tsorted) {													\
		bte *mb, *m, h;													\
																		\
		/* time to create the cluster map */							\
		if ((cmap = BATnew((!*order)?TYPE_void:TYPE_oid, TYPE_bte, BATcount(b), TRANSIENT)) == NULL) { \
			BBPunfix(*B);												\
			BBPunfix(psum->batCacheid);									\
			throw(MAL, "cluster.new", MAL_MALLOC_FAIL);					\
		}																\
		BATseqbase(cmap, b->H->seq);									\
		cmap->tdense = FALSE;											\
		mb = m = (bte*)Tloc(cmap, BUNfirst(cmap));						\
																		\
		if (!*order) {													\
			cmap->tsorted = FALSE;										\
			cmap->trevsorted = FALSE;									\
			for ( ; bt < be; bt++) {									\
				int h = (((int)(*bt)) >> off) & mask;					\
			   	*m++ = h;												\
				cnt[h]++;												\
			}															\
		} else { /* try an optimized distribution, 1/Nth in each part */ \
			oid *o, base;												\
			lng sz = 0, parts = mask+1, psz = BATcount(b)/parts;		\
			TYPE prev = *bt - 1;										\
			h = -1;														\
																		\
			cmap->hdense= FALSE;										\
			base = b->hseqbase;											\
			o = (oid*)Hloc(cmap, BUNfirst(cmap));						\
			for ( ; bt < be; bt++, sz++) {								\
				if (prev != *bt && sz >= (h+1)*psz && h < (parts-1)) {	\
					h++;												\
					assert(base + bt - bs >= 0);						\
					assert(base + bt - bs <= (ptrdiff_t) GDK_oid_max);	\
					*o++ = (oid) (base + bt - bs);						\
			   		*m++ = h;											\
				}														\
				cnt[h]++;												\
				prev = *bt;												\
			}															\
		}																\
		assert(m - mb >= 0);											\
		assert((lng) (m - mb) <= (lng) BUN_MAX);						\
		BATsetcount(cmap, (BUN) (m - mb));								\
	} else {															\
		bte *m;															\
																		\
		/* time to create the cluster map */							\
		if ((cmap = BATnew(TYPE_void, TYPE_bte, BATcount(b), TRANSIENT)) == NULL) { \
			BBPunfix(*B);												\
			BBPunfix(psum->batCacheid);									\
			throw(MAL, "cluster.new", MAL_MALLOC_FAIL);					\
		}																\
		BATsetcount(cmap, BATcount(b));									\
		BATseqbase(cmap, b->H->seq);									\
		cmap->tsorted = FALSE;											\
		cmap->trevsorted = FALSE;										\
		cmap->tdense = FALSE;											\
		m = (bte*)Tloc(cmap, BUNfirst(cmap));							\
																		\
		for ( ; bt < be; bt++) {										\
			int h = (((int)(*bt)) >> off) & mask;						\
			cnt[h]++;													\
			*m++ = h;													\
		}																\
	}																	\
																		\
	/* convert histogram into prefix sum */								\
	for (sum = 0, i=0 ; i <= mask; i++) {								\
		wrd psum = sum;													\
																		\
		sum += cnt[i];													\
		cnt[i] = psum;													\
	}																	\
																		\
	BBPunfix(*B);														\
	BBPkeepref(*rpsum = psum->batCacheid);								\
	BBPkeepref(*rcmap = cmap->batCacheid);								\
	BATsetaccess(psum, BAT_READ);										\
	BATsetaccess(cmap, BAT_READ);										\
	return MAL_SUCCEED;													\
}

CLUSTERCREATE2(bte)
CLUSTERCREATE2(sht)
CLUSTERCREATE2(int)
CLUSTERCREATE2(wrd)
CLUSTERCREATE2(lng)
#ifdef HAVE_HGE
CLUSTERCREATE2(hge)
#endif
CLUSTERCREATE2(flt)
CLUSTERCREATE2(dbl)

#define CLSMAP(TYPE)							\
static str										\
CLS_map_##TYPE(BAT *rb, BAT *cmap, BAT *b)		\
{												\
	wrd *m;										\
	TYPE *r, *bt, *be;							\
												\
	r = (TYPE*)Tloc(rb, BUNfirst(rb));			\
	m = (wrd*)Tloc(cmap, BUNfirst(cmap));		\
	bt = (TYPE*)Tloc(b, BUNfirst(b));			\
	be = bt + BATcount(b);						\
	for ( ; bt < be; )							\
		r[*m++] = *bt++;						\
	BBPunfix(cmap->batCacheid);					\
	BBPunfix(b->batCacheid);					\
	BBPkeepref(rb->batCacheid);					\
	BATsetaccess(rb, BAT_READ);					\
	return MAL_SUCCEED;							\
}

CLSMAP(bte)
CLSMAP(sht)
CLSMAP(int)
CLSMAP(lng)
#ifdef HAVE_HGE
CLSMAP(hge)
#endif

#define CLSMAP2(TYPE)									\
static str												\
CLS_map2_##TYPE(BAT *rb, wrd *psum, BAT *cmap, BAT *b)	\
{														\
	TYPE *m;											\
	TYPE *r, *bt, *be;									\
														\
	r = (TYPE*)Tloc(rb, BUNfirst(rb));					\
	m = (TYPE*)Tloc(cmap, BUNfirst(cmap));				\
	bt = (TYPE*)Tloc(b, BUNfirst(b));					\
	be = bt + BATcount(b);								\
	for ( ; bt < be; )									\
		r[psum[*m++]++] = *bt++;						\
	GDKfree(psum);										\
	BBPunfix(cmap->batCacheid);							\
	BBPunfix(b->batCacheid);							\
	BBPkeepref(rb->batCacheid);							\
	BATsetaccess(rb, BAT_READ);							\
	return MAL_SUCCEED;									\
}

CLSMAP2(bte)
CLSMAP2(sht)
CLSMAP2(int)
CLSMAP2(lng)
#ifdef HAVE_HGE
CLSMAP2(hge)
#endif

str
CLS_map(bat *RB, bat *CMAP, bat *B)
{
	BATiter bi;
	BAT *rb, *cmap = NULL, *b = NULL;
	BUN i = 0, bf;
	wrd *m;

	if ((cmap = BATdescriptor(*CMAP)) == NULL ||
	    (b = BATdescriptor(*B)) == NULL) {
		if (cmap)
			BBPunfix(*CMAP);
		throw(MAL, "cluster.map", INTERNAL_BAT_ACCESS);
	}
	if (BATcount(cmap) != BATcount(b) ||
	    cmap->H->seq != b->H->seq) {
		BBPunfix(*CMAP);
		BBPunfix(*B);
			throw(MAL, "cluster.map", OPERATION_FAILED " Counts of operands do not match");
	}

	if ((rb = BATnew(TYPE_void, b->ttype, BATcount(b), TRANSIENT)) == NULL) {
		BBPunfix(*CMAP);
		BBPunfix(*B);
		throw(MAL, "cluster.map", MAL_MALLOC_FAIL);
	}
	BATsetcount(rb, BATcount(b));
	BATseqbase(rb, b->H->seq);
	rb->tsorted= FALSE;
	rb->trevsorted= FALSE;
	rb->tdense= FALSE;
	rb->H->nonil = b->H->nonil;
	rb->T->nonil = b->T->nonil;
	*RB = rb->batCacheid;

	switch(ATOMstorage(b->ttype)) {
	case TYPE_bte:
			return CLS_map_bte(rb, cmap, b);
	case TYPE_sht:
			return CLS_map_sht(rb, cmap, b);
#if SIZEOF_WRD == SIZEOF_INT
	case TYPE_wrd:
#endif
#if SIZEOF_OID == SIZEOF_INT
	case TYPE_oid:
#endif
	case TYPE_flt:
	case TYPE_int:
			return CLS_map_int(rb, cmap, b);
#if SIZEOF_WRD == SIZEOF_LNG
	case TYPE_wrd:
#endif
#if SIZEOF_OID == SIZEOF_LNG
	case TYPE_oid:
#endif
	case TYPE_dbl:
	case TYPE_lng:
			return CLS_map_lng(rb, cmap, b);
#ifdef HAVE_HGE
	case TYPE_hge:
			return CLS_map_hge(rb, cmap, b);
#endif
	default:
		break;
	}
	bi = bat_iterator(b);
	bf = BUNfirst(b);
	m = (wrd*)Tloc(cmap, BUNfirst(cmap));
	if (b->T->varsized) {
		for (i = 0; i < BATcount(b); i++) {
			BUNinplace(rb, (BUN)m[i], NULL, BUNtvar(bi, bf+i), 0);
		}
	} else {
		for (i = 0; i < BATcount(b); i++) {
			BUNinplace(rb, (BUN)m[i], NULL, BUNtloc(bi, bf+i), 0);
		}
	}
	BBPunfix(*CMAP);
	BBPunfix(*B);
	BATsetaccess(rb, BAT_READ);
	BBPkeepref(*RB = rb->batCacheid);
	return MAL_SUCCEED;
}

str
CLS_map2(bat *RB, bat *PSUM, bat *CMAP, bat *B)
{
	BATiter bi;
	BAT *rb, *psum = NULL, *cmap = NULL, *b = NULL;
	BUN i = 0, bf;
	bte *m;
	wrd *psumcp;

	if ((psum = BATdescriptor(*PSUM)) == NULL ||
	    (cmap = BATdescriptor(*CMAP)) == NULL ||
	    (b = BATdescriptor(*B)) == NULL) {
		if (psum)
			BBPunfix(*PSUM);
		if (cmap)
			BBPunfix(*CMAP);
		throw(MAL, "cluster.map", INTERNAL_BAT_ACCESS);
	}
	if (cmap->tsorted) {
		/* input to cluster was sorted, ie nothing to do here
		   than to return the input */
		BBPunfix(*PSUM);
		BBPunfix(*CMAP);
		BBPkeepref(*RB = b->batCacheid);
		return MAL_SUCCEED;
	}
	/* work around non aligned bats */
	if (BATcount(cmap) &&
	    cmap->H->seq != b->H->seq && b->H->type != TYPE_void) {
		BAT *ob = b;
		BAT *v = VIEWcombine(cmap);

		b = BATleftjoin(v, b, BATcount(b));
		BBPunfix(ob->batCacheid);
	}
	if (BATcount(cmap) != BATcount(b) ||
	   (BATcount(cmap) && cmap->H->seq != b->H->seq)) {
		BBPunfix(*PSUM);
		BBPunfix(*CMAP);
		BBPunfix(b->batCacheid);
		throw(MAL, "cluster.map", OPERATION_FAILED " Counts of operands do not match");
	}

	psumcp = (wrd*)GDKmalloc(BATcount(psum) * sizeof(wrd));
	if ( psumcp == NULL || (rb = BATnew(TYPE_void, ATOMtype(b->ttype), BATcount(b), TRANSIENT)) == NULL) {
		if (psumcp != NULL) {
			GDKfree(psumcp);
		}
		BBPunfix(*PSUM);
		BBPunfix(*CMAP);
		BBPunfix(b->batCacheid);
		throw(MAL, "cluster.map", MAL_MALLOC_FAIL);
	}
	BATsetcount(rb, BATcount(b));
	BATseqbase(rb, b->H->seq);
	rb->tsorted= FALSE;
	rb->trevsorted= FALSE;
	rb->tdense= FALSE;
	rb->H->nonil = b->H->nonil;
	rb->T->nonil = b->T->nonil;
	*RB = rb->batCacheid;

	memcpy(psumcp, Tloc(psum,BUNfirst(psum)), BATcount(psum) * sizeof(wrd));
	BBPunfix(*PSUM);

	switch(ATOMstorage(b->ttype)) {
	case TYPE_bte:
			return CLS_map2_bte(rb, psumcp, cmap, b);
	case TYPE_sht:
			return CLS_map2_sht(rb, psumcp, cmap, b);
#if SIZEOF_WRD == SIZEOF_INT
	case TYPE_wrd:
#endif
#if SIZEOF_OID == SIZEOF_INT
	case TYPE_oid:
#endif
	case TYPE_flt:
	case TYPE_int:
			return CLS_map2_int(rb, psumcp, cmap, b);
#if SIZEOF_WRD == SIZEOF_LNG
	case TYPE_wrd:
#endif
#if SIZEOF_OID == SIZEOF_LNG
	case TYPE_oid:
#endif
	case TYPE_dbl:
	case TYPE_lng:
			return CLS_map2_lng(rb, psumcp, cmap, b);
#ifdef HAVE_HGE
	case TYPE_hge:
			return CLS_map2_hge(rb, psumcp, cmap, b);
#endif
	default:
		break;
	}
	bi = bat_iterator(b);
	bf = BUNfirst(b);
	m = (bte*)Tloc(cmap, BUNfirst(cmap));
	if (b->T->varsized) {
		for (i = 0; i < BATcount(b); i++) {
			BUNinplace(rb, (BUN)(psumcp[m[i]]), NULL, BUNtvar(bi, bf+i), 0);
			psumcp[m[i]]++;
		}
	} else {
		for (i = 0; i < BATcount(b); i++) {
			BUNinplace(rb, (BUN)(psumcp[m[i]]), NULL, BUNtloc(bi, bf+i), 0);
			psumcp[m[i]]++;
		}
	}
	GDKfree(psumcp);
	BBPunfix(*CMAP);
	BBPunfix(b->batCacheid);
	BATsetaccess(rb, BAT_READ);
	BBPkeepref(*RB = rb->batCacheid);
	return MAL_SUCCEED;
}
str
CLS_split( Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	int i;
	const bat *bid = getArgReference_bat(stk, pci, pci->retc);
	const bat *psum = getArgReference_bat(stk, pci, pci->retc+1);
	BAT *b, *pb;
	wrd *cnt, *end;
	BUN l = 0, h = l;

	(void)cntxt;
	(void)mb;

	b = BATdescriptor(*bid);
	if ( b == NULL)
		throw(MAL,"cluster.split", RUNTIME_OBJECT_MISSING);

	pb = BATdescriptor(*psum);
	if ( pb == NULL){
		BBPunfix(b->batCacheid);
		throw(MAL,"cluster.split", RUNTIME_OBJECT_MISSING);
	}
	cnt = (wrd*)Tloc(pb, BUNfirst(pb));
	end = cnt + BATcount(pb);

	for( i = 0; i<pci->retc && cnt < end; i++, cnt++) {
		bat *res = getArgReference_bat(stk, pci, i);
		BAT *v;

		assert((lng) *cnt <= (lng) BUN_MAX);
		assert(*cnt >= 0);
		l = (BUN) *cnt;
		if (cnt+1 < end) {
			assert(*(cnt+1) >= 0);
			assert((lng) *(cnt+1) <= (lng) BUN_MAX);
			h = (BUN) *(cnt+1);
		} else
			h = BATcount(b)+1;
		v = BATslice(b, l, h);
		BBPkeepref(*res = v->batCacheid);
	}
	BBPunfix(*bid);
	BBPunfix(*psum);
	return MAL_SUCCEED;
}
