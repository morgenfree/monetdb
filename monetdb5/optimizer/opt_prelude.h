/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

#ifndef MAL_PRELUDE
#define MAL_PRELUDE
#include "opt_support.h"

/* cf., gdk/gdk.mx */
#define DEBUGoptimizers		if (GDKdebug & GRPoptimizers)

opt_export  str abortRef;
opt_export  str affectedRowsRef;
opt_export  str aggrRef;
opt_export  str alarmRef;
opt_export  str algebraRef;
opt_export  str batalgebraRef;
opt_export  str appendidxRef;
opt_export  str appendRef;
opt_export  str assertRef;
opt_export  str attachRef;
opt_export  str avgRef;
opt_export  str arrayRef;
opt_export  str basketRef;
opt_export  str batcalcRef;
opt_export  str batRef;
opt_export  str boxRef;
opt_export  str batstrRef;
opt_export  str batmtimeRef;
opt_export  str batmmathRef;
opt_export  str batxmlRef;
opt_export  str bbpRef;
opt_export  str tidRef;
opt_export  str dateRef;
opt_export  str deltaRef;
opt_export  str subdeltaRef;
opt_export  str projectdeltaRef;
opt_export  str binddbatRef;
opt_export  str bindidxRef;
opt_export  str bindRef;
opt_export  str bpmRef;
opt_export  str bstreamRef;
opt_export  str calcRef;
opt_export  str catalogRef;
opt_export  str clear_tableRef;
opt_export  str closeRef;
opt_export  str columnRef;
opt_export  str commitRef;
opt_export  str columnBindRef;
opt_export  str connectRef;
opt_export  str constraintsRef;
opt_export  str countRef;
opt_export  str subcountRef;
opt_export  str copyRef;
opt_export  str copy_fromRef;
opt_export  str export_tableRef;
opt_export  str count_no_nilRef;
opt_export  str crossRef;
opt_export  str createRef;
opt_export  str dataflowRef;
opt_export  str datacyclotronRef;
opt_export  str dblRef;
opt_export  str defineRef;
opt_export  str deleteRef;
opt_export  str depositRef;
opt_export  str differenceRef;
opt_export  str tdifferenceRef;
opt_export  str tintersectRef;
opt_export  str tdiffRef;
opt_export  str tinterRef;
opt_export  str mergecandRef;
opt_export  str mergepackRef;
opt_export  str intersectcandRef;
opt_export  str eqRef;
opt_export  str disconnectRef;
opt_export  str evalRef;
opt_export  str execRef;
opt_export  str expandRef;
opt_export	str exportOperationRef;
opt_export  str finishRef;
opt_export  str firstnRef;
opt_export  str getRef;
opt_export  str generatorRef;
opt_export  str grabRef;
opt_export  str groupRef;
opt_export  str subgroupRef;
opt_export  str subgroupdoneRef;
opt_export  str groupbyRef;
opt_export  str hgeRef;
opt_export  str hashRef;
opt_export  str identityRef;
opt_export  str ifthenelseRef;
opt_export  str inplaceRef;
opt_export  str insertRef;
opt_export  str intRef;
opt_export  str ioRef;
opt_export  str iteratorRef;
opt_export  str joinPathRef;
opt_export  str jsonRef;
opt_export  str joinRef;
opt_export  str antijoinRef;
opt_export  str bandjoinRef;
opt_export  str thetajoinRef;
opt_export  str subjoinRef;
opt_export  str subleftjoinRef;
opt_export  str subantijoinRef;
opt_export  str subbandjoinRef;
opt_export  str subrangejoinRef;
opt_export  str subthetajoinRef;
opt_export  str kdifferenceRef;
opt_export  str kunionRef;
opt_export  str languageRef;
opt_export  str leftfetchjoinRef;
opt_export  str leftfetchjoinPathRef;
opt_export  str leftjoinRef;
opt_export  str leftjoinPathRef;
opt_export  str likeselectRef;
opt_export  str ilikeselectRef;
opt_export  str likeuselectRef;
opt_export  str ilikeuselectRef;
opt_export  str listRef;
opt_export  str likeRef;
opt_export  str ilikeRef;
opt_export  str not_likeRef;
opt_export  str not_ilikeRef;
opt_export  str lockRef;
opt_export  str lookupRef;
opt_export  str malRef;
opt_export  str batmalRef;
opt_export  str mapiRef;
opt_export  str markRef;
opt_export  str mark_grpRef;
opt_export  str mtimeRef;
opt_export  str dense_rank_grpRef;
opt_export  str matRef;
opt_export  str max_no_nilRef;
opt_export  str maxRef;
opt_export  str submaxRef;
opt_export  str submedianRef;
opt_export  str mdbRef;
opt_export  str min_no_nilRef;
opt_export  str minRef;
opt_export  str subminRef;
opt_export  str mirrorRef;
opt_export  str mitosisRef;
opt_export  str mkeyRef;
opt_export  str mmathRef;
opt_export  str multiplexRef;
opt_export  str manifoldRef;
opt_export  str multicolumnRef;
opt_export  str mvcRef;
opt_export  str newRef;
opt_export  str notRef;
opt_export  str nextRef;
opt_export  str oidRef;
opt_export  str openRef;
opt_export  str optimizerRef;
opt_export  str parametersRef;
opt_export  str packRef;
opt_export  str pack2Ref;
opt_export  str passRef;
opt_export  str partitionRef;
opt_export  str pcreRef;
opt_export  str pinRef;
opt_export  str plusRef;
opt_export  str minusRef;
opt_export  str mulRef;
opt_export  str divRef;
opt_export  str printRef;
opt_export  str preludeRef;
opt_export  str prodRef;
opt_export  str subprodRef;
opt_export  str postludeRef;
opt_export  str profilerRef;
opt_export  str projectRef;
opt_export  str putRef;
opt_export  str querylogRef;
opt_export  str queryRef;
opt_export  str rapiRef;
opt_export  str batrapiRef;
opt_export  str subeval_aggrRef;
opt_export  str rankRef;
opt_export  str rank_grpRef;
opt_export  str rapiRef;
opt_export  str reconnectRef;
opt_export  str recycleRef;
opt_export  str refineRef;
opt_export  str refine_reverseRef;
opt_export  str registerRef;
opt_export  str remapRef;
opt_export  str remoteRef;
opt_export  str replaceRef;
opt_export  str replicatorRef;
opt_export  str resultSetRef;
opt_export  str reuseRef;
opt_export  str reverseRef;
opt_export  str rpcRef;
opt_export  str rsColumnRef;
opt_export  str schedulerRef;
opt_export  str selectNotNilRef;
opt_export  str seriesRef;
opt_export  str semaRef;
opt_export  str setAccessRef;
opt_export  str setWriteModeRef;
opt_export  str sinkRef;
opt_export  str sliceRef;
opt_export  str subsliceRef;
opt_export  str singleRef;
opt_export  str sortRef;
opt_export  str sortReverseRef;
opt_export  str sqlRef;
opt_export  str srvpoolRef;
opt_export  str streamsRef;
opt_export  str startRef;
opt_export  str stopRef;
opt_export  str strRef;
opt_export  str sumRef;
opt_export  str subsumRef;
opt_export  str subavgRef;
opt_export  str subsortRef;
opt_export  str timestampRef;
opt_export  str takeRef;
opt_export  str not_uniqueRef;
opt_export  str unpackRef;
opt_export  str unpinRef;
opt_export  str unlockRef;
opt_export  str updateRef;
opt_export  str subselectRef;
opt_export  str thetasubselectRef;
opt_export  str likesubselectRef;
opt_export  str ilikesubselectRef;
opt_export  str userRef;
opt_export  str vectorRef;
opt_export  str zero_or_oneRef;

opt_export int canBeCrackedProp;	/* binary */
opt_export int canBeJoinselectProp;	/* binary */
opt_export int sidewaysSelectProp;	/* int */
opt_export int headProp;		/* int */
opt_export int pivotProp;		/* int */
opt_export int pivotDisjunctiveProp;	/* int */
opt_export int removeProp;		/* int */
opt_export int tableProp;	        /* str */
opt_export int sqlfunctionProp;

opt_export int inlineProp;		/* binary */
opt_export int keepProp;		/* binary */
opt_export int notnilProp;		/* binary */
opt_export int rowsProp;		/* long */
opt_export int fileProp;			/* str */
opt_export int runonceProp;		/* binary */
opt_export int unsafeProp;		/* binary */

opt_export int stableProp;		/* binary */
opt_export int insertionsProp;		/* binary */
opt_export int updatesProp;		/* binary */
opt_export int deletesProp;		/* binary */

opt_export int hlbProp;			/* any (head lower bound) */
opt_export int hubProp;			/* any (head upper bound) */
opt_export int tlbProp;			/* any (tail lower bound) */
opt_export int tubProp;			/* any (tail upper bound) */
opt_export int horiginProp;		/* original oid source */
opt_export int toriginProp;		/* original oid source */
opt_export int mtProp;			/* enclosing merge table (id) */
#endif
