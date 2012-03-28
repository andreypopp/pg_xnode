/*
 * Copyright (C) 2012, Antonin Houska
 */

#include "xpath.h"
#include "xml_search.h"

static void checkPredicates(XPath locationPath);

PG_FUNCTION_INFO_V1(xmlbranch_in);

/*
 * TODO
 * Design rather a concise storage. XPath is an overkill for XML branch.
 */

/*
 * The branch is considered a simplified location path.
 * Thus we can use the standard XPath parser but the result has to be checked.
 *
 * General rule that limits complexity of the branch is that it must always be possible to compare any 2 branches
 * and say one is lower/greater than the other or they are equal.
 */

/*
 * TODO
 * Reuse code of xpath_in() in more efficient way than copy & paste.
 */

/*
 * TODO
 * rename 'xpath' -> 'xbranch' where appropriate
 */

Datum
xmlbranch_in(PG_FUNCTION_ARGS)
{
	pg_enc		dbEnc = GetDatabaseEncoding();
	char	   *xpathStr;
	XPathHeader xpathHdr;
	char	   *result;
	unsigned short resSize;
	XPathOffset outPos;
	XPath		paths[XPATH_MAX_SUBPATHS];
	unsigned short pathCount = 0;
	XPathExpression expr;
	XPathParserStateData state;
	XPathExprOperand opnd;
	XPath		locPath = NULL;
	XPathOffset hdrOff;

	if (dbEnc != PG_UTF8)
	{
		elog(ERROR, "The current version of xpath requires database encoding to be UTF-8.");
	}
	xpathStr = PG_GETARG_CSTRING(0);
	expr = (XPathExpression) palloc(XPATH_EXPR_BUFFER_SIZE);

	state.c = xpathStr;
	state.cWidth = 0;
	state.pos = 0;
	state.output = state.result = (char *) palloc(XPATH_EXPR_BUFFER_SIZE);

	outPos = sizeof(XPathExpressionData) + XPATH_EXPR_VAR_MAX * sizeof(XPathOffset);
	parseXPathExpression(expr, &state, XPATH_TERM_NULL, NULL, (char *) expr, &outPos, false, false, paths, &pathCount, true);
	resSize = VARHDRSZ + expr->size;

	if (expr->members != 1)
	{
		elog(ERROR, "'%s' is not a valid xml branch: xml branch must contain exactly one operand", xpathStr);
	}

	expr->mainExprAbs = true;

	if (pathCount != 1)
	{
		elog(ERROR, "'%s' is not a valid xml branch: xml branch must contain exactly 1 location path.", xpathStr);
	}

	opnd = (XPathExprOperand) ((char *) expr + sizeof(XPathExpressionData) + sizeof(XPathOffset));
	if (opnd->type != XPATH_OPERAND_PATH)
	{
		elog(ERROR, "'%s' is not a valid xml branch: xml branch may only contain (absolute) location path.", xpathStr);
	}

	locPath = paths[0];

	if (locPath->relative)
	{
		elog(ERROR, "'%s' is not a valid xml branch: xml branch must not be a relative location path.", xpathStr);
	}
	if (locPath->descendants)
	{
		elog(ERROR, "'%s' is not a valid xml branch: xml branch must not use descendant axe.", xpathStr);
	}

	checkPredicates(locPath);

	resSize += sizeof(XPathHeaderData) + locPath->size;

	result = (char *) palloc(resSize);
	outPos = VARHDRSZ;
	memcpy(result + outPos, expr, expr->size);
	outPos += expr->size;
	hdrOff = outPos;
	xpathHdr = (XPathHeader) (result + hdrOff);
	xpathHdr->pathCount = 1;
	outPos += sizeof(XPathHeaderData);
	memcpy(result + outPos, locPath, locPath->size);
	xpathHdr->paths[0] = outPos - hdrOff;
	pfree(locPath);

	pfree(expr);
	SET_VARSIZE(result, resSize);
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(xmlbranch_out);

Datum
xmlbranch_out(PG_FUNCTION_ARGS)
{
	XPathExpression expr = (XPathExpression) VARDATA(PG_GETARG_POINTER(0));

	/*
	 * At this moment we don't know whether the xpath header is there. If
	 * there's none, the 'dump...()' functions simply don't try to access it.
	 */
	XPathHeader xpHdr = (XPathHeader) ((char *) expr + expr->size);
	StringInfoData output;

	initStringInfo(&output);
	dumpXPathExpression(expr, xpHdr, &output, true, false);
	PG_RETURN_POINTER(output.data);
}

/*
 * Make sure all predicates are 'simple enough' to be comparable with other predicates.
 */
static void
checkPredicates(XPath locationPath)
{
	unsigned short i,
				last;

	if (locationPath->depth == 0)
	{
		return;
	}
	last = (locationPath->targNdKind == XMLNODE_ELEMENT) ? locationPath->depth : locationPath->depth - 1;
	for (i = 1; i <= last; i++)
	{
		XPathElement el = (XPathElement) ((char *) locationPath + locationPath->elements[i - 1]);

		if (el->hasPredicate)
		{
			XPathExpression predExpr = (XPathExpression) ((char *) el +
								sizeof(XPathElementData) + strlen(el->name));

			/* XPath parser should not allow other values. */
			Assert(predExpr->type == XPATH_OPERAND_EXPR_TOP);
			Assert(predExpr->members > 0);

			/*
			 * So far, only the following forms are allowed: [<operand>
			 * <binary operator> <operand>] or [<operand>]
			 */
			if (predExpr->members <= 2)
			{
				XPathExprOperand operand = (XPathExprOperand) ((char *) predExpr + sizeof(XPathExpressionData)
								+ predExpr->variables * sizeof(XPathOffset));

				if (operand->type != XPATH_OPERAND_ATTRIBUTE && operand->type != XPATH_OPERAND_LITERAL)
				{
					elog(ERROR, "predicate expression inappropriate for xml branch.");
				}

				if (predExpr->members == 2)
				{
					XPathExprOperatorIdStore *opIdPtr = (XPathExprOperatorIdStore *) ((char *) operand + operand->size);

					operand = (XPathExprOperand) ((char *) opIdPtr + sizeof(XPathExprOperatorIdStore));
					if (operand->type != XPATH_OPERAND_ATTRIBUTE && operand->type != XPATH_OPERAND_LITERAL)
					{
						elog(ERROR, "predicate expression inappropriate for xml branch.");
					}
				}
			}
			else
			{
				elog(ERROR, "predicate expression can't be used in xml branch");
			}
		}
	}
}
