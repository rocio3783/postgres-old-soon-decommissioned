/*-------------------------------------------------------------------------
 *
 * heaptuple.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef	HEAPTUPLE_H
#define HEAPTUPLE_H

#include <access/tupdesc.h>
#include <storage/buf.h>
#include <access/htup.h>

extern char *heap_getattr(HeapTuple tup,
                          Buffer b,
                          int attnum,
                          TupleDesc tupleDesc,
                          bool *isnull);

#endif	/* HEAP_TUPLE_H */ 
