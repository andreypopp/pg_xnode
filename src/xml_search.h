/*
 * Copyright (C) 2012, Antonin Houska
 */

#ifndef XML_SEARCH_H_
#define XML_SEARCH_H_

#include "postgres.h"
#include "mb/pg_wchar.h"

typedef struct varlena xmlbranchtype;
typedef xmlbranchtype *xmlbranch;

extern Datum xmlbranch_in(PG_FUNCTION_ARGS);
extern Datum xmlbranch_out(PG_FUNCTION_ARGS);
extern Datum xmldoc_to_branches(PG_FUNCTION_ARGS);

#endif   /* XML_SEARCH_H_ */
