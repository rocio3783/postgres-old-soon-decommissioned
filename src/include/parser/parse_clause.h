/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *	  handle clauses in parser
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void transformFromClause(ParseState *pstate, List *frmList);
extern int setTargetTable(ParseState *pstate, RangeVar *relation,
			   bool inh, bool alsoSource);
extern bool interpretInhOption(InhOption inhOpt);

extern Node *transformWhereClause(ParseState *pstate, Node *where);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
					 List *targetlist, List *sortClause);
extern List *transformSortClause(ParseState *pstate, List *orderlist,
					List *targetlist, bool resolveUnknown);
extern List *transformDistinctClause(ParseState *pstate, List *distinctlist,
						List *targetlist, List **sortClause);

extern List *addAllTargetsToSortList(ParseState *pstate,
									 List *sortlist, List *targetlist,
									 bool resolveUnknown);
extern Index assignSortGroupRef(TargetEntry *tle, List *tlist);
extern bool targetIsInSortList(TargetEntry *tle, List *sortList);

#endif   /* PARSE_CLAUSE_H */
