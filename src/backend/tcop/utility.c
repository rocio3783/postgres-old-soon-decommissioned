/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_shadow.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/copy.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/lockcmds.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "access/xlog.h"

/*
 * Error-checking support for DROP commands
 */

struct kindstrings
{
	char		kind;
	char	   *indef_article;
	char	   *name;
	char	   *command;
};

static struct kindstrings kindstringarray[] = {
	{RELKIND_RELATION, "a", "table", "TABLE"},
	{RELKIND_SEQUENCE, "a", "sequence", "SEQUENCE"},
	{RELKIND_VIEW, "a", "view", "VIEW"},
	{RELKIND_INDEX, "an", "index", "INDEX"},
	{RELKIND_COMPOSITE_TYPE, "a", "type", "TYPE"},
	{'\0', "a", "???", "???"}
};


static void
DropErrorMsg(char *relname, char wrongkind, char rightkind)
{
	struct kindstrings *rentry;
	struct kindstrings *wentry;

	for (rentry = kindstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = kindstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind could be something we don't have in our table... */
	if (wentry->kind != '\0')
		elog(ERROR, "\"%s\" is not %s %s. Use DROP %s to remove %s %s",
			 relname, rentry->indef_article, rentry->name,
			 wentry->command, wentry->indef_article, wentry->name);
	else
		elog(ERROR, "\"%s\" is not %s %s",
			 relname, rentry->indef_article, rentry->name);
}

static void
CheckDropPermissions(RangeVar *rel, char rightkind)
{
	struct kindstrings *rentry;
	Oid			relOid;
	HeapTuple	tuple;
	Form_pg_class classform;

	for (rentry = kindstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	relOid = RangeVarGetRelid(rel, true);
	if (!OidIsValid(relOid))
		elog(ERROR, "%s \"%s\" does not exist", rentry->name, rel->relname);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "%s \"%s\" does not exist", rentry->name, rel->relname);

	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != rightkind)
		DropErrorMsg(rel->relname, classform->relkind, rightkind);

	/* Allow DROP to either table owner or schema owner */
	if (!pg_class_ownercheck(relOid, GetUserId()) &&
		!pg_namespace_ownercheck(classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, rel->relname);

	if (!allowSystemTableMods && IsSystemClass(classform))
		elog(ERROR, "%s \"%s\" is a system %s",
			 rentry->name, rel->relname, rentry->name);

	ReleaseSysCache(tuple);
}

static void
CheckOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	relOid = RangeVarGetRelid(rel, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Relation \"%s\" does not exist", rel->relname);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, rel->relname);

	if (noCatalogs)
	{
		if (!allowSystemTableMods &&
			IsSystemClass((Form_pg_class) GETSTRUCT(tuple)))
			elog(ERROR, "relation \"%s\" is a system catalog",
				 rel->relname);
	}

	ReleaseSysCache(tuple);
}


static void
check_xact_readonly(Node *parsetree)
{
	if (!XactReadOnly)
		return;

	/*
	 * Note: Commands that need to do more complicated checking are
	 * handled elsewhere.
	 */

	switch (nodeTag(parsetree))
	{
		case T_AlterDatabaseSetStmt:
		case T_AlterDomainStmt:
		case T_AlterGroupStmt:
		case T_AlterSeqStmt:
		case T_AlterTableStmt:
		case T_RenameStmt:
		case T_AlterUserStmt:
		case T_AlterUserSetStmt:
		case T_CommentStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreatedbStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_CreateGroupStmt:
		case T_IndexStmt:
		case T_CreatePLangStmt:
		case T_CreateOpClassStmt:
		case T_RuleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStmt:
		case T_CreateTrigStmt:
		case T_CompositeTypeStmt:
		case T_CreateUserStmt:
		case T_ViewStmt:
		case T_RemoveAggrStmt:
		case T_DropCastStmt:
		case T_DropStmt:
		case T_DropdbStmt:
		case T_RemoveFuncStmt:
		case T_DropGroupStmt:
		case T_DropPLangStmt:
		case T_RemoveOperStmt:
		case T_RemoveOpClassStmt:
		case T_DropPropertyStmt:
		case T_DropUserStmt:
		case T_GrantStmt:
		case T_TruncateStmt:
			elog(ERROR, "transaction is read-only");
			break;
		default:
			/*nothing*/;
	}
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag is only set nonempty if we want to return a nondefault
 * status (currently, only used for MOVE/FETCH).
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   CommandDest dest,
			   char *completionTag)
{
	check_xact_readonly(parsetree);

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						BeginTransactionBlock();
						break;

						/*
						 * START TRANSACTION, as defined by SQL99:
						 * Identical to BEGIN, except that it takes a few
						 * additional options.
						 */
					case TRANS_STMT_START:
						{
							BeginTransactionBlock();

							if (stmt->options)
							{
								List *head;

								foreach(head, stmt->options)
								{
									DefElem *item = (DefElem *) lfirst(head);

									if (strcmp(item->defname, "transaction_isolation")==0)
										SetPGVariable("transaction_isolation",
													  makeList1(item->arg), false);
									else if (strcmp(item->defname, "transaction_read_only")==0)
										SetPGVariable("transaction_read_only",
													  makeList1(item->arg), false);
								}
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						EndTransactionBlock();
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock();
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 */
		case T_DeclareCursorStmt:
			PerformCursorOpen((DeclareCursorStmt *) parsetree, dest);
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest,
							   completionTag);
			break;

			/*
			 * relation and attribute manipulation
			 */
		case T_CreateSchemaStmt:
			CreateSchemaCommand((CreateSchemaStmt *) parsetree);
			break;

		case T_CreateStmt:
			{
				Oid			relOid;

				relOid = DefineRelation((CreateStmt *) parsetree,
										RELKIND_RELATION);

				/*
				 * Let AlterTableCreateToastTable decide if this one needs
				 * a secondary relation too.
				 */
				CommandCounterIncrement();
				AlterTableCreateToastTable(relOid, true);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;
				List	   *arg;

				foreach(arg, stmt->objects)
				{
					List	   *names = (List *) lfirst(arg);
					RangeVar   *rel;

					switch (stmt->removeType)
					{
						case DROP_TABLE:
							rel = makeRangeVarFromNameList(names);
							CheckDropPermissions(rel, RELKIND_RELATION);
							RemoveRelation(rel, stmt->behavior);
							break;

						case DROP_SEQUENCE:
							rel = makeRangeVarFromNameList(names);
							CheckDropPermissions(rel, RELKIND_SEQUENCE);
							RemoveRelation(rel, stmt->behavior);
							break;

						case DROP_VIEW:
							rel = makeRangeVarFromNameList(names);
							CheckDropPermissions(rel, RELKIND_VIEW);
							RemoveView(rel, stmt->behavior);
							break;

						case DROP_INDEX:
							rel = makeRangeVarFromNameList(names);
							CheckDropPermissions(rel, RELKIND_INDEX);
							RemoveIndex(rel, stmt->behavior);
							break;

						case DROP_TYPE:
							/* RemoveType does its own permissions checks */
							RemoveType(names, stmt->behavior);
							break;

						case DROP_DOMAIN:

							/*
							 * RemoveDomain does its own permissions
							 * checks
							 */
							RemoveDomain(names, stmt->behavior);
							break;

						case DROP_CONVERSION:
							DropConversionCommand(names, stmt->behavior);
							break;

						case DROP_SCHEMA:

							/*
							 * RemoveSchema does its own permissions
							 * checks
							 */
							RemoveSchema(names, stmt->behavior);
							break;
					}

					/*
					 * We used to need to do CommandCounterIncrement()
					 * here, but now it's done inside performDeletion().
					 */
				}
			}
			break;

		case T_TruncateStmt:
			{
				TruncateStmt *stmt = (TruncateStmt *) parsetree;

				TruncateRelation(stmt->relation);
			}
			break;

		case T_CommentStmt:
			CommentObject((CommentStmt *) parsetree);
			break;

		case T_CopyStmt:
			DoCopy((CopyStmt *) parsetree);
			break;

		case T_PrepareStmt:
			PrepareQuery((PrepareStmt *) parsetree);
			break;

		case T_ExecuteStmt:
			ExecuteQuery((ExecuteStmt *) parsetree, dest);
			break;

		case T_DeallocateStmt:
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

			/*
			 * schema
			 */
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;
				Oid			relid;

				CheckOwnership(stmt->relation, true);

				relid = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->renameType)
				{
					case RENAME_TABLE:
						{
							/*
							 * RENAME TABLE requires that we (still) hold
							 * CREATE rights on the containing namespace,
							 * as well as ownership of the table.
							 */
							Oid			namespaceId = get_rel_namespace(relid);
							AclResult	aclresult;

							aclresult = pg_namespace_aclcheck(namespaceId,
															  GetUserId(),
															  ACL_CREATE);
							if (aclresult != ACLCHECK_OK)
								aclcheck_error(aclresult,
										get_namespace_name(namespaceId));

							renamerel(relid, stmt->newname);
							break;
						}
					case RENAME_COLUMN:
						renameatt(relid,
								  stmt->oldname,		/* old att name */
								  stmt->newname,		/* new att name */
							  interpretInhOption(stmt->relation->inhOpt),		/* recursive? */
								  false);		/* recursing already? */
						break;
					case RENAME_TRIGGER:
						renametrig(relid,
								   stmt->oldname,		/* old att name */
								   stmt->newname);		/* new att name */
						break;
					case RENAME_RULE:
						elog(ERROR, "ProcessUtility: Invalid type for RENAME: %d",
							 stmt->renameType);
						break;
					default:
						elog(ERROR, "ProcessUtility: Invalid type for RENAME: %d",
							 stmt->renameType);
				}
			}
			break;

			/* various Alter Table forms */

		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
				Oid			relid;

				relid = RangeVarGetRelid(stmt->relation, false);

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'A':	/* ADD COLUMN */

						/*
						 * Recursively add column to table and, if
						 * requested, to descendants
						 */
						AlterTableAddColumn(relid,
							  interpretInhOption(stmt->relation->inhOpt),
											(ColumnDef *) stmt->def);
						break;
					case 'T':	/* ALTER COLUMN DEFAULT */

						/*
						 * Recursively alter column default for table and,
						 * if requested, for descendants
						 */
						AlterTableAlterColumnDefault(relid,
							  interpretInhOption(stmt->relation->inhOpt),
													 stmt->name,
													 stmt->def);
						break;
					case 'N':	/* ALTER COLUMN DROP NOT NULL */
						AlterTableAlterColumnDropNotNull(relid,
							  interpretInhOption(stmt->relation->inhOpt),
														 stmt->name);
						break;
					case 'n':	/* ALTER COLUMN SET NOT NULL */
						AlterTableAlterColumnSetNotNull(relid,
							  interpretInhOption(stmt->relation->inhOpt),
														stmt->name);
						break;
					case 'S':	/* ALTER COLUMN STATISTICS */
					case 'M':	/* ALTER COLUMN STORAGE */

						/*
						 * Recursively alter column statistics for table
						 * and, if requested, for descendants
						 */
						AlterTableAlterColumnFlags(relid,
							  interpretInhOption(stmt->relation->inhOpt),
												   stmt->name,
												   stmt->def,
												   &(stmt->subtype));
						break;
					case 'D':	/* DROP COLUMN */

						/*
						 * Recursively drop column from table and, if
						 * requested, from descendants
						 */
						AlterTableDropColumn(relid,
							  interpretInhOption(stmt->relation->inhOpt),
											 false,
											 stmt->name,
											 stmt->behavior);
						break;
					case 'C':	/* ADD CONSTRAINT */

						/*
						 * Recursively add constraint to table and, if
						 * requested, to descendants
						 */
						AlterTableAddConstraint(relid,
							  interpretInhOption(stmt->relation->inhOpt),
												(List *) stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */

						/*
						 * Recursively drop constraint from table and, if
						 * requested, from descendants
						 */
						AlterTableDropConstraint(relid,
							  interpretInhOption(stmt->relation->inhOpt),
												 stmt->name,
												 stmt->behavior);
						break;
					case 'E':	/* CREATE TOAST TABLE */
						AlterTableCreateToastTable(relid, false);
						break;
					case 'U':	/* ALTER OWNER */
						/* check that we are the superuser */
						if (!superuser())
							elog(ERROR, "ALTER TABLE: permission denied");
						/* get_usesysid raises an error if no such user */
						AlterTableOwner(relid,
										get_usesysid(stmt->name));
						break;
					case 'L':	/* CLUSTER ON */
						AlterTableClusterOn(relid, stmt->name);
						break;
					case 'o': /* ADD OIDS */
						AlterTableAlterOids(relid,
						 interpretInhOption(stmt->relation->inhOpt),
											false);
						break;
					default:	/* oops */
						elog(ERROR, "ProcessUtility: Invalid type for AlterTableStmt: %d",
							 stmt->subtype);
						break;
				}
			}
			break;

		case T_AlterDomainStmt:
			{
				AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'T':	/* ALTER COLUMN DEFAULT */

						/*
						 * Recursively alter column default for table and,
						 * if requested, for descendants
						 */
						AlterDomainDefault(stmt->typename,
										   stmt->def);
						break;
					case 'N':	/* ALTER COLUMN DROP NOT NULL */
						AlterDomainNotNull(stmt->typename,
										   false);
						break;
					case 'O':	/* ALTER COLUMN SET NOT NULL */
						AlterDomainNotNull(stmt->typename,
										   true);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterDomainAddConstraint(stmt->typename,
												 stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterDomainDropConstraint(stmt->typename,
												  stmt->name,
												  stmt->behavior);
						break;
					case 'U':	/* OWNER TO */
						/* check that we are the superuser */
						if (!superuser())
							elog(ERROR, "ALTER DOMAIN: permission denied");
						/* get_usesysid raises an error if no such user */
						AlterTypeOwner(stmt->typename,
									   get_usesysid(stmt->name));
						break;
					default:	/* oops */
						elog(ERROR, "ProcessUtility: Invalid type for AlterDomainStmt: %d",
							 stmt->subtype);
						break;
				}
			}
			break;


		case T_GrantStmt:
			ExecuteGrantStmt((GrantStmt *) parsetree);
			break;

			/*
			 * ******************************** object creation /
			 * destruction ********************************
			 *
			 */
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;

				switch (stmt->kind)
				{
					case DEFINE_STMT_AGGREGATE:
						DefineAggregate(stmt->defnames, stmt->definition);
						break;
					case DEFINE_STMT_OPERATOR:
						DefineOperator(stmt->defnames, stmt->definition);
						break;
					case DEFINE_STMT_TYPE:
						DefineType(stmt->defnames, stmt->definition);
						break;
				}
			}
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
			{
				CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

				DefineCompositeType(stmt->typevar, stmt->coldeflist);
			}
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			{
				ViewStmt   *stmt = (ViewStmt *) parsetree;

				DefineView(stmt->view, stmt->query, stmt->replace);
			}
			break;

		case T_CreateFunctionStmt:		/* CREATE FUNCTION */
			CreateFunction((CreateFunctionStmt *) parsetree);
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				CheckOwnership(stmt->relation, true);

				DefineIndex(stmt->relation,		/* relation */
							stmt->idxname,		/* index name */
							stmt->accessMethod, /* am name */
							stmt->indexParams,	/* parameters */
							stmt->unique,
							stmt->primary,
							stmt->isconstraint,
							(Expr *) stmt->whereClause,
							stmt->rangetable);
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			DefineQueryRewrite((RuleStmt *) parsetree);
			break;

		case T_CreateSeqStmt:
			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_AlterSeqStmt:
			AlterSequence((AlterSeqStmt *) parsetree);
			break;

		case T_RemoveAggrStmt:
			RemoveAggregate((RemoveAggrStmt *) parsetree);
			break;

		case T_RemoveFuncStmt:
			RemoveFunction((RemoveFuncStmt *) parsetree);
			break;

		case T_RemoveOperStmt:
			RemoveOperator((RemoveOperStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			createdb((CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseSetStmt:
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

				dropdb(stmt->dbname);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				Async_Notify(stmt->relation->relname);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				Async_Listen(stmt->relation->relname, MyProcPid);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				Async_Unlisten(stmt->relation->relname, MyProcPid);
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				load_file(stmt->filename);
			}
			break;

		case T_ClusterStmt:
			cluster((ClusterStmt *) parsetree);
			break;

		case T_VacuumStmt:
			vacuum((VacuumStmt *) parsetree);
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, dest);
			break;

		case T_VariableSetStmt:
			{
				VariableSetStmt *n = (VariableSetStmt *) parsetree;

				/*
				 * Special cases for special SQL syntax that
				 * effectively sets more than one variable per
				 * statement.
				 */
				if (strcmp(n->name, "TRANSACTION")==0)
				{
					List *head;

					foreach(head, n->args)
					{
						DefElem *item = (DefElem *) lfirst(head);

						if (strcmp(item->defname, "transaction_isolation")==0)
							SetPGVariable("transaction_isolation",
										  makeList1(item->arg), n->is_local);
						else if (strcmp(item->defname, "transaction_read_only")==0)
							SetPGVariable("transaction_read_only",
										  makeList1(item->arg), n->is_local);
					}
				}
				else if (strcmp(n->name, "SESSION CHARACTERISTICS")==0)
				{
					List *head;

					foreach(head, n->args)
					{
						DefElem *item = (DefElem *) lfirst(head);

						if (strcmp(item->defname, "transaction_isolation")==0)
							SetPGVariable("default_transaction_isolation",
										  makeList1(item->arg), n->is_local);
						else if (strcmp(item->defname, "transaction_read_only")==0)
							SetPGVariable("default_transaction_read_only",
										  makeList1(item->arg), n->is_local);
					}
				}
				else
					SetPGVariable(n->name, n->args, n->is_local);
			}
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name);
			}
			break;

		case T_VariableResetStmt:
			{
				VariableResetStmt *n = (VariableResetStmt *) parsetree;

				ResetPGVariable(n->name);
			}
			break;

		case T_CreateTrigStmt:
			CreateTrigger((CreateTrigStmt *) parsetree, false);
			break;

		case T_DropPropertyStmt:
			{
				DropPropertyStmt *stmt = (DropPropertyStmt *) parsetree;
				Oid			relId;

				relId = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->removeType)
				{
					case DROP_RULE:
						/* RemoveRewriteRule checks permissions */
						RemoveRewriteRule(relId, stmt->property,
										  stmt->behavior);
						break;
					case DROP_TRIGGER:
						/* DropTrigger checks permissions */
						DropTrigger(relId, stmt->property,
									stmt->behavior);
						break;
				}
			}
			break;

		case T_CreatePLangStmt:
			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

		case T_DropPLangStmt:
			DropProceduralLanguage((DropPLangStmt *) parsetree);
			break;

			/*
			 * ******************************** DOMAIN statements ****
			 */
		case T_CreateDomainStmt:
			DefineDomain((CreateDomainStmt *) parsetree);
			break;

			/*
			 * ******************************** USER statements ****
			 */
		case T_CreateUserStmt:
			CreateUser((CreateUserStmt *) parsetree);
			break;

		case T_AlterUserStmt:
			AlterUser((AlterUserStmt *) parsetree);
			break;

		case T_AlterUserSetStmt:
			AlterUserSet((AlterUserSetStmt *) parsetree);
			break;

		case T_DropUserStmt:
			DropUser((DropUserStmt *) parsetree);
			break;

		case T_LockStmt:
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			DeferredTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CreateGroupStmt:
			CreateGroup((CreateGroupStmt *) parsetree);
			break;

		case T_AlterGroupStmt:
			AlterGroup((AlterGroupStmt *) parsetree, "ALTER GROUP");
			break;

		case T_DropGroupStmt:
			DropGroup((DropGroupStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!superuser())
				elog(ERROR, "CHECKPOINT: permission denied");
			CreateCheckPoint(false, false);
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				switch (stmt->kind)
				{
					case REINDEX_INDEX:
						CheckOwnership(stmt->relation, false);
						ReindexIndex(stmt->relation, stmt->force);
						break;
					case REINDEX_TABLE:
						CheckOwnership(stmt->relation, false);
						ReindexTable(stmt->relation, stmt->force);
						break;
					case REINDEX_DATABASE:
						ReindexDatabase(stmt->name, stmt->force, false);
						break;
				}
				break;
			}
			break;

		case T_CreateConversionStmt:
			CreateConversionCommand((CreateConversionStmt *) parsetree);
			break;

		case T_CreateCastStmt:
			CreateCast((CreateCastStmt *) parsetree);
			break;

		case T_DropCastStmt:
			DropCast((DropCastStmt *) parsetree);
			break;

		case T_CreateOpClassStmt:
			DefineOpClass((CreateOpClassStmt *) parsetree);
			break;

		case T_RemoveOpClassStmt:
			RemoveOpClass((RemoveOpClassStmt *) parsetree);
			break;

		default:
			elog(ERROR, "ProcessUtility: command #%d unsupported",
				 nodeTag(parsetree));
			break;
	}
}


/*
 * CreateCommandTag
 *		utility to get a string representation of the
 *		command operation, given a raw (un-analyzed) parsetree.
 *
 * This must handle all raw command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * NB: all result strings must be shorter than COMPLETION_TAG_BUFSIZE.
 * Also, the result must point at a true constant (permanent storage).
 */
const char *
CreateCommandTag(Node *parsetree)
{
	const char *tag;

	switch (nodeTag(parsetree))
	{
		case T_InsertStmt:
			tag = "INSERT";
			break;

		case T_DeleteStmt:
			tag = "DELETE";
			break;

		case T_UpdateStmt:
			tag = "UPDATE";
			break;

		case T_SelectStmt:
			tag = "SELECT";
			break;

		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = "BEGIN";
						break;

					case TRANS_STMT_START:
						tag = "START TRANSACTION";
						break;

					case TRANS_STMT_COMMIT:
						tag = "COMMIT";
						break;

					case TRANS_STMT_ROLLBACK:
						tag = "ROLLBACK";
						break;

					default:
						tag = "???";
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = "DECLARE CURSOR";
			break;

		case T_ClosePortalStmt:
			tag = "CLOSE CURSOR";
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? "MOVE" : "FETCH";
			}
			break;

		case T_CreateDomainStmt:
			tag = "CREATE DOMAIN";
			break;

		case T_CreateSchemaStmt:
			tag = "CREATE SCHEMA";
			break;

		case T_CreateStmt:
			tag = "CREATE TABLE";
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case DROP_TABLE:
					tag = "DROP TABLE";
					break;
				case DROP_SEQUENCE:
					tag = "DROP SEQUENCE";
					break;
				case DROP_VIEW:
					tag = "DROP VIEW";
					break;
				case DROP_INDEX:
					tag = "DROP INDEX";
					break;
				case DROP_TYPE:
					tag = "DROP TYPE";
					break;
				case DROP_DOMAIN:
					tag = "DROP DOMAIN";
					break;
				case DROP_CONVERSION:
					tag = "DROP CONVERSION";
					break;
				case DROP_SCHEMA:
					tag = "DROP SCHEMA";
					break;
				default:
					tag = "???";
			}
			break;

		case T_TruncateStmt:
			tag = "TRUNCATE TABLE";
			break;

		case T_CommentStmt:
			tag = "COMMENT";
			break;

		case T_CopyStmt:
			tag = "COPY";
			break;

		case T_RenameStmt:
			if (((RenameStmt *) parsetree)->renameType == RENAME_TRIGGER)
				tag = "ALTER TRIGGER";
			else
				tag = "ALTER TABLE";
			break;

		case T_AlterTableStmt:
			tag = "ALTER TABLE";
			break;

		case T_AlterDomainStmt:
			tag = "ALTER DOMAIN";
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT" : "REVOKE";
			}
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case DEFINE_STMT_AGGREGATE:
					tag = "CREATE AGGREGATE";
					break;
				case DEFINE_STMT_OPERATOR:
					tag = "CREATE OPERATOR";
					break;
				case DEFINE_STMT_TYPE:
					tag = "CREATE TYPE";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CompositeTypeStmt:
			tag = "CREATE TYPE";
			break;

		case T_ViewStmt:
			tag = "CREATE VIEW";
			break;

		case T_CreateFunctionStmt:
			tag = "CREATE FUNCTION";
			break;

		case T_IndexStmt:
			tag = "CREATE INDEX";
			break;

		case T_RuleStmt:
			tag = "CREATE RULE";
			break;

		case T_CreateSeqStmt:
			tag = "CREATE SEQUENCE";
			break;

		case T_AlterSeqStmt:
			tag = "ALTER SEQUENCE";
			break;

		case T_RemoveAggrStmt:
			tag = "DROP AGGREGATE";
			break;

		case T_RemoveFuncStmt:
			tag = "DROP FUNCTION";
			break;

		case T_RemoveOperStmt:
			tag = "DROP OPERATOR";
			break;

		case T_CreatedbStmt:
			tag = "CREATE DATABASE";
			break;

		case T_AlterDatabaseSetStmt:
			tag = "ALTER DATABASE";
			break;

		case T_DropdbStmt:
			tag = "DROP DATABASE";
			break;

		case T_NotifyStmt:
			tag = "NOTIFY";
			break;

		case T_ListenStmt:
			tag = "LISTEN";
			break;

		case T_UnlistenStmt:
			tag = "UNLISTEN";
			break;

		case T_LoadStmt:
			tag = "LOAD";
			break;

		case T_ClusterStmt:
			tag = "CLUSTER";
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->vacuum)
				tag = "VACUUM";
			else
				tag = "ANALYZE";
			break;

		case T_ExplainStmt:
			tag = "EXPLAIN";
			break;

		case T_VariableSetStmt:
			tag = "SET";
			break;

		case T_VariableShowStmt:
			tag = "SHOW";
			break;

		case T_VariableResetStmt:
			tag = "RESET";
			break;

		case T_CreateTrigStmt:
			tag = "CREATE TRIGGER";
			break;

		case T_DropPropertyStmt:
			switch (((DropPropertyStmt *) parsetree)->removeType)
			{
				case DROP_TRIGGER:
					tag = "DROP TRIGGER";
					break;
				case DROP_RULE:
					tag = "DROP RULE";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreatePLangStmt:
			tag = "CREATE LANGUAGE";
			break;

		case T_DropPLangStmt:
			tag = "DROP LANGUAGE";
			break;

		case T_CreateUserStmt:
			tag = "CREATE USER";
			break;

		case T_AlterUserStmt:
			tag = "ALTER USER";
			break;

		case T_AlterUserSetStmt:
			tag = "ALTER USER";
			break;

		case T_DropUserStmt:
			tag = "DROP USER";
			break;

		case T_LockStmt:
			tag = "LOCK TABLE";
			break;

		case T_ConstraintsSetStmt:
			tag = "SET CONSTRAINTS";
			break;

		case T_CreateGroupStmt:
			tag = "CREATE GROUP";
			break;

		case T_AlterGroupStmt:
			tag = "ALTER GROUP";
			break;

		case T_DropGroupStmt:
			tag = "DROP GROUP";
			break;

		case T_CheckPointStmt:
			tag = "CHECKPOINT";
			break;

		case T_ReindexStmt:
			tag = "REINDEX";
			break;

		case T_CreateConversionStmt:
			tag = "CREATE CONVERSION";
			break;

		case T_CreateCastStmt:
			tag = "CREATE CAST";
			break;

		case T_DropCastStmt:
			tag = "DROP CAST";
			break;

		case T_CreateOpClassStmt:
			tag = "CREATE OPERATOR CLASS";
			break;

		case T_RemoveOpClassStmt:
			tag = "DROP OPERATOR CLASS";
			break;

		case T_PrepareStmt:
			tag = "PREPARE";
			break;

		case T_ExecuteStmt:
			tag = "EXECUTE";
			break;

		case T_DeallocateStmt:
			tag = "DEALLOCATE";
			break;

		default:
			elog(LOG, "CreateCommandTag: unknown parse node type %d",
				 nodeTag(parsetree));
			tag = "???";
			break;
	}

	return tag;
}
