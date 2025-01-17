-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0.  If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.
--
-- Copyright 2008-2015 MonetDB B.V.

-- COPY into reject management

create function sys.rejects()
returns table(
	rowid bigint,
	fldid int,
	"message" string,
	"input" string
)
external name sql.copy_rejects;

create view sys.rejects as select * from sys.rejects();
create procedure sys.clearrejects()
external name sql.copy_rejects_clear;
