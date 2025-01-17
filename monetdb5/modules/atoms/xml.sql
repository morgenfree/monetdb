-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0.  If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.
--
-- Copyright 2008-2015 MonetDB B.V.

CREATE TYPE xml EXTERNAL NAME xml;
CREATE FUNCTION xml (s STRING) RETURNS xml external name xml.xml;
CREATE FUNCTION str (s XML) RETURNS STRING external name xml.str;
CREATE FUNCTION comment (s STRING) RETURNS xml external name xml.comment;
CREATE FUNCTION parse (doccont STRING, val STRING, "option" STRING) RETURNS xml external name xml.parse;
CREATE FUNCTION pi (nme STRING, val STRING) RETURNS xml external name xml.pi;
CREATE FUNCTION root (val STRING, version STRING, standalone STRING) RETURNS xml external name xml.root;
CREATE FUNCTION attribute (nme STRING, val STRING) RETURNS xml external name xml.attribute;
CREATE FUNCTION "element" (nme STRING, ns xml, attr xml, s xml) RETURNS xml external name xml.element;
CREATE FUNCTION concat (val1 xml, val2 xml) RETURNS xml external name xml.concat;
CREATE FUNCTION forest (val1 xml, val2 xml) RETURNS xml external name xml.forest;
CREATE FUNCTION isdocument (val STRING) RETURNS xml external name xml.isdocument;


