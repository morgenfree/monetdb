/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#ifndef BAT_UTILS_H
#define BAT_UTILS_H

#include "sql_storage.h"
#include <gdk_logger.h>

#define bat_set_access(b,access) b->batRestricted = access
#define bat_clear(b) bat_set_access(b,BAT_WRITE);BATclear(b,TRUE);bat_set_access(b,BAT_READ)

extern BAT *temp_descriptor(log_bid b);
extern BAT *quick_descriptor(log_bid b);
extern void temp_destroy(log_bid b);
extern void temp_dup(log_bid b);
extern log_bid temp_create(BAT *b);
extern log_bid temp_copy(log_bid b, int temp);

extern void bat_destroy(BAT *b);
extern BAT *bat_new(int ht, int tt, BUN size, int role);

extern BUN append_inserted(BAT *b, BAT *i );
extern BUN copy_inserted(BAT *b, BAT *i );

extern BAT *ebats[MAXATOMS];
extern BAT *eubats[MAXATOMS];

#define isEbat(b) 	(ebats[b->ttype] && ebats[b->ttype] == b) 
#define isEUbat(b) 	(eubats[b->ttype] && eubats[b->ttype] == b) 

extern log_bid ebat2real(log_bid b, oid ibase);
extern log_bid e_bat(int type);
extern BAT *e_BAT(int type);
extern log_bid e_ubat(int type);
extern log_bid ebat_copy(log_bid b, oid ibase, int temp);
extern log_bid eubat_copy(log_bid b, int temp);
extern void bat_utils_init(void);

extern sql_schema * tr_find_schema( sql_trans *tr, sql_schema *s);
extern sql_table * tr_find_table( sql_trans *tr, sql_table *t);
extern sql_column * tr_find_column( sql_trans *tr, sql_column *c);
extern sql_idx * tr_find_idx( sql_trans *tr, sql_idx *i);


#endif /* BAT_UTILS_H */
