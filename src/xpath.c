#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/syscache.h"

#include "xmlnode.h"
#include "xpath.h"
#include "xml_parser.h"
#include "xmlnode_util.h"

static ArrayType *getResultArray(XMLScanContext ctx, XMLNodeOffset baseNodeOff);
static xpathval getXPathExprValue(xmldoc document, bool *notNull, XPathExprOperandValue res);


/* The order must follow XPathValueType */
char	   *xpathValueTypes[] = {"bool", "number", "string", "nodeset"};

/*
 * Check here quickly which cast is (not) allowed instead of calling the
 * appropriate castTo... function. This matrix is useful when no particular
 * value needs to be cast.
 *
 * Row represents source type, column the target type. The order always
 * follows XPathValueType.
 */
bool		xpathCasts[4][4] = {
	{true, false, false, false},
	{true, true, false, false},
	{true, true, true, false},
	{true, false, true, true}
};

PG_FUNCTION_INFO_V1(xpath_in);

Datum
xpath_in(PG_FUNCTION_ARGS)
{
	pg_enc		dbEnc = GetDatabaseEncoding();
	char	   *xpathStr;
	XPathHeader xpathHdr;
	char	   *result;
	unsigned short i,
				resSize;
	XPathOffset outPos;
	XPath		paths[XPATH_MAX_SUBPATHS];
	unsigned short pathCount = 0;
	XPathExpression expr;
	XPathParserStateData state;
	XPathOffset *varOffPtr;

	if (dbEnc != PG_UTF8)
	{
		elog(ERROR, "The current version of xpath requires database encoding to be UTF-8.");
	}
	xpathStr = PG_GETARG_CSTRING(0);
	expr = (XPathExpression) palloc(XPATH_EXPR_BUFFER_SIZE);

	state.c = xpathStr;

	/*
	 * With other expression types (sub-expressions or function arg. list),
	 * it's assumed that we're at '(' or '[' when calling the
	 * parseXPathExpression() function. In this specific case we fake such
	 * state by setting the char width to 0 so that the first call of
	 * 'nextChar()' function doesn't move the current character.
	 */
	state.cWidth = 0;
	state.pos = 0;
	state.output = state.result = (char *) palloc(XPATH_EXPR_BUFFER_SIZE);

	outPos = sizeof(XPathExpressionData) + XPATH_EXPR_VAR_MAX * sizeof(XPathOffset);
	parseXPathExpression(expr, &state, '\0', NULL, (char *) expr, &outPos, false, false, paths, &pathCount, true);

	/*
	 * Check if the path may be used as a base path.
	 */
	expr->mainExprAbs = true;
	varOffPtr = (XPathOffset *) ((char *) expr + sizeof(XPathExpressionData));
	for (i = 0; i < expr->variables; i++)
	{
		XPathExprOperand opnd = (XPathExprOperand) ((char *) expr + *varOffPtr);

		if (opnd->type == XPATH_OPERAND_PATH)
		{
			XPath		path = paths[opnd->value.v.path];

			if (path->relative)
			{
				expr->mainExprAbs = false;
				break;
			}
		}
		else if (opnd->type == XPATH_OPERAND_ATTRIBUTE)
		{
			expr->mainExprAbs = false;
			break;
		}
		varOffPtr++;
	}

	resSize = VARHDRSZ + expr->size;
	if (pathCount > 0)
	{
		unsigned short i;

		resSize += sizeof(XPathHeaderData);
		for (i = 0; i < pathCount; i++)
		{
			XPath		path = paths[i];

			/*
			 * We need to store the path itself as well as its offset
			 * (reference)
			 */
			resSize += path->size;

			/*
			 * The first offset (i == 0) will be saved inside the
			 * XPathHeaderData structure
			 */
			if (i > 0)
			{
				resSize += sizeof(XPathOffset);
			}
		}
	}
	result = (char *) palloc(resSize);
	outPos = VARHDRSZ;
	memcpy(result + outPos, expr, expr->size);

	if (pathCount > 0)
	{
		/* Save the paths */
		unsigned short i;
		XPathOffset hdrOff;

		outPos += expr->size;
		hdrOff = outPos;
		xpathHdr = (XPathHeader) (result + hdrOff);
		xpathHdr->pathCount = pathCount;

		/*
		 * Leave space for path references. Again, (pathCount - 1) means that
		 * the array starts inside XPathHeaderData.
		 */
		outPos += sizeof(XPathHeaderData) + (pathCount - 1) * sizeof(XPathOffset);

		for (i = 0; i < pathCount; i++)
		{
			XPath		path = paths[i];

			memcpy(result + outPos, path, path->size);
			xpathHdr->paths[i] = outPos - hdrOff;
			outPos += path->size;
			pfree(path);
		}
	}
	pfree(expr);
	SET_VARSIZE(result, resSize);
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(xpath_out);

Datum
xpath_out(PG_FUNCTION_ARGS)
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

PG_FUNCTION_INFO_V1(xpath_debug_print);

Datum
xpath_debug_print(PG_FUNCTION_ARGS)
{
	XPathExpression expr = (XPathExpression) VARDATA(PG_GETARG_POINTER(0));

	/*
	 * At this moment we don't know whether the xpath header is there. If
	 * there's none, the 'dump...()' functions simply don't try to access it.
	 */
	XPathHeader xpHdr = (XPathHeader) ((char *) expr + expr->size);
	StringInfoData output;

	initStringInfo(&output);
	dumpXPathExpression(expr, xpHdr, &output, true, true);

	if (expr->npaths > 0)
	{

		unsigned short i;

		appendStringInfoChar(&output, '\n');
		for (i = 0; i < xpHdr->pathCount; i++)
		{
			appendStringInfo(&output, "\n\n");
			dumpLocationPath(xpHdr, &output, true, i);
			appendStringInfoChar(&output, '\n');
		}
	}
	PG_RETURN_POINTER(output.data);
}

XPath
getSingleXPath(XPathExpression expr, XPathHeader xpHdr)
{
	XPathExprOperand operand;
	XPath		path;

	if (expr->members != 1 || expr->npaths == 0)
	{
		elog(ERROR, "xpath expression can't be used as a base path");
	}

	/*
	 * A valid path may be enclosed in round brackets
	 */
	operand = (XPathExprOperand) ((char *) expr + sizeof(XPathExpressionData) +
								  expr->variables * sizeof(XPathOffset));
	while (operand->type == XPATH_OPERAND_EXPR_SUB)
	{
		expr = (XPathExpression) operand;
		operand = (XPathExprOperand) ((char *) expr + sizeof(XPathExpressionData));
	}

	if (operand->type != XPATH_OPERAND_PATH || expr->members != 1)
	{
		elog(ERROR, "xpath expression can't be used as a base path");
	}
	path = XPATH_HDR_GET_PATH(xpHdr, operand->value.v.path);

	if (path->relative)
	{
		elog(ERROR, "base XPath must be absolute");
	}
	return path;
}


PG_FUNCTION_INFO_V1(xpath_single);

Datum
xpath_single(PG_FUNCTION_ARGS)
{
	xpath		xpathIn = (xpath) PG_GETARG_POINTER(0);
	XPathExpression expr = (XPathExpression) VARDATA(xpathIn);
	XPathExpression exprCopy;
	XPathHeader xpHdr = (XPathHeader) ((char *) expr + expr->size);
	xmldoc		doc = (xmldoc) PG_GETARG_VARLENA_P(1);
	bool		notNull;
	xpathval	result;
	XPathExprOperandValueData resData;

	if (!expr->mainExprAbs)
	{
		elog(ERROR, "neither relative paths nor attributes expected in main expression");
	}
	exprCopy = prepareXPathExpression(expr, (XMLCompNodeHdr) XNODE_ROOT(doc), doc, xpHdr, NULL);
	evaluateXPathExpression(exprCopy, NULL, (XMLCompNodeHdr) XNODE_ROOT(doc), 0, &resData);
	result = getXPathExprValue(doc, &notNull, &resData);

	if (resData.type == XPATH_VAL_NODESET)
	{
		if (notNull)
		{
			PG_RETURN_POINTER(result);
		}
		else
		{
			PG_RETURN_NULL();
		}
	}
	else
	{
		PG_RETURN_POINTER(result);
	}
}


PG_FUNCTION_INFO_V1(xpath_array);

/*
 * TODO Check if it's safe to store pointers to elements of
 * PG_GETARG_POINTER(2) across calls or if the parameter should be copied to
 * the 'fctx';
 */
Datum
xpath_array(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	XMLScan		baseScan;
	XMLNodeHdr	baseNode;
	XMLScanContext xScanCtx;

	if (SRF_IS_FIRSTCALL())
	{
		int		   *dimv;
		int			colIndex;
		bool		isNull;
		Oid			elTypOid,
					arrTypeOid;
		HeapTuple	typeTup;
		Form_pg_type typeStruct;
		int2		elTypLen,
					arrTypLen;
		bool		elByVal;
		char		elTypAlign;
		XMLNodeKind baseTarget;
		Oid			resultType;

		xpath		xpathBasePtr = (xpath) PG_GETARG_POINTER(0);
		XPath		xpathBase;
		XPathExpression exprBase = (XPathExpression) VARDATA(xpathBasePtr);
		XPathHeader xpHdrBase = (XPathHeader) ((char *) exprBase + exprBase->size);

		ArrayType  *pathsColArr = (ArrayType *) PG_GETARG_POINTER(1);
		xmldoc		doc = (xmldoc) PG_GETARG_VARLENA_P(2);
		XMLCompNodeHdr docRoot = (XMLCompNodeHdr) XNODE_ROOT(doc);
		MemoryContext oldcontext;

		xpathBase = getSingleXPath(exprBase, xpHdrBase);

		fctx = SRF_FIRSTCALL_INIT();
		if (get_call_result_type(fcinfo, &resultType, NULL) != TYPEFUNC_SCALAR)
		{
			elog(ERROR, "function called in a context that doesn't accept scalar.");
		}
		oldcontext = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		if (ARR_NDIM(pathsColArr) != 1)
		{
			elog(ERROR, " 1-dimensional array must be used to pass column xpaths");
		}
		dimv = ARR_DIMS(pathsColArr);
		if (*dimv > XMLNODE_SET_MAX_COLS)
		{
			elog(ERROR, "maximum number of xpath columns is %u", XMLNODE_SET_MAX_COLS);
		}

		/*
		 * Retrieve parameters for 'array_ref()'
		 */
		elTypOid = ARR_ELEMTYPE(pathsColArr);
		typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elTypOid));
		Assert(HeapTupleIsValid(typeTup));
		typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
		elTypLen = typeStruct->typlen;
		elByVal = typeStruct->typbyval;
		elTypAlign = typeStruct->typalign;
		arrTypeOid = typeStruct->typarray;
		ReleaseSysCache(typeTup);

		typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrTypeOid));
		Assert(HeapTupleIsValid(typeTup));
		typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
		arrTypLen = typeStruct->typlen;
		ReleaseSysCache(typeTup);

		xScanCtx = (XMLScanContext) palloc(sizeof(XMLScanContextData));
		xScanCtx->baseScan = (XMLScan) palloc(sizeof(XMLScanData));

		initXMLScan(xScanCtx->baseScan, NULL, xpathBase, xpHdrBase, docRoot, doc, xpathBase->descendants > 1);
		baseTarget = xScanCtx->baseScan->xpath->targNdKind;
		if (baseTarget != XMLNODE_DOC && baseTarget != XMLNODE_ELEMENT)
		{
			elog(ERROR, "base path must point to element or document");
		}

		/*
		 * For columns, the paths are only checked and stored to the context.
		 * Initialization of the 'column scans' is deferred to the point where
		 * the base scan really finds something.
		 */
		xScanCtx->columns = *dimv;
		xScanCtx->colPaths = (xpath *) palloc(xScanCtx->columns * sizeof(xpath));
		xScanCtx->colResults = NULL;
		xScanCtx->colResNulls = NULL;
		xScanCtx->outArrayType = resultType;

		/*
		 * Output element OID to be identified later - first time a not null
		 * value is found for any xpath.
		 */
		xScanCtx->outElmType = InvalidOid;
		for (colIndex = 1; colIndex <= *dimv; colIndex++)
		{
			Datum		colPathD = array_ref(pathsColArr, 1, &colIndex, arrTypLen, elTypLen, elByVal, elTypAlign, &isNull);
			xpath		colPathPtr;

			if (isNull)
			{
				elog(ERROR, "column XPath must not be null");
			}
			colPathPtr = (xpath) DatumGetPointer(colPathD);
			xScanCtx->colPaths[colIndex - 1] = colPathPtr;
		}
		fctx->user_fctx = xScanCtx;
		MemoryContextSwitchTo(oldcontext);
	}
	fctx = SRF_PERCALL_SETUP();
	xScanCtx = (XMLScanContext) fctx->user_fctx;
	baseScan = xScanCtx->baseScan;

	if (baseScan->xpath->targNdKind == XMLNODE_DOC && !baseScan->done)
	{
		ArrayType  *result = getResultArray(xScanCtx, XNODE_ROOT_OFFSET(xScanCtx->baseScan->document));

		baseScan->done = true;

		if (result != NULL)
		{
			SRF_RETURN_NEXT(fctx, PointerGetDatum(result));
		}
	}
	if (!baseScan->done)
	{
		baseNode = getNextXMLNode(baseScan, false);
		if (baseNode != NULL)
		{
			XMLNodeOffset baseNdOff = (char *) baseNode - VARDATA(xScanCtx->baseScan->document);
			ArrayType  *result = getResultArray(xScanCtx, baseNdOff);

			if (result != NULL)
			{
				SRF_RETURN_NEXT(fctx, PointerGetDatum(result));
			}
			else
			{
				baseScan->done = true;
			}
		}
		else
		{
			baseScan->done = true;
		}
	}
	if (baseScan->done)
	{
		finalizeXMLScan(baseScan);
		pfree(baseScan);

		pfree(xScanCtx->colPaths);
		if (xScanCtx->colResults != NULL)
		{
			pfree(xScanCtx->colResults);
		}
		if (xScanCtx->colResNulls != NULL)
		{
			pfree(xScanCtx->colResNulls);
		}
		pfree(xScanCtx);
		SRF_RETURN_DONE(fctx);
	}
	SRF_RETURN_DONE(fctx);
}

void
xpathValCastToBool(XPathExprOperandValue valueSrc, XPathExprOperandValue valueDst)
{
	valueDst->type = XPATH_VAL_BOOLEAN;
	switch (valueSrc->type)
	{
		case XPATH_VAL_BOOLEAN:
			valueDst->v.boolean = valueSrc->v.boolean;
			break;

		case XPATH_VAL_NUMBER:
			valueDst->v.boolean = (valueSrc->v.num != 0.0);
			break;

		case XPATH_VAL_STRING:
			valueDst->v.boolean = (!valueSrc->isNull && (strlen(valueSrc->v.string.str) > 0));
			break;

		case XPATH_VAL_NODESET:
			if (valueSrc->v.nodeSet.isDocument)
			{
				/* Document is always a non-empty node set. */
				valueDst->v.boolean = true;
			}
			else
			{
				valueDst->v.boolean = (valueSrc->v.nodeSet.count > 0);
			}
			break;

		default:
			elog(ERROR, "unable to cast type %u to boolean", valueSrc->type);
			break;
	}
}

void
xpathValCastToStr(XPathExprOperandValue valueSrc, XPathExprOperandValue valueDst)
{
	valueDst->type = XPATH_VAL_STRING;
	valueDst->isNull = valueSrc->isNull;

	switch (valueSrc->type)
	{
		case XPATH_VAL_STRING:

			if (!valueSrc->isNull)
			{
				if (valueSrc->v.string.mustFree)
				{
					char	   *src = valueSrc->v.string.str;
					unsigned int len = sizeof(src) + 1;
					char	   *dst = (char *) palloc(len);

					memcpy(dst, src, len);
					valueDst->v.string.str = dst;
				}
				else
				{
					valueDst->v.string.str = valueSrc->v.string.str;
				}

				valueDst->v.string.mustFree = valueSrc->v.string.mustFree;
				return;
			}
			break;

		case XPATH_VAL_NODESET:
			if (valueSrc->v.nodeSet.isDocument)
			{
				elog(ERROR, "document can't be cast to string");
			}
			if (!valueSrc->isNull)
			{
				XPathNodeSet ns = &valueSrc->v.nodeSet;

				/*
				 * If the node-set contains multiple nodes,only the first on
				 * is used (http://www.w3.org/TR/1999/REC-xpath-19991116/#sect
				 * ion-String-Functions)
				 */
				XMLNodeHdr	node = ns->count == 1 ? ns->nodes.single : ns->nodes.array[0];

				if (node->kind == XMLNODE_ELEMENT)
				{
					valueDst->v.string.str = getElementNodeStr((XMLCompNodeHdr) node);
					valueDst->v.string.mustFree = true;
				}
				else
				{
					valueDst->v.string.str = getNonElementNodeStr(node);
					valueDst->v.string.mustFree = false;
				}
			}
			break;


		default:
			elog(ERROR, "unable to cast type %u to string", valueSrc->type);
			break;
	}
}

/*
 * Neither null value nor NaN strings expected.
 */
void
xpathValCastToNum(XPathExprOperandValue valueSrc, XPathExprOperandValue valueDst)
{
	valueDst->type = XPATH_VAL_NUMBER;
	switch (valueSrc->type)
	{
		case XPATH_VAL_NUMBER:
			valueDst->v.num = valueSrc->v.num;
			break;

		case XPATH_VAL_STRING:
			Assert(operand->common.castToNumber);
			valueDst->v.num = xnodeGetNumValue(valueSrc->v.string.str);
			break;

		default:
			elog(ERROR, "unable to cast type %u to number", valueSrc->type);
			break;
	}
}

/*
 * XPathExprOperandValue is used so that we can access 'isNull; attribute.
 * However, XPATH_VAL_STRING is the only value type accepted.
 */
void
xpathStrFree(XPathExprOperandValue strValue)
{
	if (!strValue->isNull)
	{
		Assert(strValue->type == XPATH_VAL_STRING);
		if (strValue->v.string.mustFree)
		{
			pfree(strValue->v.string.str);
		}
	}
}


/*
 * Returns array of nodes where i-th element is result of scan starting at
 * 'baseNodeOff', using 'ctx->colPaths[i]' as XPath expression.
 */
static ArrayType *
getResultArray(XMLScanContext ctx, XMLNodeOffset baseNodeOff)
{
	bool		notNull = false;
	unsigned short int i;
	ArrayType  *result = NULL;

	if (ctx->colResults == NULL)
	{
		ctx->colResults = (Datum *) palloc(ctx->columns * sizeof(Datum));
	}
	if (ctx->colResNulls == NULL)
	{
		ctx->colResNulls = (bool *) palloc(ctx->columns * sizeof(bool));
	}
	for (i = 0; i < ctx->columns; i++)
	{
		xpathval	colValue = NULL;
		xmldoc		doc = ctx->baseScan->document;
		xpath		xpathPtr = ctx->colPaths[i];
		XPathExpression expr = (XPathExpression) VARDATA(xpathPtr);
		XPathHeader xpHdr = (XPathHeader) ((char *) expr + expr->size);
		bool		colNotNull = false;
		XMLCompNodeHdr baseNode = (XMLCompNodeHdr) ((char *) VARDATA(doc) + baseNodeOff);
		XPathExpression exprCopy = prepareXPathExpression(expr, baseNode, doc, xpHdr, ctx->baseScan);
		XPathExprOperandValueData resData;

		evaluateXPathExpression(exprCopy, XMLSCAN_CURRENT_LEVEL(ctx->baseScan),
		(XMLCompNodeHdr) ((char *) VARDATA(doc) + baseNodeOff), 0, &resData);
		colValue = getXPathExprValue(doc, &colNotNull, &resData);
		ctx->colResults[i] = PointerGetDatum(colValue);
		ctx->colResNulls[i] = !colNotNull;
		notNull = (notNull || colNotNull);
	}

	if (notNull)
	{
		int			dims[1];
		int			lbs[1];

		if (ctx->outElmType == InvalidOid)
		{
			Form_pg_type typeStruct;
			HeapTuple	typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(ctx->outArrayType));

			Assert(HeapTupleIsValid(typeTup));
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
			ctx->outElmType = typeStruct->typelem;
			ReleaseSysCache(typeTup);

			typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(ctx->outElmType));
			Assert(HeapTupleIsValid(typeTup));
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
			ctx->outElmLen = typeStruct->typlen;
			ctx->outElmByVal = typeStruct->typbyval;
			ctx->outElmalign = typeStruct->typalign;
			ReleaseSysCache(typeTup);
		}
		dims[0] = ctx->columns;
		lbs[0] = 1;
		result = construct_md_array(ctx->colResults, ctx->colResNulls, 1, dims, lbs, ctx->outElmType, ctx->outElmLen,
									ctx->outElmByVal, ctx->outElmalign);
	}
	return result;
}

static xpathval
getXPathExprValue(xmldoc document, bool *notNull, XPathExprOperandValue res)
{
	xpathval	retValue = NULL;
	XPathValue	xpval = NULL;
	unsigned int resSize = 0;
	char	   *output = NULL;

	*notNull = false;

	if (res->isNull)
	{
		return retValue;
	}
	if (res->type == XPATH_VAL_NODESET)
	{
		if (res->v.nodeSet.isDocument)
		{
			/*
			 * Convert document to a node fragment. Make sure that XML
			 * declaration is removed.
			 */
			XMLNodeHdr	node;
			XMLNodeOffset rootOffOrig;
			XMLCompNodeHdr rootOrig;
			unsigned int sizeOrig,
						sizeNew;
			char	   *output,
					   *targ;

			Assert(xpath->targNdKind == XMLNODE_DOC);

			rootOffOrig = XNODE_ROOT_OFFSET(document);
			rootOrig = (XMLCompNodeHdr) XNODE_ROOT(document);
			sizeOrig = VARSIZE(document);
			sizeNew = (rootOrig->common.flags & XNODE_DOC_XMLDECL) ? sizeOrig - sizeof(XMLDeclData) : sizeOrig;
			sizeNew += sizeof(XPathValueData);
			output = (char *) palloc(sizeNew);

			xpval = (XPathValue) VARDATA(output);
			xpval->type = XPATH_VAL_NODESET;
			xpval->v.nodeSetRoot = rootOffOrig + sizeof(XPathValueData);

			targ = (char *) xpval + sizeof(XPathValueData);
			memcpy(targ, VARDATA(document), sizeNew - (VARHDRSZ + sizeof(XPathValueData)));
			SET_VARSIZE(output, sizeNew);
			retValue = (xpathval) output;

			node = (XMLNodeHdr) ((char *) VARDATA(output) + rootOffOrig + sizeof(XPathValueData));
			node->kind = XMLNODE_DOC_FRAGMENT;
			*notNull = true;
		}
		else
		{
			unsigned int j = res->v.nodeSet.count;
			XMLNodeHdr	firstNode = NULL;

			if (j > 0)
			{
				char	   *outTmp;

				*notNull = true;

				if (j == 1)
				{
					unsigned int nodeSize;
					XMLNodeOffset root;

					firstNode = res->v.nodeSet.nodes.single;
					nodeSize = getXMLNodeSize(firstNode, true);
					resSize = VARHDRSZ + sizeof(XPathValueData) + nodeSize;
					output = (char *) palloc(resSize);
					outTmp = VARDATA(output);
					xpval = (XPathValue) outTmp;
					outTmp += sizeof(XPathValueData);
					copyXMLNode(firstNode, outTmp, false, &root);
					xpval->type = XPATH_VAL_NODESET;
					xpval->v.nodeSetRoot = sizeof(XPathValueData) + root;
				}
				else
				{
					/*
					 * Construct a document fragment from the list of nodes
					 */
					unsigned int k;
					unsigned int nodeSizeTotal = 0;
					char		bwidth;
					char	   *refTarget;
					XMLNodeHdr	node;
					unsigned int *nodeSizes;
					XMLCompNodeHdr fragmentHdr;
					XMLNodeOffset targetPos;

					nodeSizes = (unsigned int *) palloc(j * sizeof(unsigned int));
					for (k = 0; k < j; k++)
					{
						if (k == XMLNODE_MAX_CHILDREN)
						{
							elog(ERROR, "Maximum number of %u children exceeded for node document fragment.",
								 XMLNODE_MAX_CHILDREN);
						}
						node = res->v.nodeSet.nodes.array[k];
						nodeSizes[k] = getXMLNodeSize(node, true);
						nodeSizeTotal += nodeSizes[k];
					}

					/*
					 * 'nodeSizeTotal' now equals to the greatest distance
					 * between parent (doc fragment) and its child.
					 */
					bwidth = getXMLNodeOffsetByteWidth(nodeSizeTotal);
					resSize = VARHDRSZ + sizeof(XPathValueData) + nodeSizeTotal + sizeof(XMLCompNodeHdrData) +
						bwidth * j;
					output = (char *) palloc(resSize);
					xpval = (XPathValue) VARDATA(output);
					outTmp = (char *) xpval + sizeof(XPathValueData);
					fragmentHdr = (XMLCompNodeHdr) (outTmp + nodeSizeTotal);
					refTarget = (char *) fragmentHdr + sizeof(XMLCompNodeHdrData);
					targetPos = 0;

					for (k = 0; k < j; k++)
					{
						XMLNodeOffset root,
									dist;
						char	   *ndTarget = outTmp + targetPos;

						node = res->v.nodeSet.nodes.array[k];
						copyXMLNode(node, ndTarget, false, &root);
						dist = nodeSizeTotal - (targetPos + root);
						writeXMLNodeOffset(dist, &refTarget, bwidth, true);
						targetPos += nodeSizes[k];
					}
					pfree(nodeSizes);

					fragmentHdr->common.kind = XMLNODE_DOC_FRAGMENT;
					fragmentHdr->common.flags = 0;
					XNODE_SET_REF_BWIDTH(fragmentHdr, bwidth);
					fragmentHdr->children = j;

					xpval->type = XPATH_VAL_NODESET;
					xpval->v.nodeSetRoot = sizeof(XPathValueData) + nodeSizeTotal;
				}
				SET_VARSIZE(output, resSize);
				retValue = (xpathval) output;
			}
		}
	}
	else
	{
		if (res->type == XPATH_VAL_NUMBER || res->type == XPATH_VAL_BOOLEAN)
		{
			resSize = VARHDRSZ + sizeof(XPathValueData);
			output = (char *) palloc(resSize);
			xpval = (XPathValue) VARDATA(output);
			xpval->type = res->type;
			if (res->type == XPATH_VAL_NUMBER)
			{
				xpval->v.numVal = res->v.num;
			}
			else
			{
				xpval->v.booVal = res->v.boolean;
			}
			*notNull = true;
		}
		else if (res->type == XPATH_VAL_STRING)
		{
			unsigned int len = strlen(res->v.string.str);

			resSize = VARHDRSZ + sizeof(XPathValueData) + len;
			output = (char *) palloc(resSize);
			xpval = (XPathValue) VARDATA(output);
			memcpy(xpval->v.strVal, res->v.string.str, len);
			xpval->v.strVal[len] = '\0';
			xpval->type = res->type;
			xpathStrFree(res);
			*notNull = true;
		}
		else
		{
			elog(ERROR, "unknown value type: %u", res->type);
		}
		SET_VARSIZE(output, resSize);
		retValue = (xpathval) output;
	}
	return retValue;
}

PG_FUNCTION_INFO_V1(xpathval_in);

Datum
xpathval_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "not implemented");
}

PG_FUNCTION_INFO_V1(xpathval_out);

Datum
xpathval_out(PG_FUNCTION_ARGS)
{
	xpathval	xpval = PG_GETARG_VARLENA_P(0);
	XPathValue	xp = (XPathValue) VARDATA(xpval);
	StringInfoData out;

	switch (xp->type)
	{
		case XPATH_VAL_BOOLEAN:
			out.maxlen = 6;		/* 'false' + '\0' */
			out.data = (char *) palloc(out.maxlen);
			resetStringInfo(&out);
			if (xp->v.booVal)
			{
				appendStringInfo(&out, "%s", "true");
			}
			else
			{
				appendStringInfo(&out, "%s", "false");
			}
			PG_RETURN_CSTRING(out.data);
			break;

		case XPATH_VAL_NUMBER:
			out.maxlen = 8;
			out.data = (char *) palloc(out.maxlen);
			resetStringInfo(&out);
			appendStringInfo(&out, "%f", xp->v.numVal);
			PG_RETURN_CSTRING(out.data);
			break;

		case XPATH_VAL_STRING:
			{
				unsigned int len = strlen(xp->v.strVal);

				out.data = (char *) palloc(len + 1);
				memcpy(out.data, xp->v.strVal, len);
				out.data[len] = '\0';
				PG_RETURN_CSTRING(out.data);
			}
			break;
		case XPATH_VAL_NODESET:
			{
				char	   *data = (char *) VARDATA(xpval);
				XMLNodeOffset rootNdOff = xp->v.nodeSetRoot;

				PG_RETURN_CSTRING(dumpXMLNode(data, rootNdOff));
			}
			break;
		default:
			elog(ERROR, "unrecognized type of xpath value: %u", xp->type);
			break;
	}
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(xpathval_to_xmlnode);

Datum
xpathval_to_xmlnode(PG_FUNCTION_ARGS)
{
	xpathval	xpval = PG_GETARG_VARLENA_P(0);
	XPathValue	xp = (XPathValue) VARDATA(xpval);

	if (xp->type == XPATH_VAL_NODESET)
	{
		unsigned int sizeNew = VARSIZE(xpval) - sizeof(XPathValueData);
		unsigned int resSize = sizeNew + sizeof(XMLNodeOffset);
		char	   *result = (char *) palloc(resSize);
		XPathValue	xpv = (XPathValue) VARDATA(xpval);
		char	   *src = (char *) xpv + sizeof(XPathValueData);
		char	   *dst = VARDATA(result);
		XMLNodeOffset *rootOffPtr = (XMLNodeOffset *) (result + sizeNew);

		memcpy(dst, src, sizeNew - VARHDRSZ);
		*rootOffPtr = xp->v.nodeSetRoot - sizeof(XPathValueData);
		SET_VARSIZE(result, resSize);
		PG_RETURN_POINTER(result);
	}
	else
	{
		PG_RETURN_NULL();
	}
}
