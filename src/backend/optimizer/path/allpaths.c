/*-------------------------------------------------------------------------
 *
 * allpaths.c--
 *	  Routines to find possible search paths for processing a query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdio.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "optimizer/internal.h"

#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/xfunc.h"
#include "optimizer/cost.h"

#include "commands/creatinh.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo.h"

#ifdef GEQO
bool		_use_geqo_ = true;

#else
bool		_use_geqo_ = false;

#endif
int32		_use_geqo_rels_ = GEQO_RELS;


static void find_rel_paths(Query *root, List *rels);
static List *find_join_paths(Query *root, List *outer_rels, int levels_needed);

#ifdef OPTIMIZER_DEBUG
static void debug_print_rel(Query *root, RelOptInfo *rel);

#endif

/*
 * find-paths--
 *	  Finds all possible access paths for executing a query, returning the
 *	  top level list of relation entries.
 *
 * 'rels' is the list of single relation entries appearing in the query
 */
List *
find_paths(Query *root, List *rels)
{
	int			levels_needed;

	/*
	 * Set the number of join (not nesting) levels yet to be processed.
	 */
	levels_needed = length(rels);

	if (levels_needed <= 0)
		return NIL;

	/*
	 * Find the base relation paths.
	 */
	find_rel_paths(root, rels);

	if (levels_needed <= 1)
	{
		/*
		 * Unsorted single relation, no more processing is required.
		 */
		return rels;
	}
	else
	{
		/*
		 * this means that joins or sorts are required. set selectivities
		 * of clauses that have not been set by an index.
		 */
		set_rest_relselec(root, rels);

		return find_join_paths(root, rels, levels_needed);
	}
}

/*
 * find-rel-paths--
 *	  Finds all paths available for scanning each relation entry in
 *	  'rels'.  Sequential scan and any available indices are considered
 *	  if possible(indices are not considered for lower nesting levels).
 *	  All unique paths are attached to the relation's 'pathlist' field.
 *
 *	  MODIFIES: rels
 */
static void
find_rel_paths(Query *root, List *rels)
{
	List	   *temp;
	List	   *lastpath;

	foreach(temp, rels)
	{
		List	   *sequential_scan_list;
		List	   *rel_index_scan_list;
		List	   *or_index_scan_list;
		RelOptInfo *rel = (RelOptInfo *) lfirst(temp);

		sequential_scan_list = lcons(create_seqscan_path(rel), NIL);

		rel_index_scan_list = find_index_paths(root,
											   rel,
											   find_relation_indices(root, rel),
											   rel->restrictinfo,
											   rel->joininfo);

		or_index_scan_list = create_or_index_paths(root, rel, rel->restrictinfo);

		rel->pathlist = add_pathlist(rel,
									 sequential_scan_list,
									 append(rel_index_scan_list,
											or_index_scan_list));

		/*
		 * The unordered path is always the last in the list. If it is not
		 * the cheapest path, prune it.
		 */
		lastpath = rel->pathlist;
		while (lnext(lastpath) != NIL)
			lastpath = lnext(lastpath);
		set_cheapest(rel, rel->pathlist);

		/*
		 * if there is a qualification of sequential scan the selec. value
		 * is not set -- so set it explicitly -- Sunita
		 */
		set_rest_selec(root, rel->restrictinfo);
		rel->size = compute_rel_size(rel);
		rel->width = compute_rel_width(rel);
	}
	return;
}

/*
 * find-join-paths--
 *	  Find all possible joinpaths for a query by successively finding ways
 *	  to join single relations into join relations.
 *
 *	  if BushyPlanFlag is set, bushy tree plans will be generated:
 *	  Find all possible joinpaths(bushy trees) for a query by systematically
 *	  finding ways to join relations(both original and derived) together.
 *
 * 'outer-rels' is the current list of relations for which join paths
 *				are to be found, i.e., he current list of relations that
 *				have already been derived.
 * 'levels-needed' is the number of iterations needed
 *
 * Returns the final level of join relations, i.e., the relation that is
 * the result of joining all the original relations together.
 */
static List *
find_join_paths(Query *root, List *outer_rels, int levels_needed)
{
	List	   *x;
	List	   *new_rels = NIL;
	RelOptInfo *rel;

	/*******************************************
	 * genetic query optimizer entry point	   *
	 *	  <utesch@aut.tu-freiberg.de>		   *
	 *******************************************/
	{
		List	   *temp;
		int			paths_to_consider = 0;

		foreach(temp, outer_rels)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(temp);
			paths_to_consider += length(rel->pathlist);
		}

		if ((_use_geqo_) && paths_to_consider >= _use_geqo_rels_)
			/* returns _one_ RelOptInfo, so lcons it */
			return lcons(geqo(root), NIL);	
	}
	
	/*******************************************
	 * rest will be deprecated in case of GEQO *
	 *******************************************/

	while (--levels_needed)
	{
		/*
		 * Determine all possible pairs of relations to be joined at this
		 * level. Determine paths for joining these relation pairs and
		 * modify 'new-rels' accordingly, then eliminate redundant join
		 * relations.
		 */
		new_rels = find_join_rels(root, outer_rels);

		find_all_join_paths(root, new_rels);

		prune_joinrels(new_rels);

#if 0
		/*
		 * * for each expensive predicate in each path in each distinct
		 * rel, * consider doing pullup  -- JMH
		 */
		if (XfuncMode != XFUNC_NOPULL && XfuncMode != XFUNC_OFF)
			foreach(x, new_rels)
				xfunc_trypullup((RelOptInfo *) lfirst(x));
#endif

		rels_set_cheapest(new_rels);

		if (BushyPlanFlag)
		{

			/*
			 * In case of bushy trees if there is still a join between a
			 * join relation and another relation, add a new joininfo that
			 * involves the join relation to the joininfo list of the
			 * other relation
			 */
			add_new_joininfos(root, new_rels, outer_rels);
		}

		foreach(x, new_rels)
		{
			rel = (RelOptInfo *) lfirst(x);
			if (rel->size <= 0)
				rel->size = compute_rel_size(rel);
			rel->width = compute_rel_width(rel);

			/* #define OPTIMIZER_DEBUG */
#ifdef OPTIMIZER_DEBUG
			printf("levels left: %d\n", levels_needed);
			debug_print_rel(root, rel);
#endif
		}

		if (BushyPlanFlag)
		{
			/*
			 * prune rels that have been completely incorporated into new
			 * join rels
			 */
			outer_rels = prune_oldrels(outer_rels);

			/*
			 * merge join rels if then contain the same list of base rels
			 */
			outer_rels = merge_joinrels(new_rels, outer_rels);
			root->join_rel_list = outer_rels;
		}
		else
			root->join_rel_list = new_rels;
		if (!BushyPlanFlag)
			outer_rels = new_rels;
	}

	if (BushyPlanFlag)
		return final_join_rels(outer_rels);
	else
		return new_rels;
}

/*****************************************************************************
 *
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG
static void
print_joinclauses(Query *root, List *clauses)
{
	List	   *l;
	extern void print_expr(Node *expr, List *rtable);	/* in print.c */

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->rtable);
		if (lnext(l))
			printf(" ");
	}
}

static void
print_path(Query *root, Path *path, int indent)
{
	char	   *ptype = NULL;
	JoinPath   *jp;
	bool		join = false;
	int			i;

	for (i = 0; i < indent; i++)
		printf("\t");

	switch (nodeTag(path))
	{
		case T_Path:
			ptype = "SeqScan";
			join = false;
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			join = false;
			break;
		case T_JoinPath:
			ptype = "Nestloop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		default:
			break;
	}
	if (join)
	{
		int			size = path->parent->size;

		jp = (JoinPath *) path;
		printf("%s size=%d cost=%f\n", ptype, size, path->path_cost);
		switch (nodeTag(path))
		{
			case T_MergePath:
			case T_HashPath:
				for (i = 0; i < indent + 1; i++)
					printf("\t");
				printf("   clauses=(");
				print_joinclauses(root, ((JoinPath *) path)->pathinfo);
				printf(")\n");

				if (nodeTag(path) == T_MergePath)
				{
					MergePath  *mp = (MergePath *) path;

					if (mp->outersortkeys || mp->innersortkeys)
					{
						for (i = 0; i < indent + 1; i++)
							printf("\t");
						printf("   sortouter=%d sortinner=%d\n",
							   ((mp->outersortkeys) ? 1 : 0),
							   ((mp->innersortkeys) ? 1 : 0));
					}
				}
				break;
			default:
				break;
		}
		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}
	else
	{
		int			size = path->parent->size;
		int			relid = lfirsti(path->parent->relids);

		printf("%s(%d) size=%d cost=%f",
			   ptype, relid, size, path->path_cost);

		if (nodeTag(path) == T_IndexPath)
		{
			List	   *k,
					   *l;

			printf(" pathkeys=");
			foreach(k, path->pathkeys)
			{
				printf("(");
				foreach(l, lfirst(k))
				{
					Var		   *var = lfirst(l);

					printf("%d.%d", var->varnoold, var->varoattno);
					if (lnext(l))
						printf(", ");
				}
				printf(")");
				if (lnext(k))
					printf(", ");
			}
		}
		printf("\n");
	}
}

static void
debug_print_rel(Query *root, RelOptInfo *rel)
{
	List	   *l;

	printf("(");
	foreach(l, rel->relids)
		printf("%d ", lfirsti(l));
	printf("): size=%d width=%d\n", rel->size, rel->width);

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	printf("\tcheapest path:\n");
	print_path(root, rel->cheapestpath, 1);
}

#endif	 /* OPTIMIZER_DEBUG */
