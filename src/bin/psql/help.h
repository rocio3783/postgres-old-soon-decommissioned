/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header$
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(unsigned short int pager);

void		helpSQL(const char *topic, unsigned short int pager);

void		print_copyright(void);

#endif
