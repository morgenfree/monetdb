/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/*
 * (c) M. L. Kersten, P. Boncz, S. Manegold, N. Nes, K.S. Mullender
 * Common BAT Operations
 * We factor out all possible overhead by inlining code.  This
 * includes the macros BUNhead and BUNtail, which do a test to see
 * whether the atom resides in the buns or in a variable storage
 * heap. The updateloop(dstbat, srcbat, operation) macro invokes
 * operation(dstbat, BUNhead(srcbat), BUNtail(srcbat)) on all buns of
 * the srcbat, but testing only once where they reside.
 */
#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_private.h"

#define updateloop(bn, b, func)						\
	do {								\
		BATiter bi = bat_iterator(b);				\
		BUN p1, p2;						\
									\
		BATloop(b, p1, p2) {					\
			func(bn, BUNhead(bi, p1), BUNtail(bi, p1));	\
		}							\
	} while (0)

gdk_return
unshare_string_heap(BAT *b)
{
	if (b->ttype == TYPE_str &&
	    b->T->vheap->parentid != abs(b->batCacheid)) {
		Heap *h = GDKzalloc(sizeof(Heap));
		if (h == NULL)
			return GDK_FAIL;
		h->parentid = abs(b->batCacheid);
		h->farmid = BBPselectfarm(b->batRole, TYPE_str, varheap);
		if (b->T->vheap->filename) {
			char *nme = BBP_physical(b->batCacheid);
			h->filename = GDKfilepath(NOFARM, NULL, nme, "theap");
			if (h->filename == NULL) {
				GDKfree(h);
				return GDK_FAIL;
			}
		}
		if (HEAPcopy(h, b->T->vheap) != GDK_SUCCEED) {
			HEAPfree(h, 1);
			GDKfree(h);
			return GDK_FAIL;
		}
		BBPunshare(b->T->vheap->parentid);
		b->T->vheap = h;
	}
	return GDK_SUCCEED;
}

/* We try to be clever when appending one string bat to another.
 * First of all, we try to actually share the string heap so that we
 * don't need an extra copy, and if that can't be done, we see whether
 * it makes sense to just quickly copy the whole string heap instead
 * of inserting individual strings.  See the comments in the code for
 * more information. */
static gdk_return
insert_string_bat(BAT *b, BAT *n, int append, int force)
{
	BATiter ni;		/* iterator */
	int tt;			/* tail type */
	size_t toff = ~(size_t) 0;	/* tail offset */
	BUN p, q;		/* loop variables */
	oid o = 0;		/* in case we're appending */
	const void *hp, *tp;	/* head and tail value pointers */
	unsigned char tbv;	/* tail value-as-bte */
	unsigned short tsv;	/* tail value-as-sht */
#if SIZEOF_VAR_T == 8
	unsigned int tiv;	/* tail value-as-int */
#endif
	var_t v;		/* value */
	size_t off;		/* offset within n's string heap */

	assert(b->htype == TYPE_void || b->htype == TYPE_oid);
	if (n->batCount == 0)
		return GDK_SUCCEED;
	ni = bat_iterator(n);
	hp = NULL;
	tp = NULL;
	if (append && b->htype != TYPE_void) {
		hp = &o;
		o = MAXoid(b);
	}
	tt = b->ttype;
	if (tt == TYPE_str &&
	    (!GDK_ELIMDOUBLES(b->T->vheap) || b->batCount == 0) &&
	    !GDK_ELIMDOUBLES(n->T->vheap) &&
	    b->T->vheap->hashash == n->T->vheap->hashash &&
	    /* if needs to be kept unique, take slow path */
	    (b->tkey & BOUND2BTRUE) == 0) {
		if (b->S->role == TRANSIENT) {
			/* If b is in the transient farm (i.e. b will
			 * never become persistent), we try some
			 * clever tricks to avoid copying:
			 * - if b is empty, we just let it share the
                         *   string heap with n;
			 * - otherwise, if b's string heap and n's
                         *   string heap are the same (i.e. shared),
                         *   we leave it that way;
			 * - otherwise, if b shares its string heap
                         *   with some other bat, we materialize it
                         *   and we will have to copy strings.
			 */
			bat bid = abs(b->batCacheid);

			if (b->batCount == 0) {
				if (b->T->vheap->parentid != bid) {
					BBPunshare(b->T->vheap->parentid);
				} else {
					HEAPfree(b->T->vheap, 1);
					GDKfree(b->T->vheap);
				}
				BBPshare(n->T->vheap->parentid);
				b->T->vheap = n->T->vheap;
				toff = 0;
			} else if (b->T->vheap->parentid == n->T->vheap->parentid) {
				toff = 0;
			} else if (b->T->vheap->parentid != bid &&
				   unshare_string_heap(b) != GDK_SUCCEED) {
				return GDK_FAIL;
			}
		}
		if (toff == ~(size_t) 0 && n->batCount > 1024) {
			/* If b and n aren't sharing their string
			 * heaps, we try to determine whether to copy
			 * n's whole string heap to the end of b's, or
			 * whether we will insert each string from n
			 * individually.  We do this by testing a
			 * sample of n's strings and extrapolating
			 * from that sample whether n uses a
			 * significant part of its string heap for its
			 * strings (i.e. whether there are many unused
			 * strings in n's string heap).  If n doesn't
			 * have many strings in the first place, we
			 * skip this and just insert them all
			 * individually.  We also check whether a
			 * significant number of n's strings happen to
			 * have the same offset in b.  In the latter
			 * case we also want to insert strings
			 * individually, but reusing the string in b's
			 * string heap. */
			int match = 0, i;
			size_t len = b->T->vheap->hashash ? 1024 * EXTRALEN : 0;
			for (i = 0; i < 1024; i++) {
				p = BUNfirst(n) + (BUN) (((double) rand() / RAND_MAX) * (BATcount(n) - 1));
				off = BUNtvaroff(ni, p);
				if (off < b->T->vheap->free &&
				    strcmp(b->T->vheap->base + off, n->T->vheap->base + off) == 0 &&
				    (!b->T->vheap->hashash ||
				     ((BUN *) (b->T->vheap->base + off))[-1] == (n->T->vheap->hashash ? ((BUN *) (n->T->vheap->base + off))[-1] : strHash(n->T->vheap->base + off))))
					match++;
				len += (strlen(n->T->vheap->base + off) + 8) & ~7;
			}
			if (match < 768 && (size_t) (BATcount(n) * (double) len / 1024) >= n->T->vheap->free / 2) {
				/* append string heaps */
				toff = b->batCount == 0 ? 0 : b->T->vheap->free;
				/* make sure we get alignment right */
				toff = (toff + GDK_VARALIGN - 1) & ~(GDK_VARALIGN - 1);
				assert(((toff >> GDK_VARSHIFT) << GDK_VARSHIFT) == toff);
				/* if in "force" mode, the heap may be shared when
				 * memory mapped */
				if (HEAPextend(b->T->vheap, toff + n->T->vheap->size, force) != GDK_SUCCEED) {
					toff = ~(size_t) 0;
					goto bunins_failed;
				}
				memcpy(b->T->vheap->base + toff, n->T->vheap->base, n->T->vheap->free);
				b->T->vheap->free = toff + n->T->vheap->free;
				/* flush double-elimination hash table */
				memset(b->T->vheap->base, 0, GDK_STRHASHSIZE);
			}
		}
		if (toff != ~(size_t) 0) {
			/* we only have to copy the offsets from n to
			 * b, possibly with an offset (if toff != 0),
			 * so set up some variables and set up b's
			 * tail so that it looks like it's a fixed
			 * size column.  Of course, we must make sure
			 * first that the width of b's offset heap can
			 * accommodate all values. */
			if (b->T->width < SIZEOF_VAR_T &&
			    ((size_t) 1 << 8 * b->T->width) <= (b->T->width <= 2 ? (b->T->vheap->size >> GDK_VARSHIFT) - GDK_VAROFFSET : (b->T->vheap->size >> GDK_VARSHIFT))) {
				/* offsets aren't going to fit, so
				 * widen offset heap */
				if (GDKupgradevarheap(b->T, (var_t) (b->T->vheap->size >> GDK_VARSHIFT), 0, force) != GDK_SUCCEED) {
					toff = ~(size_t) 0;
					goto bunins_failed;
				}
			}
			switch (b->T->width) {
			case 1:
				tt = TYPE_bte;
				tp = &tbv;
				break;
			case 2:
				tt = TYPE_sht;
				tp = &tsv;
				break;
#if SIZEOF_VAR_T == 8
			case 4:
				tt = TYPE_int;
				tp = &tiv;
				break;
#endif
			default:
				tt = TYPE_var;
				tp = &v;
				break;
			}
			b->tvarsized = 0;
			b->ttype = tt;
		}
	}
	if (!append) {
		if (b->htype == TYPE_void)
			hp = NULL;
		else if (n->htype == TYPE_void) {
			assert(b->htype == TYPE_oid);
			o = n->hseqbase;
			hp = &o;
			append = 1;
		}
	}
	if (toff == 0 && n->T->width == b->T->width && (b->htype == TYPE_void || !append)) {
		/* we don't need to do any translation of offset
		 * values, nor do we need to do any calculations for
		 * the head column, so we can use fast memcpy */
		memcpy(Tloc(b, BUNlast(b)), Tloc(n, BUNfirst(n)),
		       BATcount(n) * n->T->width);
		if (b->htype != TYPE_void) {
			assert(n->htype == b->htype);
			assert(!append);
			memcpy(Hloc(b, BUNlast(b)), Hloc(n, BUNfirst(n)),
			       BATcount(n) * Hsize(n));
		}
		BATsetcount(b, BATcount(b) + BATcount(n));
	} else if (toff != ~(size_t) 0) {
		/* we don't need to insert any actual strings since we
		 * have already made sure that they are all in b's
		 * string heap at known locations (namely the offset
		 * in n added to toff), so insert offsets from n after
		 * adding toff into b */
		/* note the use of the "restrict" qualifier here: all
		 * four pointers below point to the same value, but
		 * only one of them will actually be used, hence we
		 * still obey the rule for restrict-qualified
		 * pointers */
		const unsigned char *restrict tbp = (const unsigned char *) Tloc(n, BUNfirst(n));
		const unsigned short *restrict tsp = (const unsigned short *) Tloc(n, BUNfirst(n));
#if SIZEOF_VAR_T == 8
		const unsigned int *restrict tip = (const unsigned int *) Tloc(n, BUNfirst(n));
#endif
		const var_t *restrict tvp = (const var_t *) Tloc(n, BUNfirst(n));

		BATloop(n, p, q) {
			if (!append && b->htype)
				hp = BUNhloc(ni, p);

			switch (n->T->width) {
			case 1:
				v = (var_t) *tbp++ + GDK_VAROFFSET;
				break;
			case 2:
				v = (var_t) *tsp++ + GDK_VAROFFSET;
				break;
#if SIZEOF_VAR_T == 8
			case 4:
				v = (var_t) *tip++;
				break;
#endif
			default:
				v = *tvp++;
				break;
			}
			v = (var_t) ((((size_t) v << GDK_VARSHIFT) + toff) >> GDK_VARSHIFT);
			assert(v >= GDK_VAROFFSET);
			assert(((size_t) v << GDK_VARSHIFT) < b->T->vheap->free);
			switch (b->T->width) {
			case 1:
				assert(v - GDK_VAROFFSET < ((var_t) 1 << 8));
				tbv = (unsigned char) (v - GDK_VAROFFSET);
				break;
			case 2:
				assert(v - GDK_VAROFFSET < ((var_t) 1 << 16));
				tsv = (unsigned short) (v - GDK_VAROFFSET);
				break;
#if SIZEOF_VAR_T == 8
			case 4:
				assert(v < ((var_t) 1 << 32));
				tiv = (unsigned int) v;
				break;
#endif
			default:
				break;
			}
			bunfastins(b, hp, tp);
			o++;
		}
	} else {
		/* Insert values from n individually into b; however,
		 * we check whether there is a string in b's string
		 * heap at the same offset as the string is in n's
		 * string heap (in case b's string heap is a copy of
		 * n's).  If this is the case, we just copy the
		 * offset, otherwise we insert normally.  */
		BATloop(n, p, q) {
			if (!append && b->htype)
				hp = BUNhloc(ni, p);

			off = BUNtvaroff(ni, p); /* the offset */
			tp = n->T->vheap->base + off; /* the string */
			if (off < b->T->vheap->free &&
			    strcmp(b->T->vheap->base + off, tp) == 0 &&
			    (!b->T->vheap->hashash ||
			     ((BUN *) (b->T->vheap->base + off))[-1] == (n->T->vheap->hashash ? ((BUN *) tp)[-1] : strHash(tp)))) {
				/* we found the string at the same
				 * offset in b's string heap as it was
				 * in n's string heap, so we don't
				 * have to insert a new string into b:
				 * we can just copy the offset */
				if (b->H->type)
					*(oid *) Hloc(b, BUNlast(b)) = *(oid *) hp;
				v = (var_t) (off >> GDK_VARSHIFT);
				if (b->T->width < SIZEOF_VAR_T &&
				    ((size_t) 1 << 8 * b->T->width) <= (b->T->width <= 2 ? v - GDK_VAROFFSET : v)) {
					/* offset isn't going to fit,
					 * so widen offset heap */
					if (GDKupgradevarheap(b->T, v, 0, force) != GDK_SUCCEED) {
						goto bunins_failed;
					}
				}
				switch (b->T->width) {
				case 1:
					assert(v - GDK_VAROFFSET < ((var_t) 1 << 8));
					*(unsigned char *)Tloc(b, BUNlast(b)) = (unsigned char) (v - GDK_VAROFFSET);
					b->T->heap.free += 1;
					break;
				case 2:
					assert(v - GDK_VAROFFSET < ((var_t) 1 << 16));
					*(unsigned short *)Tloc(b, BUNlast(b)) = (unsigned short) (v - GDK_VAROFFSET);
					b->T->heap.free += 2;
					break;
#if SIZEOF_VAR_T == 8
				case 4:
					assert(v < ((var_t) 1 << 32));
					*(unsigned int *)Tloc(b, BUNlast(b)) = (unsigned int) v;
					b->T->heap.free += 4;
					break;
#endif
				default:
					*(var_t *)Tloc(b, BUNlast(b)) = v;
					b->T->heap.free += SIZEOF_VAR_T;
					break;
				}
				b->batCount++;
			} else {
				bunfastins(b, hp, tp);
			}
			o++;
		}
	}
	if (toff != ~(size_t) 0) {
		b->tvarsized = 1;
		b->ttype = TYPE_str;
	}
	return GDK_SUCCEED;
      bunins_failed:
	if (toff != ~(size_t) 0) {
		b->tvarsized = 1;
		b->ttype = TYPE_str;
	}
	return GDK_FAIL;
}

/*
 * BAT insert/delete/replace
 * The content of a BAT can be appended to (removed from) another
 * using BATins (BATdel).
 */
#define bunins(b,h,t) if (BUNins(b,h,t,force) != GDK_SUCCEED) return GDK_FAIL;
gdk_return
BATins(BAT *b, BAT *n, bit force)
{
	BAT *tmp = NULL;
	gdk_return res = GDK_FAIL;
	int fastpath = 0;
	int countonly;

	if (b == NULL || n == NULL || BATcount(n) == 0) {
		return GDK_SUCCEED;
	}
	if (b->htype != TYPE_void && b->htype != TYPE_oid) {
		GDKerror("BATins: input must be (V)OID headed\n");
		return GDK_FAIL;
	}
	ALIGNins(b, "BATins", force, GDK_FAIL);
	BATcompatible(b, n, GDK_FAIL, "BATins");

	countonly = (b->htype == TYPE_void && b->ttype == TYPE_void);

	if (BUNlast(b) + BATcount(n) > BUN_MAX) {
		GDKerror("BATins: combined BATs too large\n");
		return GDK_FAIL;
	}

	if (b->htype != TYPE_void &&
	    (b->ttype == TYPE_void ||
	     (!b->H->hash && b->T->hash &&
	      ATOMstorage(b->ttype) == TYPE_int))) {	/* OIDDEPEND */
		return BATins(BATmirror(b), BATmirror(n), force);
	}

	if (BUNlast(b) + BATcount(n) > BATcapacity(b)) {
		/* if needed space exceeds a normal growth extend just
		 * with what's needed */
		BUN ncap = BUNlast(b) + BATcount(n);
		BUN grows = BATgrows(b);

		if (ncap > grows)
			grows = ncap;
		if (BATextend(b, grows) != GDK_SUCCEED)
			goto bunins_failed;
	}

	if (b->htype == TYPE_void && b->hseqbase != oid_nil) {
		BATiter ni = bat_iterator(n);
		BUN t = (BUN) b->hseqbase + BATcount(b) - 1;
		oid h = *(oid *) BUNhead(ni, BUNfirst(n));

		if (BATcount(b) == 0 && (BATcount(n) == 1 || BAThdense(n))) {
			b->hseqbase = h;
		} else if (t + 1 != (BUN) h || !BAThdense(n)) {
			if (BATmaterializeh(b) != GDK_SUCCEED)
				return GDK_FAIL;
			countonly = 0;
		}
	}
	if (b->ttype == TYPE_void && b->tseqbase != oid_nil) {
		BATiter ni = bat_iterator(n);
		BUN t = (BUN) b->tseqbase + BATcount(b) - 1;
		oid h = *(oid *) BUNtail(ni, BUNfirst(n));

		if (BATcount(b) == 0 && (BATcount(n) == 1 || BATtdense(n))) {
			b->tseqbase = h;
		} else if (t + 1 != (BUN) h || !BATtdense(n)) {
			if (BATmaterializet(b) != GDK_SUCCEED)
				return GDK_FAIL;
			countonly = 0;
		}
	}
	if (b->T->hash == NULL &&
	    (b->tkey & BOUND2BTRUE) == 0 &&
	    ((b->hkey & BOUND2BTRUE) == 0 || n->hkey) &&
	    (b->H->hash == NULL || ATOMstorage(b->htype) == ATOMstorage(TYPE_oid))) {
		if (b->hkey & BOUND2BTRUE && b->batCount > 0) {
			tmp = n = BATkdiff(n, b);
			if (n == NULL)
				return GDK_FAIL;
		}
		fastpath = 1;
	}

	if (b->H->hash == NULL && b->hkey & BOUND2BTRUE &&
	    (b->batCount > 0 || !n->hkey) &&
	    (BATcount(b) ||
	     (BATcount(n) && n->htype != TYPE_void && !n->hdense))) {
		BAThash(BATmirror(b), BATcount(b) + BATcount(n));
	}
	if (b->T->hash == NULL && b->tkey & BOUND2BTRUE &&
	    (b->batCount > 0 || !n->tkey) &&
	    (BATcount(b) ||
	     (BATcount(n) && n->ttype != TYPE_void && !n->tdense))) {
		BAThash(b, BATcount(b) + BATcount(n));
	}
	b->batDirty = 1;
	if (fastpath) {
		BUN p, q, r = BUNlast(b);

		if (BATcount(b) == 0) {
			ALIGNset(b, n);
		} else if (BATcount(n)) {
			BUN last = BUNlast(b) - 1;
			BATiter ni = bat_iterator(n);
			BATiter bi = bat_iterator(b);
			int xx = ATOMcmp(b->htype, BUNhead(ni, BUNfirst(n)), BUNhead(bi, last));
			if (BAThordered(b) && (!BAThordered(n) || xx < 0)) {
				b->hsorted = FALSE;
				b->H->nosorted = r;
				if (b->hdense) {
					b->hdense = FALSE;
					b->H->nodense = r;
				}
			}
			if (BAThrevordered(b) &&
			    (!BAThrevordered(n) || xx > 0)) {
				b->hrevsorted = FALSE;
				b->H->norevsorted = r;
			}
			if (b->hkey && (b->hkey & BOUND2BTRUE) == 0 &&
			    (!(BAThordered(b) || BAThrevordered(b)) ||
			     n->hkey == 0 || xx == 0)) {
				BATkey(b, FALSE);
			}
			if (b->htype != TYPE_void && b->hsorted && b->hdense &&
			    (BAThdense(n) == 0 ||
			     1 + *(oid *) BUNhloc(bi, last) != *(oid *) BUNhead(ni, BUNfirst(n)))) {
				b->hdense = FALSE;
				b->H->nodense = r;
			}

			xx = ATOMcmp(b->ttype, BUNtail(ni, BUNfirst(n)), BUNtail(bi, last));
			if (BATtordered(b) && (!BATtordered(n) || xx < 0)) {
				b->tsorted = FALSE;
				b->T->nosorted = r;
				if (b->tdense) {
					b->tdense = FALSE;
					b->T->nodense = r;
				}
			}
			if (BATtrevordered(b) &&
			    (!BATtrevordered(n) || xx > 0)) {
				b->trevsorted = FALSE;
				b->T->norevsorted = r;
			}
			if (b->tkey && (b->tkey & BOUND2BTRUE) == 0 &&
			    (!(BATtordered(b) || BATtrevordered(b)) ||
			     n->tkey == 0 || xx == 0)) {
				BATkey(BATmirror(b), FALSE);
			}
			if (b->ttype != TYPE_void && b->tsorted && b->tdense &&
			    (BATtdense(n) == 0 ||
			     1 + *(oid *) BUNtloc(bi, last) != *(oid *) BUNtail(ni, BUNfirst(n)))) {
				b->tdense = FALSE;
				b->T->nodense = r;
			}
		}
		if (countonly) {
			BATsetcount(b, b->batCount + n->batCount);
		} else if (b->ttype == TYPE_str &&
			   (b->batCount == 0 || !GDK_ELIMDOUBLES(b->T->vheap)) &&
			   !GDK_ELIMDOUBLES(n->T->vheap) &&
			   b->T->vheap->hashash == n->T->vheap->hashash &&
			   VIEWtparent(n) == 0) {
			if (insert_string_bat(b, n, 0, force) != GDK_SUCCEED)
				return GDK_FAIL;
		} else if (b->htype == TYPE_void) {
			if (!ATOMvarsized(b->ttype) &&
			    BATatoms[b->ttype].atomFix == NULL &&
			    n->ttype != TYPE_void) {
				/* use fast memcpy if we can */
				memcpy(Tloc(b, BUNlast(b)),
				       Tloc(n, BUNfirst(n)),
				       BATcount(n) * Tsize(n));
				BATsetcount(b, BATcount(b) + BATcount(n));
			} else {
				BATiter ni = bat_iterator(n);

				BATloop(n, p, q) {
					bunfastapp_nocheck(b, r, BUNtail(ni, p), Tsize(b));
					r++;
				}
			}
		} else if (b->H->hash) {
			BUN i = BUNlast(b);
			BATiter ni = bat_iterator(n);

			BATloop(n, p, q) {
				const void *v = BUNhead(ni, p);

				bunfastins_nocheck(b, r, v, BUNtail(ni, p), Hsize(b), Tsize(b));
				HASHins_oid(b->H->hash, i, v);
				r++;
				i++;
			}
		} else if (!ATOMvarsized(b->htype) &&
			   BATatoms[b->htype].atomFix == NULL &&
			   !ATOMvarsized(b->ttype) &&
			   BATatoms[b->ttype].atomFix == NULL &&
			   n->htype != TYPE_void &&
			   n->ttype != TYPE_void) {
			/* use fast memcpy if we can */
			memcpy(Hloc(b, BUNlast(b)),
			       Hloc(n, BUNfirst(n)),
			       BATcount(n) * Hsize(n));
			memcpy(Tloc(b, BUNlast(b)),
			       Tloc(n, BUNfirst(n)),
			       BATcount(n) * Tsize(n));
			BATsetcount(b, BATcount(b) + BATcount(n));
		} else {
			BATiter ni = bat_iterator(n);

			BATloop(n, p, q) {
				bunfastins_nocheck(b, r, BUNhead(ni, p), BUNtail(ni, p), Hsize(b), Tsize(b));
				r++;
			}
		}
		b->H->nonil &= n->H->nonil;
		b->T->nonil &= n->T->nonil;
	} else {
		updateloop(b, n, bunins);
	}
	res = GDK_SUCCEED;
      bunins_failed:
	BBPreclaim(tmp);
	return res;
}

gdk_return
BATappend(BAT *b, BAT *n, bit force)
{
	BUN sz;
	int fastpath = 1;

	if (b == NULL || n == NULL || (sz = BATcount(n)) == 0) {
		return GDK_SUCCEED;
	}
	/* almost: assert(!isVIEW(b)); */
	assert(!(b->H->heap.parentid ||
		 b->T->heap.parentid ||
		 (b->H->vheap && b->H->vheap->parentid != abs(b->batCacheid)) ||
		 (b->T->vheap && b->T->vheap->parentid != abs(b->batCacheid) && b->T->type != TYPE_str)));

	if (b->htype != TYPE_void && b->htype != TYPE_oid) {
		GDKerror("BATappend: input must be (V)OID headed\n");
		return GDK_FAIL;
	}
	ALIGNapp(b, "BATappend", force, GDK_FAIL);
	BATcompatible(b, n, GDK_FAIL, "BATappend");

	if (BUNlast(b) + BATcount(n) > BUN_MAX) {
		GDKerror("BATappend: combined BATs too large\n");
		return GDK_FAIL;
	}

	b->batDirty = 1;

	if (sz > BATcapacity(b) - BUNlast(b)) {
		/* if needed space exceeds a normal growth extend just
		 * with what's needed */
		BUN ncap = BUNlast(b) + sz;
		BUN grows = BATgrows(b);

		if (ncap > grows)
			grows = ncap;
		if (BATextend(b, grows) != GDK_SUCCEED)
			goto bunins_failed;
	}

	/* append two void,void bats */
	if (b->ttype == TYPE_void && BATtdense(b)) {
		oid f = n->tseqbase;

		if (n->ttype != TYPE_void)
			f = *(oid *) BUNtloc(bat_iterator(n), BUNfirst(n));

		if (BATcount(b) == 0 && f != oid_nil)
			BATseqbase(BATmirror(b), f);
		if (BATtdense(n) && BATcount(b) + b->tseqbase == f) {
			if (b->htype != TYPE_void) {
				BUN r = BUNlast(b);
				oid m;

				f = MAXoid(b);
				f++;
				if (f + sz >= GDK_oid_max) {
					GDKerror("BATappend: overflow of head value\n");
					return GDK_FAIL;
				}
				m = (oid) (f + sz);
				b = BATmirror(b); /* so we can use bunfastapp */
				for (; f < m; f++, r++) {
					bunfastapp_nocheck(b, r, (ptr) &f, Tsize(b));
				}
				b = BATmirror(b);
			} else {
				sz += BATcount(b);
				BATsetcount(b, sz);
			}
			return GDK_SUCCEED;
		}
		/* we need to materialize the tail */
		if (BATmaterializet(b) != GDK_SUCCEED)
			return GDK_FAIL;
	}

	IMPSdestroy(b);		/* imprints do not support updates yet */
	/* a hash is useless for void bats */
	if (b->H->hash)
		HASHremove(BATmirror(b));

	if (b->T->hash && (2 * b->T->hash->mask) < (BATcount(b) + sz)) {
		HASHremove(b);
	}
	if (b->T->hash != NULL ||
	    (b->tkey & BOUND2BTRUE) != 0 ||
	    (b->H->hash != NULL && ATOMstorage(b->htype) != ATOMstorage(TYPE_oid)))
		fastpath = 0;

	if (fastpath) {
		BUN p, q, r = BUNlast(b);

		if (BATcount(b) == 0) {
			BATiter ni = bat_iterator(n);

			ALIGNsetH(BATmirror(b), BATmirror(n));
			b->tseqbase = n->tseqbase;

			if (n->tdense && n->ttype == TYPE_oid) {
				b->tseqbase = *(oid *) BUNtail(ni, BUNfirst(n));
			}
			b->tdense = n->tdense;
			b->T->nodense = n->T->nodense;
			b->tkey |= (n->tkey & TRUE);
			b->T->nokey[0] = n->T->nokey[0];
			b->T->nokey[1] = n->T->nokey[1];
			b->T->nonil = n->T->nonil;
		} else {
			BUN last = BUNlast(b) - 1;
			BATiter ni = bat_iterator(n);
			BATiter bi = bat_iterator(b);
			int xx = ATOMcmp(b->ttype, BUNtail(ni, BUNfirst(n)), BUNtail(bi, last));
			if (BATtordered(b) && (!BATtordered(n) || xx < 0)) {
				b->tsorted = FALSE;
				b->T->nosorted = r;
				if (b->tdense) {
					b->tdense = FALSE;
					b->T->nodense = r;
				}
			}
			if (BATtrevordered(b) &&
			    (!BATtrevordered(n) || xx > 0)) {
				b->trevsorted = FALSE;
				b->T->norevsorted = r;
			}
			if (b->tkey &&
			    (!(BATtordered(b) || BATtrevordered(b)) ||
			     n->tkey == 0 || xx == 0)) {
				BATkey(BATmirror(b), FALSE);
			}
			if (b->ttype != TYPE_void && b->tsorted && b->tdense &&
			    (BATtdense(n) == 0 ||
			     1 + *(oid *) BUNtloc(bi, last) != *(oid *) BUNtail(ni, BUNfirst(n)))) {
				b->tdense = FALSE;
				b->T->nodense = r;
			}
		}
		if (b->ttype == TYPE_str &&
		    (b->batCount == 0 || !GDK_ELIMDOUBLES(b->T->vheap)) &&
		    !GDK_ELIMDOUBLES(n->T->vheap) &&
		    b->T->vheap->hashash == n->T->vheap->hashash) {
			if (insert_string_bat(b, n, 1, force) != GDK_SUCCEED)
				return GDK_FAIL;
		} else if (b->htype == TYPE_void) {
			if (!ATOMvarsized(b->ttype) &&
			    BATatoms[b->ttype].atomFix == NULL &&
			    b->ttype != TYPE_void && n->ttype != TYPE_void) {
				/* use fast memcpy if we can */
				memcpy(Tloc(b, BUNlast(b)),
				       Tloc(n, BUNfirst(n)),
				       BATcount(n) * Tsize(n));
				BATsetcount(b, BATcount(b) + BATcount(n));
			} else {
				BATiter ni = bat_iterator(n);

				BATloop(n, p, q) {
					bunfastapp_nocheck(b, r, BUNtail(ni, p), Tsize(b));
					r++;
				}
			}
			if (b->hseqbase != oid_nil)
				b->hrevsorted = 0;
		} else {
			oid o = MAXoid(b);
			BATiter ni = bat_iterator(n);

			o++;
			BATloop(n, p, q) {
				bunfastins_nocheck(b, r, &o, BUNtail(ni, p), Hsize(b), Tsize(b));
				o++;
				r++;
			}
			b->hrevsorted = 0;
		}
	} else {
		BUN p, q;
		BUN i = BUNlast(b);
		BATiter ni = bat_iterator(n);

		if (b->tkey & BOUND2BTRUE) {
			b->tdense = b->tsorted = b->trevsorted = 0;
			if (b->hseqbase != oid_nil) {
				oid h = b->hseqbase + i;

				BATloop(n, p, q) {
					const void *t = BUNtail(ni, p);

					if (BUNfnd(b, t) == BUN_NONE) {
						bunfastins(b, &h, t);
						if (b->T->hash) {
							HASHins(b, i, t);
						}
						h++;
						i++;
					}
				}
				b->hrevsorted = 0;
			} else {
				oid on = oid_nil;

				BATloop(n, p, q) {
					const void *t = BUNtail(ni, p);

					if (BUNfnd(b, t) == BUN_NONE) {
						bunfastins(b, &on, t);
						if (b->T->hash) {
							HASHins(b, i, t);
						}
						i++;
					}
				}
				BATkey(b, FALSE);
				b->hdense = b->hsorted = b->hrevsorted = 0;
			}
		} else if (b->hseqbase != oid_nil) {
			oid h;

			if (b->hseqbase + BATcount(b) + BATcount(n) >= GDK_oid_max) {
				GDKerror("BATappend: overflow of head value\n");
				return GDK_FAIL;
			}
			h = (oid) (b->hseqbase + BATcount(b));

			BATloop(n, p, q) {
				const void *t = BUNtail(ni, p);

				bunfastins(b, &h, t);
				if (b->T->hash) {
					HASHins(b, i, t);
				}
				i++;
				h++;
			}
			BATkey(BATmirror(b), FALSE);
			b->tdense = b->tsorted = b->trevsorted = 0;
			b->hrevsorted = 0;
		} else {
			oid on = oid_nil;

			BATloop(n, p, q) {
				const void *t = BUNtail(ni, p);

				bunfastins(b, &on, t);
				if (b->T->hash) {
					HASHins(b, i, t);
				}
				i++;
			}
			BATkey(b, FALSE);
			BATkey(BATmirror(b), FALSE);
			b->hdense = b->hsorted = b->hrevsorted = 0;
			b->tdense = b->tsorted = b->trevsorted = 0;
		}
	}
	b->H->nonil &= n->H->nonil;
	b->T->nonil &= n->T->nonil;
	return GDK_SUCCEED;
      bunins_failed:
	return GDK_FAIL;
}


#define TYPEcheck(t1,t2,func)						\
	do {								\
		if (TYPEerror(t1, t2)) {				\
			GDKerror("%s: Incompatible types %s and %s.\n", \
				 func, ATOMname(t2), ATOMname(t1));	\
			return GDK_FAIL;				\
		}							\
	} while (0)

#define bundel(b,h,t) do { if (BUNdel(b,h,t,force) != GDK_SUCCEED) { GDKerror("BATdel: BUN does not occur.\n"); return GDK_FAIL; } } while (0)
gdk_return
BATdel(BAT *b, BAT *n, bit force)
{
	ERRORcheck(b == NULL, "set:BAT required\n", GDK_FAIL);
	ERRORcheck(n == NULL, "set:BAT required\n", GDK_FAIL);
	if (BATcount(n) == 0) {
		return GDK_SUCCEED;
	}
	ALIGNdel(b, "BATdel", force, GDK_FAIL);
	TYPEcheck(b->htype, n->htype, "BATdel");
	TYPEcheck(b->ttype, n->ttype, "BATdel");
	updateloop(b, n, bundel);
	return GDK_SUCCEED;
}

/*
 * The last in this series is a BATreplace, which replaces all the
 * buns mentioned.
 */
#define BUNreplace_force(a,b,c) BUNreplace(a,b,c,force)
gdk_return
BATreplace(BAT *b, BAT *p, BAT *n, bit force)
{
	if (b == NULL || p == NULL || n == NULL || BATcount(n) == 0) {
		return GDK_SUCCEED;
	}
	void_replace_bat(b, p, n, force);
	return GDK_SUCCEED;
}


/*
 *  BAT Selections
 * The BAT selectors are among the most heavily used operators.
 * Their efficient implementation is therefore mandatory.
 *
 * The interface supports seven operations: BATslice, BATselect,
 * BATfragment, BATproject, BATrestrict.
 *
 * BAT slice
 * This function returns a horizontal slice from a BAT. It optimizes
 * execution by avoiding to copy when the BAT is memory mapped (in
 * this case, an independent submap is created) or else when it is
 * read-only, then a VIEW bat is created as a result.
 *
 * If a new copy has to be created, this function takes care to
 * preserve void-columns (in this case, the seqbase has to be
 * recomputed in the result).
 *
 * Note that the BATslice() is used indirectly as well as a special
 * case for BATselect (range selection on sorted column) and
 * BATsemijoin (when two dense columns are semijoined).
 *
 * NOTE new semantics, the selected range is excluding the high value.
 */
#undef BATslice
BAT *
BATslice(BAT *b, BUN l, BUN h)
{
	BUN low = l;
	BAT *bn;
	BATiter bni, bi = bat_iterator(b);
	oid foid;		/* first oid value if oid column */

	BATcheck(b, "BATslice", NULL);
	if (h > BATcount(b))
		h = BATcount(b);
	if (h < l)
		h = l;
	l += BUNfirst(b);
	h += BUNfirst(b);

	if (l > BUN_MAX || h > BUN_MAX) {
		GDKerror("BATslice: boundary out of range\n");
		return NULL;
	}

	/*
	 * If the source BAT is readonly, then we can obtain a VIEW
	 * that just reuses the memory of the source.
	 */
	if (BAThrestricted(b) == BAT_READ && BATtrestricted(b) == BAT_READ) {
		BUN cnt = h - l;

		bn = VIEWcreate_(b, b, TRUE);
		bn->batFirst = bn->batDeleted = bn->batInserted = 0;
		bn->H->heap.base = (bn->htype) ? BUNhloc(bi, l) : NULL;
		bn->T->heap.base = (bn->ttype) ? BUNtloc(bi, l) : NULL;
		bn->H->heap.size = headsize(bn, cnt);
		bn->T->heap.size = tailsize(bn, cnt);
		BATsetcount(bn, cnt);
		BATsetcapacity(bn, cnt);
	/*
	 * We have to do it: create a new BAT and put everything into it.
	 */
	} else {
		BUN p = l;
		BUN q = h;

		bn = BATnew((BAThdense(b)?TYPE_void:b->htype), b->ttype, h - l, TRANSIENT);
		if (bn == NULL) {
			return bn;
		}
		if ((bn->htype == TYPE_void || !bn->hvarsized) &&
		    BATatoms[bn->htype].atomPut == NULL &&
		    BATatoms[bn->htype].atomFix == NULL &&
		    (bn->ttype == TYPE_void || !bn->tvarsized) &&
		    BATatoms[bn->ttype].atomPut == NULL &&
		    BATatoms[bn->ttype].atomFix == NULL) {
			if (bn->htype)
				memcpy(Hloc(bn, BUNfirst(bn)), Hloc(b, p),
				       (q - p) * Hsize(bn));
			if (bn->ttype)
				memcpy(Tloc(bn, BUNfirst(bn)), Tloc(b, p),
				       (q - p) * Tsize(bn));
			BATsetcount(bn, h - l);
		} else if (BAThdense(b) && b->ttype) {
			for (; p < q; p++) {
				bunfastapp(bn, BUNtail(bi, p));
			}
		} else if (b->htype != b->ttype || b->htype != TYPE_void) {
			for (; p < q; p++) {
				bunfastins(bn, BUNhead(bi, p), BUNtail(bi, p));
			}
		} else {
			BATsetcount(bn, h - l);
		}
		bn->hsorted = b->hsorted;
		bn->hrevsorted = b->hrevsorted;
		bn->hkey = b->hkey & 1;
		bn->H->nonil = b->H->nonil;
		bn->tsorted = b->tsorted;
		bn->trevsorted = b->trevsorted;
		bn->tkey = b->tkey & 1;
		bn->T->nonil = b->T->nonil;
	}
	bni = bat_iterator(bn);
	if (BAThdense(b)) {
		bn->hdense = TRUE;
		BATseqbase(bn, (oid) (b->hseqbase + low));
	} else if (bn->hkey && bn->htype == TYPE_oid) {
		if (BATcount(bn) == 0) {
			bn->hdense = TRUE;
			BATseqbase(bn, 0);
		} else if (bn->hsorted &&
			   (foid = *(oid *) BUNhloc(bni, BUNfirst(bn))) != oid_nil &&
			   foid + BATcount(bn) - 1 == *(oid *) BUNhloc(bni, BUNlast(bn) - 1)) {
			bn->hdense = TRUE;
			BATseqbase(bn, *(oid *) BUNhloc(bni, BUNfirst(bn)));
		}
	}
	if (BATtdense(b)) {
		bn->tdense = TRUE;
		BATseqbase(BATmirror(bn), (oid) (b->tseqbase + low));
	} else if (bn->tkey && bn->ttype == TYPE_oid) {
		if (BATcount(bn) == 0) {
			bn->tdense = TRUE;
			BATseqbase(BATmirror(bn), 0);
		} else if (bn->tsorted &&
			   (foid = *(oid *) BUNtloc(bni, BUNfirst(bn))) != oid_nil &&
			   foid + BATcount(bn) - 1 == *(oid *) BUNtloc(bni, BUNlast(bn) - 1)) {
			bn->tdense = TRUE;
			BATseqbase(BATmirror(bn), *(oid *) BUNtloc(bni, BUNfirst(bn)));
		}
	}
	if (bn->batCount <= 1) {
		bn->hsorted = ATOMlinear(b->htype);
		bn->tsorted = ATOMlinear(b->ttype);
		bn->hrevsorted = ATOMlinear(b->htype);
		bn->trevsorted = ATOMlinear(b->ttype);
		BATkey(bn, 1);
		BATkey(BATmirror(bn), 1);
	} else {
		bn->hsorted = BAThordered(b);
		bn->tsorted = BATtordered(b);
		bn->hrevsorted = BAThrevordered(b);
		bn->trevsorted = BATtrevordered(b);
		BATkey(bn, BAThkey(b));
		BATkey(BATmirror(bn), BATtkey(b));
	}
	bn->H->nonil = b->H->nonil || bn->batCount == 0;
	bn->T->nonil = b->T->nonil || bn->batCount == 0;
	bn->H->nil = bn->T->nil = 0;	/* we just don't know */
	return bn;
      bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

/*
 *  BAT Sorting
 * BATsort returns a sorted copy. BATorder sorts the BAT itself.
 */
int
BATordered(BAT *b)
{
	if (!b->hsorted)
		BATderiveHeadProps(b, 0);
	return b->hsorted;
}

int
BATordered_rev(BAT *b)
{
	if (!b->hrevsorted)
		BATderiveHeadProps(b, 0);
	return b->hrevsorted;
}

/* figure out which sort function is to be called
 * stable sort can produce an error (not enough memory available),
 * "quick" sort does not produce errors */
static gdk_return
do_sort(void *h, void *t, const void *base, size_t n, int hs, int ts, int tpe,
	int reverse, int stable)
{
	if (n <= 1)		/* trivially sorted */
		return GDK_SUCCEED;
	if (reverse) {
		if (stable) {
			if (GDKssort_rev(h, t, base, n, hs, ts, tpe) < 0) {
				return GDK_FAIL;
			}
		} else {
			GDKqsort_rev(h, t, base, n, hs, ts, tpe);
		}
	} else {
		if (stable) {
			if (GDKssort(h, t, base, n, hs, ts, tpe) < 0) {
				return GDK_FAIL;
			}
		} else {
			GDKqsort(h, t, base, n, hs, ts, tpe);
		}
	}
	return GDK_SUCCEED;
}

/* Sort b according to stable and reverse, do it in-place if copy is
 * unset, otherwise do it on a copy */
static BAT *
BATorder_internal(BAT *b, int stable, int reverse, int copy, const char *func)
{
	BATcheck(b, func, NULL);
	/* set some trivial properties (probably not necessary, but
	 * it's cheap) */
	if (b->htype == TYPE_void) {
		b->hsorted = 1;
		b->hrevsorted = b->hseqbase == oid_nil || b->batCount <= 1;
		b->hkey |= b->hseqbase != oid_nil;
	} else if (b->batCount <= 1) {
		b->hsorted = b->hrevsorted = 1;
	}
	if (reverse ? b->hrevsorted : b->hsorted) {
		/* b is already ordered as desired, hence we return b
		 * as is */
		return copy ? BATcopy(b, b->htype, b->ttype, FALSE, TRANSIENT) : b;
	}
	if (copy) {
		/* now make a writable copy that we're going to sort
		 * materialize any VOID columns while we're at it */
		b = BATcopy(b, BAThtype(b), BATttype(b), TRUE, TRANSIENT);
	} else if (b->ttype == TYPE_void && b->tseqbase != oid_nil) {
		/* materialize void-tail in-place */
		/* note, we don't need to materialize the head column:
		 * if it is void, either we didn't get here (already
		 * sorted correctly), or we will fall into BATrevert
		 * below which does the materialization for us */
		if (BATmaterializet(b) != GDK_SUCCEED)
			return NULL;
	}

	if ((reverse ? b->hsorted : b->hrevsorted) && (!stable || b->hkey)) {
		/* b is ordered in the opposite direction, hence we
		 * revert b (note that if requesting stable sort, the
		 * column needs to be key) */
		return BATrevert(b) == GDK_SUCCEED ? b : NULL;
	}
	if (!(reverse ? b->hrevsorted : b->hsorted) &&
	    do_sort(Hloc(b, BUNfirst(b)), Tloc(b, BUNfirst(b)),
		    b->H->vheap ? b->H->vheap->base : NULL,
		    BATcount(b), Hsize(b), Tsize(b), b->htype,
		    reverse, stable) != GDK_SUCCEED) {
		if (copy)
			BBPreclaim(b);
		return NULL;
	}
	if (reverse) {
		b->hrevsorted = 1;
		b->hsorted = b->batCount <= 1;
	} else {
		b->hsorted = 1;
		b->hrevsorted = b->batCount <= 1;
	}
	b->tsorted = b->trevsorted = 0;
	HASHdestroy(b);
	IMPSdestroy(b);
	ALIGNdel(b, func, FALSE, NULL);
	b->hdense = 0;
	b->tdense = 0;
	b->batDirtydesc = b->H->heap.dirty = b->T->heap.dirty = TRUE;

	return b;
}

#undef BATorder
#undef BATorder_rev
#undef BATsort
#undef BATsort_rev
#undef BATssort
#undef BATssort_rev

gdk_return
BATorder(BAT *b)
{
	return BATorder_internal(b, 0, 0, 0, "BATorder") ? GDK_SUCCEED : GDK_FAIL;
}

gdk_return
BATorder_rev(BAT *b)
{
	return BATorder_internal(b, 0, 1, 0, "BATorder_rev") ? GDK_SUCCEED : GDK_FAIL;
}

BAT *
BATsort(BAT *b)
{
	return BATorder_internal(b, 0, 0, 1, "BATsort");
}

BAT *
BATsort_rev(BAT *b)
{
	return BATorder_internal(b, 0, 1, 1, "BATsort_rev");
}

BAT *
BATssort(BAT *b)
{
	return BATorder_internal(b, 1, 0, 1, "BATssort");
}

BAT *
BATssort_rev(BAT *b)
{
	return BATorder_internal(b, 1, 1, 1, "BATssort_rev");
}

/* subsort the bat b according to both o and g.  The stable and
 * reverse parameters indicate whether the sort should be stable or
 * descending respectively.  The parameter b is required, o and g are
 * optional (i.e., they may be NULL).
 *
 * A sorted copy is returned through the sorted parameter, the new
 * ordering is returned through the order parameter, group information
 * is returned through the groups parameter.  All three output
 * parameters may be NULL.  If they're all NULL, this function does
 * nothing.
 *
 * All BATs involved must be dense-headed.
 *
 * If o is specified, it is used to first rearrange b according to the
 * order specified in o, after which b is sorted taking g into
 * account.
 *
 * If g is specified, it indicates groups which should be individually
 * ordered.  Each row of consecutive equal values in g indicates a
 * group which is sorted according to stable and reverse.  g is used
 * after the order in b was rearranged according to o.
 *
 * The outputs order and groups can be used in subsequent calls to
 * this function.  This can be used if multiple BATs need to be sorted
 * together.  The BATs should then be sorted in order of significance,
 * and each following call should use the original unordered BAT plus
 * the order and groups bat from the previous call.  In this case, the
 * sorted BATs are not of much use, so the sorted output parameter
 * does not need to be specified.
 */
gdk_return
BATsubsort(BAT **sorted, BAT **order, BAT **groups,
	   BAT *b, BAT *o, BAT *g, int reverse, int stable)
{
	BAT *bn = NULL, *on = NULL, *gn;
	oid *restrict grps, prev;
	BUN p, q, r;

	if (b == NULL || !BAThdense(b)) {
		GDKerror("BATsubsort: b must be dense-headed\n");
		return GDK_FAIL;
	}
	if (o != NULL &&
	    (!BAThdense(o) ||		       /* dense head */
	     ATOMtype(o->ttype) != TYPE_oid || /* oid tail */
	     BATcount(o) != BATcount(b) ||     /* same size as b */
	     (o->ttype == TYPE_void &&	       /* no nil tail */
	      BATcount(o) != 0 &&
	      o->tseqbase == oid_nil))) {
		GDKerror("BATsubsort: o must be [dense,oid] and same size as b\n");
		return GDK_FAIL;
	}
	if (g != NULL &&
	    (!BAThdense(g) ||		       /* dense head */
	     ATOMtype(g->ttype) != TYPE_oid || /* oid tail */
	     !g->tsorted ||		       /* sorted */
	     BATcount(o) != BATcount(b) ||     /* same size as b */
	     (g->ttype == TYPE_void &&	       /* no nil tail */
	      BATcount(g) != 0 &&
	      g->tseqbase == oid_nil))) {
		GDKerror("BATsubsort: g must be [dense,oid], sorted on the tail, and same size as b\n");
		return GDK_FAIL;
	}
	assert(reverse == 0 || reverse == 1);
	assert(stable == 0 || stable == 1);
	if (sorted == NULL && order == NULL && groups == NULL) {
		/* no place to put result, so we're done quickly */
		return GDK_SUCCEED;
	}
	if (BATcount(b) <= 1 ||
	    ((reverse ? BATtrevordered(b) : BATtordered(b)) &&
	     o == NULL && g == NULL &&
	     (groups == NULL || BATtkey(b) ||
	      (reverse ? BATtordered(b) : BATtrevordered(b))))) {
		/* trivially (sub)sorted, and either we don't need to
		 * return group information, or we can trivially
		 * deduce the groups */
		if (sorted) {
			bn = BATcopy(b, TYPE_void, b->ttype, 0, TRANSIENT);
			if (bn == NULL)
				goto error;
			*sorted = bn;
		}
		if (order) {
			on = BATnew(TYPE_void, TYPE_void, BATcount(b), TRANSIENT);
			if (on == NULL)
				goto error;
			BATsetcount(on, BATcount(b));
			BATseqbase(on, b->hseqbase);
			BATseqbase(BATmirror(on), b->hseqbase);
			*order = on;
		}
		if (groups) {
			if (BATtkey(b)) {
				/* singleton groups */
				gn = BATnew(TYPE_void, TYPE_void, BATcount(b), TRANSIENT);
				if (gn == NULL)
					goto error;
				BATsetcount(gn, BATcount(b));
				BATseqbase(BATmirror(gn), 0);
			} else {
				/* single group */
				const oid *o = 0;
				assert(BATcount(b) == 1 ||
				       (BATtordered(b) && BATtrevordered(b)));
				gn = BATconstant(TYPE_oid, &o, BATcount(b), TRANSIENT);
				if (gn == NULL)
					goto error;
			}
			BATseqbase(gn, 0);
			*groups = gn;
		}
		return GDK_SUCCEED;
	}
	if (o) {
		bn = BATproject(o, b);
		if (bn == NULL)
			goto error;
		if (bn->ttype == TYPE_void || isVIEW(bn)) {
			b = BATcopy(bn, TYPE_void, ATOMtype(bn->ttype), TRUE, TRANSIENT);
			BBPunfix(bn->batCacheid);
			bn = b;
		}
	} else {
		bn = BATcopy(b, TYPE_void, b->ttype, TRUE, TRANSIENT);
	}
	if (bn == NULL)
		goto error;
	if (order) {
		/* prepare order bat */
		if (o) {
			/* make copy of input so that we can refine it;
			 * copy can be read-only if we take the shortcut
			 * below in the case g is "key" */
			on = BATcopy(o, TYPE_void, TYPE_oid,
				     g == NULL ||
				     !(g->tkey || g->ttype == TYPE_void),
				     TRANSIENT);
			if (on == NULL)
				goto error;
		} else {
			/* create new order */
			on = BATnew(TYPE_void, TYPE_oid, BATcount(bn), TRANSIENT);
			if (on == NULL)
				goto error;
			grps = (oid *) Tloc(on, BUNfirst(on));
			for (p = 0, q = BATcount(bn); p < q; p++)
				grps[p] = p + b->hseqbase;
			BATsetcount(on, BATcount(bn));
			on->tkey = 1;
			on->T->nil = 0;
			on->T->nonil = 1;
		}
		BATseqbase(on, b->hseqbase);
		on->tsorted = on->trevsorted = 0; /* it won't be sorted */
		on->tdense = 0;			  /* and hence not dense */
		*order = on;
	}
	if (g) {
		if (g->tkey || g->ttype == TYPE_void) {
			/* if g is "key", all groups are size 1, so no
			 * subsorting needed */
			if (sorted) {
				*sorted = bn;
			} else {
				BBPunfix(bn->batCacheid);
			}
			if (order) {
				*order = on;
				if (o) {
					/* we can inherit sortedness
					 * after all */
					on->tsorted = o->tsorted;
					on->trevsorted = o->trevsorted;
				} else {
					/* we didn't rearrange, so
					 * still sorted */
					on->tsorted = 1;
					on->trevsorted = 0;
				}
				if (BATcount(on) <= 1) {
					on->tsorted = 1;
					on->trevsorted = 1;
				}
			}
			if (groups) {
				gn = BATcopy(g, TYPE_void, g->ttype, 0, TRANSIENT);
				if (gn == NULL)
					goto error;
				*groups = gn;
			}
			return GDK_SUCCEED;
		}
		assert(g->ttype == TYPE_oid);
		grps = (oid *) Tloc(g, BUNfirst(g));
		prev = grps[0];
		for (r = 0, p = 1, q = BATcount(g); p < q; p++) {
			if (grps[p] != prev) {
				/* sub sort [r,p) */
				if (do_sort(Tloc(bn, BUNfirst(bn) + r),
					    on ? Tloc(on, BUNfirst(on) + r) : NULL,
					    bn->T->vheap ? bn->T->vheap->base : NULL,
					    p - r, Tsize(bn), on ? Tsize(on) : 0,
					    bn->ttype, reverse, stable) != GDK_SUCCEED)
					goto error;
				r = p;
				prev = grps[p];
			}
		}
		/* sub sort [r,q) */
		if (do_sort(Tloc(bn, BUNfirst(bn) + r),
			    on ? Tloc(on, BUNfirst(on) + r) : NULL,
			    bn->T->vheap ? bn->T->vheap->base : NULL,
			    p - r, Tsize(bn), on ? Tsize(on) : 0,
			    bn->ttype, reverse, stable) != GDK_SUCCEED)
			goto error;
		/* if single group (r==0) the result is (rev)sorted,
		 * otherwise not */
		bn->tsorted = r == 0 && !reverse;
		bn->trevsorted = r == 0 && reverse;
	} else {
		if (b->ttype == TYPE_void) {
			b->tsorted = 1;
			b->trevsorted = b->tseqbase == oid_nil || b->batCount <= 1;
			b->tkey |= b->tseqbase != oid_nil;
		} else if (b->batCount <= 1) {
			b->tsorted = b->trevsorted = 1;
		}
		if (!(reverse ? bn->trevsorted : bn->tsorted) &&
		    do_sort(Tloc(bn, BUNfirst(bn)),
			    on ? Tloc(on, BUNfirst(on)) : NULL,
			    bn->T->vheap ? bn->T->vheap->base : NULL,
			    BATcount(bn), Tsize(bn), on ? Tsize(on) : 0,
			    bn->ttype, reverse, stable) != GDK_SUCCEED)
			goto error;
		bn->tsorted = !reverse;
		bn->trevsorted = reverse;
	}
	if (groups) {
		if (BATgroup_internal(groups, NULL, NULL, bn, g, NULL, NULL, 1) != GDK_SUCCEED)
			goto error;
		if ((*groups)->tkey && (bn->tsorted || bn->trevsorted)) {
			/* if new groups bat is key and the result bat
			 * is (rev)sorted (single input group), we
			 * know it is key */
			bn->tkey = 1;
		}
	}

	if (sorted)
		*sorted = bn;
	else
		BBPunfix(bn->batCacheid);

	return GDK_SUCCEED;

  error:
	if (bn)
		BBPunfix(bn->batCacheid);
	BBPreclaim(on);
	if (sorted)
		*sorted = NULL;
	if (order)
		*order = NULL;
	if (groups)
		*groups = NULL;
	return GDK_FAIL;
}

/*
 * Reverse a BAT
 * BATrevert rearranges a BAT in reverse order.
 */
gdk_return
BATrevert(BAT *b)
{
	char *h, *t;
	BUN p, q;
	BATiter bi = bat_iterator(b);
	size_t s;
	int x;

	BATcheck(b, "BATrevert", GDK_FAIL);
	if ((b->htype == TYPE_void && b->hseqbase != oid_nil) || (b->ttype == TYPE_void && b->tseqbase != oid_nil)) {
		/* materialize void columns in-place */
		if (BATmaterialize(b) != GDK_SUCCEED)
			return GDK_FAIL;
	}
	ALIGNdel(b, "BATrevert", FALSE, GDK_FAIL);
	s = Hsize(b);
	if (s > 0) {
		h = (char *) GDKmalloc(s);
		if (!h) {
			return GDK_FAIL;
		}
		for (p = BUNlast(b) ? BUNlast(b) - 1 : 0, q = BUNfirst(b); p > q; p--, q++) {
			char *ph = BUNhloc(bi, p);
			char *qh = BUNhloc(bi, q);

			memcpy(h, ph, s);
			memcpy(ph, qh, s);
			memcpy(qh, h, s);
		}
		GDKfree(h);
	}
	s = Tsize(b);
	if (s > 0) {
		t = (char *) GDKmalloc(s);
		if (!t) {
			return GDK_FAIL;
		}
		for (p = BUNlast(b) ? BUNlast(b) - 1 : 0, q = BUNfirst(b); p > q; p--, q++) {
			char *pt = BUNtloc(bi, p);
			char *qt = BUNtloc(bi, q);

			memcpy(t, pt, s);
			memcpy(pt, qt, s);
			memcpy(qt, t, s);
		}
		GDKfree(t);
	}
	HASHdestroy(b);
	IMPSdestroy(b);
	/* interchange sorted and revsorted */
	x = b->hrevsorted;
	b->hrevsorted = b->hsorted;
	b->hsorted = x;
	x = b->trevsorted;
	b->trevsorted = b->tsorted;
	b->tsorted = x;
	b->hdense = FALSE;
	b->tdense = FALSE;
	return GDK_SUCCEED;
}

/*
 * return a view on b consisting of the head of b and a new dense tail
 * starting at oid_base.
 */
BAT *
BATmark(BAT *b, oid oid_base)
{
	BAT *bn;

	BATcheck(b, "BATmark", NULL);
	bn = VIEWhead(b);
	if (bn) {
		BATseqbase(BATmirror(bn), oid_base);
		if (oid_base == oid_nil)
			bn->T->nonil = 0;
		if (BAThrestricted(b) != BAT_READ) {
			BAT *v = bn;

			bn = BATcopy(v, v->htype, v->ttype, TRUE, TRANSIENT);
			BBPreclaim(v);
		}
	}
	return bn;
}

BAT *
BATmark_grp(BAT *g, BAT *e, const oid *s)
{
	BAT *bn;
	BAT *ec;
	oid *ecp, *bp;
	oid eco;
	const oid *gp;
	BUN i;

	BATcheck(g, "BATmark_grp", NULL);
	BATcheck(e, "BATmark_grp", NULL);
	ERRORcheck(g->ttype != TYPE_void && g->ttype != TYPE_oid,
		   "BATmark_grp: tail of BAT g must be oid.\n", NULL);
	ERRORcheck(e->htype != TYPE_void && e->htype != TYPE_oid,
		   "BATmark_grp: head of BAT e must be oid.\n", NULL);
	ERRORcheck(g->ttype == TYPE_void && g->tseqbase == oid_nil,
		   "BATmark_grp: tail of BAT g must not be nil.\n", NULL);
	ERRORcheck(e->htype == TYPE_void && e->hseqbase == oid_nil,
		   "BATmark_grp: head of BAT e must not be nil.\n", NULL);
	ERRORcheck(s && *s == oid_nil,
		   "BATmark_grp: base oid s must not be nil.\n", NULL);
	ERRORcheck(s == NULL && e->ttype != TYPE_oid,
		   "BATmark_grp: tail of BAT e must be oid.\n", NULL);

	assert(BAThdense(g));
	assert(BAThdense(e));
	assert(ATOMtype(g->ttype) == TYPE_oid);
	assert(ATOMtype(e->ttype) == TYPE_oid);
	assert(s == NULL || *s != oid_nil);

	if (BATtdense(g)) {
		if (s)
			return BATconst(g, TYPE_oid, s, TRANSIENT);
		return BATproject(g, e);
	}

	bn = BATnew(TYPE_void, TYPE_oid, BATcount(g), TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (s)
		ec = BATconst(e, TYPE_oid, s, TRANSIENT);
	else
		ec = BATcopy(e, TYPE_void, TYPE_oid, TRUE, TRANSIENT);
	if (ec == NULL) {
		BBPreclaim(bn);
		return NULL;
	}

	BATsetcount(bn, BATcount(g));
	BATseqbase(bn, g->hseqbase);
	bn->T->nil = 0;
	if (BATcount(g) <= 1) {
		bn->T->sorted = 1;
		bn->T->revsorted = 1;
		bn->T->key = 1;
	} else {
		bn->T->sorted = 0;
		bn->T->revsorted = 0;
		bn->T->key = 0;
	}
	ecp = (oid *) Tloc(ec, BUNfirst(ec));
	eco = ec->hseqbase;
	gp = (const oid *) Tloc(g, BUNfirst(g));
	bp = (oid *) Tloc(bn, BUNfirst(bn));

	for (i = 0; i < g->batCount; i++) {
		if (gp[i] < eco ||
		    gp[i] >= eco + BATcount(ec) ||
		    ecp[gp[i] - eco] == oid_nil) {
			bp[i] = oid_nil;
			bn->T->nil = 1;
		} else {
			bp[i] = ecp[gp[i] - eco]++;
		}
	}
	BBPreclaim(ec);
	bn->T->nonil = !bn->T->nil;
	if (BATcount(e) == 1 && bn->T->nonil) {
		bn->T->sorted = 1;
		bn->T->dense = 1;
		bn->T->key = 1;
		if (BATcount(bn) > 0)
			bn->T->seq = bp[0];
		else
			bn->T->seq = 0;
	}
	return bn;
}

/* return a new BAT of length n with a dense head and the constant v
 * in the tail */
BAT *
BATconstant(int tailtype, const void *v, BUN n, int role)
{
	BAT *bn;
	void *restrict p;
	BUN i;

	if (v == NULL)
		return NULL;
	bn = BATnew(TYPE_void, tailtype, n, role);
	if (bn == NULL)
		return NULL;
	p = Tloc(bn, bn->batFirst);
	switch (ATOMstorage(tailtype)) {
	case TYPE_void:
		v = &oid_nil;
		BATseqbase(BATmirror(bn), oid_nil);
		break;
	case TYPE_bte:
		for (i = 0; i < n; i++)
			((bte *) p)[i] = *(bte *) v;
		break;
	case TYPE_sht:
		for (i = 0; i < n; i++)
			((sht *) p)[i] = *(sht *) v;
		break;
	case TYPE_int:
	case TYPE_flt:
		assert(sizeof(int) == sizeof(flt));
		for (i = 0; i < n; i++)
			((int *) p)[i] = *(int *) v;
		break;
	case TYPE_lng:
	case TYPE_dbl:
		assert(sizeof(lng) == sizeof(dbl));
		for (i = 0; i < n; i++)
			((lng *) p)[i] = *(lng *) v;
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		for (i = 0; i < n; i++)
			((hge *) p)[i] = *(hge *) v;
		bn->T->nil = n >= 1 && *(hge *) v == hge_nil;
		break;
#endif
	default:
		for (i = BUNfirst(bn), n += i; i < n; i++)
			tfastins_nocheck(bn, i, v, Tsize(bn));
		n -= BUNfirst(bn);
		break;
	}
	bn->T->nil = n >= 1 && (*ATOMcompare(tailtype))(v, ATOMnilptr(tailtype)) == 0;
	BATsetcount(bn, n);
	bn->tsorted = 1;
	bn->trevsorted = 1;
	bn->T->nonil = !bn->T->nil;
	bn->T->key = BATcount(bn) <= 1;
	BATseqbase(bn, 0);
	return bn;

  bunins_failed:
	BBPreclaim(bn);
	return NULL;
}

/* return a new bat which is aligned with b and with the constant v in
 * the tail */
BAT *
BATconst(BAT *b, int tailtype, const void *v, int role)
{
	BAT *bn;

	BATcheck(b, "BATconst", NULL);
	bn = BATconstant(tailtype, v, BATcount(b), role);
	if (bn == NULL)
		return NULL;
	if (b->H->type != bn->H->type) {
		BAT *bnn = VIEWcreate(b, bn);

		BBPunfix(bn->batCacheid);
		bn = bnn;
	} else {
		BATseqbase(bn, b->hseqbase);
	}

	return bn;
}

/*
 * BAT Aggregates
 *
 * We retain the size() and card() aggregate results in the column
 * descriptor.  We would like to have such functionality in an
 * extensible way for many aggregates, for DD (1) we do not want to
 * change the binary BAT format on disk and (2) aggr and size are the
 * most relevant aggregates.
 *
 * It is all hacked into the aggr[3] records; three adjacent integers
 * that were left over in the column record. We refer to these as if
 * it where an int aggr[3] array.  The below routines set and retrieve
 * the aggregate values from the tail of the BAT, as many
 * aggregate-manipulating BAT functions work on tail.
 *
 * The rules are as follows: aggr[0] contains the alignment ID of the
 * column (if set i.e. nonzero).  Hence, if this value is nonzero and
 * equal to b->talign, the precomputed aggregate values in
 * aggr[GDK_AGGR_SIZE] and aggr[GDK_AGGR_CARD] hold. However, only one
 * of them may be set at the time. This is encoded by the value
 * int_nil, which cannot occur in these two aggregates.
 *
 * This was now extended to record the property whether we know there
 * is a nil value present by mis-using the highest bits of both
 * GDK_AGGR_SIZE and GDK_AGGR_CARD.
 */

void
PROPdestroy(PROPrec *p)
{
	PROPrec *n;

	while (p) {
		n = p->next;
		if (p->v.vtype == TYPE_str)
			GDKfree(p->v.val.sval);
		GDKfree(p);
		p = n;
	}
}

PROPrec *
BATgetprop(BAT *b, int idx)
{
	PROPrec *p = b->T->props;

	while (p) {
		if (p->id == idx)
			return p;
		p = p->next;
	}
	return NULL;
}

void
BATsetprop(BAT *b, int idx, int type, void *v)
{
	ValRecord vr;
	PROPrec *p = BATgetprop(b, idx);

	if (!p && (p = (PROPrec *) GDKmalloc(sizeof(PROPrec))) != NULL) {
		p->id = idx;
		p->next = b->T->props;
		p->v.vtype = 0;
		b->T->props = p;
	}
	if (p) {
		VALset(&vr, type, v);
		VALcopy(&p->v, &vr);
		b->batDirtydesc = TRUE;
	}
}


/*
 * The BATcount_no_nil function counts all BUN in a BAT that have a
 * non-nil tail value.
 */
BUN
BATcount_no_nil(BAT *b)
{
	BUN cnt = 0;
	BUN i, n;
	const void *restrict p, *restrict nil;
	const char *restrict base;
	int t;
	int (*cmp)(const void *, const void *);

	BATcheck(b, "BATcnt", 0);
	n = BATcount(b);
	if (b->T->nonil)
		return n;
	p = Tloc(b, b->batFirst);
	t = ATOMbasetype(b->ttype);
	switch (t) {
	case TYPE_void:
		cnt = b->tseqbase == oid_nil ? 0 : n;
		break;
	case TYPE_bte:
		for (i = 0; i < n; i++)
			cnt += ((const bte *) p)[i] != bte_nil;
		break;
	case TYPE_sht:
		for (i = 0; i < n; i++)
			cnt += ((const sht *) p)[i] != sht_nil;
		break;
	case TYPE_int:
		for (i = 0; i < n; i++)
			cnt += ((const int *) p)[i] != int_nil;
		break;
	case TYPE_lng:
		for (i = 0; i < n; i++)
			cnt += ((const lng *) p)[i] != lng_nil;
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		for (i = 0; i < n; i++)
			cnt += ((const hge *) p)[i] != hge_nil;
		break;
#endif
	case TYPE_flt:
		for (i = 0; i < n; i++)
			cnt += ((const flt *) p)[i] != flt_nil;
		break;
	case TYPE_dbl:
		for (i = 0; i < n; i++)
			cnt += ((const dbl *) p)[i] != dbl_nil;
		break;
	case TYPE_str:
		base = b->T->vheap->base;
		switch (b->T->width) {
		case 1:
			for (i = 0; i < n; i++)
				cnt += base[((var_t) ((const unsigned char *) p)[i] + GDK_VAROFFSET) << GDK_VARSHIFT] != '\200';
			break;
		case 2:
			for (i = 0; i < n; i++)
				cnt += base[((var_t) ((const unsigned short *) p)[i] + GDK_VAROFFSET) << GDK_VARSHIFT] != '\200';
			break;
#if SIZEOF_VAR_T != SIZEOF_INT
		case 4:
			for (i = 0; i < n; i++)
				cnt += base[(var_t) ((const unsigned int *) p)[i] << GDK_VARSHIFT] != '\200';
			break;
#endif
		default:
			for (i = 0; i < n; i++)
				cnt += base[((const var_t *) p)[i] << GDK_VARSHIFT] != '\200';
			break;
		}
		break;
	default:
		nil = ATOMnilptr(t);
		cmp = ATOMcompare(t);
		if (b->tvarsized) {
			base = b->T->vheap->base;
			for (i = 0; i < n; i++)
				cnt += (*cmp)(nil, base + ((const var_t *) p)[i]) != 0;
		} else {
			for (i = BUNfirst(b), n += i; i < n; i++)
				cnt += (*cmp)(Tloc(b, i), nil) != 0;
		}
		break;
	}
	if (cnt == BATcount(b)) {
		/* we learned something */
		b->T->nonil = 1;
		assert(b->T->nil == 0);
		b->T->nil = 0;
	}
	return cnt;
}

static BAT *
newdensecand(oid first, oid last)
{
	BAT *bn;

	if ((bn = BATnew(TYPE_void, TYPE_void, 0, TRANSIENT)) == NULL)
		return NULL;
	if (last < first)
		first = last = 0; /* empty range */
	BATsetcount(bn, last - first + 1);
	BATseqbase(bn, 0);
	BATseqbase(BATmirror(bn), first);
	return bn;
}

/* merge two candidate lists and produce a new one
 *
 * candidate lists are VOID-headed BATs with an OID tail which is
 * sorted and unique.
 */
BAT *
BATmergecand(BAT *a, BAT *b)
{
	BAT *bn;
	const oid *restrict ap, *restrict bp, *ape, *bpe;
	oid *restrict p, i;
	oid af, al, bf, bl;
	BATiter ai, bi;
	bit ad, bd;

	BATcheck(a, "BATmergecand", NULL);
	BATcheck(b, "BATmergecand", NULL);
	assert(a->htype == TYPE_void);
	assert(b->htype == TYPE_void);
	assert(ATOMtype(a->ttype) == TYPE_oid);
	assert(ATOMtype(b->ttype) == TYPE_oid);
	assert(BATcount(a) <= 1 || a->tsorted);
	assert(BATcount(b) <= 1 || b->tsorted);
	assert(BATcount(a) <= 1 || a->tkey);
	assert(BATcount(b) <= 1 || b->tkey);
	assert(a->T->nonil);
	assert(b->T->nonil);

	/* we can return a if b is empty (and v.v.) */
	if (BATcount(a) == 0) {
		return BATcopy(b, b->htype, b->ttype, 0, TRANSIENT);
	}
	if (BATcount(b) == 0) {
		return BATcopy(a, a->htype, a->ttype, 0, TRANSIENT);
	}
	/* we can return a if a fully covers b (and v.v) */
	ai = bat_iterator(a);
	bi = bat_iterator(b);
	af = *(oid*) BUNtail(ai, BUNfirst(a));
	bf = *(oid*) BUNtail(bi, BUNfirst(b));
	al = *(oid*) BUNtail(ai, BUNlast(a) - 1);
	bl = *(oid*) BUNtail(bi, BUNlast(b) - 1);
	ad = (af + BATcount(a) - 1 == al); /* i.e., dense */
	bd = (bf + BATcount(b) - 1 == bl); /* i.e., dense */
	if (ad && bd) {
		/* both are dense */
		if (af <= bf && bf <= al + 1) {
			/* partial overlap starting with a, or b is
			 * smack bang after a */
			return newdensecand(af, al < bl ? bl : al);
		}
		if (bf <= af && af <= bl + 1) {
			/* partial overlap starting with b, or a is
			 * smack bang after b */
			return newdensecand(bf, al < bl ? bl : al);
		}
	}
	if (ad && af <= bf && al >= bl) {
		return newdensecand(af, al);
	}
	if (bd && bf <= af && bl >= al) {
		return newdensecand(bf, bl);
	}

	bn = BATnew(TYPE_void, TYPE_oid, BATcount(a) + BATcount(b), TRANSIENT);
	if (bn == NULL)
		return NULL;
	p = (oid *) Tloc(bn, BUNfirst(bn));
	if (a->ttype == TYPE_void && b->ttype == TYPE_void) {
		/* both lists are VOID */
		if (a->tseqbase > b->tseqbase) {
			BAT *t = a;

			a = b;
			b = t;
		}
		/* a->tseqbase <= b->tseqbase */
		for (i = a->tseqbase; i < a->tseqbase + BATcount(a); i++)
			*p++ = i;
		for (i = MAX(b->tseqbase, i);
		     i < b->tseqbase + BATcount(b);
		     i++)
			*p++ = i;
	} else if (a->ttype == TYPE_void || b->ttype == TYPE_void) {
		if (b->ttype == TYPE_void) {
			BAT *t = a;

			a = b;
			b = t;
		}
		/* a->ttype == TYPE_void, b->ttype == TYPE_oid */
		bp = (const oid *) Tloc(b, BUNfirst(b));
		bpe = bp + BATcount(b);
		while (bp < bpe && *bp < a->tseqbase)
			*p++ = *bp++;
		for (i = a->tseqbase; i < a->tseqbase + BATcount(a); i++)
			*p++ = i;
		while (bp < bpe && *bp < i)
			bp++;
		while (bp < bpe)
			*p++ = *bp++;
	} else {
		/* a->ttype == TYPE_oid, b->ttype == TYPE_oid */
		ap = (const oid *) Tloc(a, BUNfirst(a));
		ape = ap + BATcount(a);
		bp = (const oid *) Tloc(b, BUNfirst(b));
		bpe = bp + BATcount(b);
		while (ap < ape && bp < bpe) {
			if (*ap < *bp)
				*p++ = *ap++;
			else if (*ap > *bp)
				*p++ = *bp++;
			else {
				*p++ = *ap++;
				bp++;
			}
		}
		while (ap < ape)
			*p++ = *ap++;
		while (bp < bpe)
			*p++ = *bp++;
	}

	/* properties */
	BATsetcount(bn, (BUN) (p - (oid *) Tloc(bn, BUNfirst(bn))));
	BATseqbase(bn, 0);
	bn->trevsorted = BATcount(bn) <= 1;
	bn->tsorted = 1;
	bn->tkey = 1;
	bn->T->nil = 0;
	bn->T->nonil = 1;
	return virtualize(bn);
}

/* intersect two candidate lists and produce a new one
 *
 * candidate lists are VOID-headed BATs with an OID tail which is
 * sorted and unique.
 */
BAT *
BATintersectcand(BAT *a, BAT *b)
{
	BAT *bn;
	const oid *restrict ap, *restrict bp, *ape, *bpe;
	oid *restrict p;
	oid af, al, bf, bl;
	BATiter ai, bi;

	BATcheck(a, "BATintersectcand", NULL);
	BATcheck(b, "BATintersectcand", NULL);
	assert(a->htype == TYPE_void);
	assert(b->htype == TYPE_void);
	assert(ATOMtype(a->ttype) == TYPE_oid);
	assert(ATOMtype(b->ttype) == TYPE_oid);
	assert(a->tsorted);
	assert(b->tsorted);
	assert(a->tkey);
	assert(b->tkey);
	assert(a->T->nonil);
	assert(b->T->nonil);

	if (BATcount(a) == 0 || BATcount(b) == 0) {
		return newdensecand(0, 0);
	}

	ai = bat_iterator(a);
	bi = bat_iterator(b);
	af = *(oid*) BUNtail(ai, BUNfirst(a));
	bf = *(oid*) BUNtail(bi, BUNfirst(b));
	al = *(oid*) BUNtail(ai, BUNlast(a) - 1);
	bl = *(oid*) BUNtail(bi, BUNlast(b) - 1);

	if ((af + BATcount(a) - 1 == al) && (bf + BATcount(b) - 1 == bl)) {
		/* both lists are VOID */
		return newdensecand(MAX(af, bf), MIN(al, bl));
	}

	bn = BATnew(TYPE_void, TYPE_oid, MIN(BATcount(a), BATcount(b)), TRANSIENT);
	if (bn == NULL)
		return NULL;
	p = (oid *) Tloc(bn, BUNfirst(bn));
	if (a->ttype == TYPE_void || b->ttype == TYPE_void) {
		if (b->ttype == TYPE_void) {
			BAT *t = a;

			a = b;
			b = t;
		}
		/* a->ttype == TYPE_void, b->ttype == TYPE_oid */
		bp = (const oid *) Tloc(b, BUNfirst(b));
		bpe = bp + BATcount(b);
		while (bp < bpe && *bp < a->tseqbase)
			bp++;
		while (bp < bpe && *bp < a->tseqbase + BATcount(a))
			*p++ = *bp++;
	} else {
		/* a->ttype == TYPE_oid, b->ttype == TYPE_oid */
		ap = (const oid *) Tloc(a, BUNfirst(a));
		ape = ap + BATcount(a);
		bp = (const oid *) Tloc(b, BUNfirst(b));
		bpe = bp + BATcount(b);
		while (ap < ape && bp < bpe) {
			if (*ap < *bp)
				ap++;
			else if (*ap > *bp)
				bp++;
			else {
				*p++ = *ap++;
				bp++;
			}
		}
	}

	/* properties */
	BATsetcount(bn, (BUN) (p - (oid *) Tloc(bn, BUNfirst(bn))));
	BATseqbase(bn, 0);
	bn->trevsorted = BATcount(bn) <= 1;
	bn->tsorted = 1;
	bn->tkey = 1;
	bn->T->nil = 0;
	bn->T->nonil = 1;
	return virtualize(bn);
}
