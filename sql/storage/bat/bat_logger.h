/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#ifndef BAT_LOGGER_H
#define BAT_LOGGER_H

#include "sql_storage.h"
#include <gdk_logger.h>

extern logger *bat_logger;

extern int bat_logger_init( logger_functions *lf );

#endif /*BAT_LOGGER_H */
