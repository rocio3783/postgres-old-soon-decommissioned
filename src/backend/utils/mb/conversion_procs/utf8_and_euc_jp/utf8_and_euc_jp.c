/*-------------------------------------------------------------------------
 *
 *	  EUC_JP <--> UTF-8
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/euc_jp_to_utf8.map"
#include "../../Unicode/utf8_to_euc_jp.map"

PG_FUNCTION_INFO_V1(euc_jp_to_utf8)
PG_FUNCTION_INFO_V1(utf8_to_euc_jp)

extern Datum euc_jp_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_euc_jp(PG_FUNCTION_ARGS);

/* ----------
 * conv_proc(
 *		INTEGER,	-- source encoding id
 *		INTEGER,	-- destination encoding id
 *		CSTRING,	-- source string (null terminated C string)
 *		CSTRING,	-- destination string (null terminated C string)
 *		INTEGER		-- source string length
 * ) returns VOID;
 * ----------
 */
Datum
euc_jp_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_EUC_JP);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len > 0);

	LocalToUtf(src, dest, LUmapEUC_JP,
			   sizeof(LUmapEUC_JP) / sizeof(pg_local_to_utf), PG_EUC_JP, len);

	PG_RETURN_INT32(0);
}

Datum
utf8_to_euc_jp(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_EUC_JP);
	Assert(len > 0);

	UtfToLocal(src, dest, ULmapEUC_JP,
			   sizeof(ULmapEUC_JP) / sizeof(pg_utf_to_local), len);

	PG_RETURN_INT32(0);
}
