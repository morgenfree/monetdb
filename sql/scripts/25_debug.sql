-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0.  If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.
--
-- Copyright 2008-2015 MonetDB B.V.

-- show the optimizer statistics maintained by the SQL frontend
create function sys.optimizer_stats ()
	returns table (rewrite string, count int)
	external name sql.dump_opt_stats;


-- SQL QUERY CACHE
-- The SQL query cache returns a table with the query plans kept

create function sys.queryCache()
	returns table (query string, count int)
	external name sql.dump_cache;

-- Trace the SQL input
create procedure sys.querylog(filename string)
	external name sql.logfile;

-- MONETDB KERNEL SECTION
-- optimizer pipe catalog
create function sys.optimizers ()
	returns table (name string, def string, status string)
	external name sql.optimizers;
create view sys.optimizers as select * from sys.optimizers();

-- The environment table
create function sys.environment()
	returns table ("name" string, value string)
	external name sql.sql_environment;
create view sys.environment as select * from sys.environment();

-- The BAT buffer pool overview
create function sys.bbp ()
	returns table (id int, name string, htype string,
		ttype string, count BIGINT, refcnt int, lrefcnt int,
		location string, heat int, dirty string,
		status string, kind string)
	external name bbp.get;

create procedure sys.evalAlgebra( ra_stmt string, opt bool)
	external name sql."evalAlgebra";
