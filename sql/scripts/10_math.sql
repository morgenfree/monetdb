-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0.  If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.
--
-- Copyright 2008-2015 MonetDB B.V.

CREATE FUNCTION degrees(r double)
RETURNS double
	RETURN r*180/pi();

CREATE FUNCTION radians(d double)
RETURNS double
	RETURN d*pi()/180;

