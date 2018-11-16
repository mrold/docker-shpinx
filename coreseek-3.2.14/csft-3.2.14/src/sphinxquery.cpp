//
// $Id: sphinxquery.cpp 1851 2009-06-21 23:41:00Z shodan $
//

#include "sphinx.h"
#include "sphinxquery.h"
#include "sphinxutils.h"
#include <stdarg.h>

//////////////////////////////////////////////////////////////////////////
// EXTENDED PARSER RELOADED
//////////////////////////////////////////////////////////////////////////

#include "yysphinxquery.h"

//////////////////////////////////////////////////////////////////////////

class XQParser_t
{
public:
					XQParser_t ();
					~XQParser_t () {}

public:
	bool			Parse ( XQQuery_t & tQuery, const char * sQuery, const ISphTokenizer * pTokenizer, const CSphSchema * pSchema, CSphDict * pDict );

	bool			Error ( const char * sTemplate, ... );
	void			Warning ( const char * sTemplate, ... );

	bool			AddField ( DWORD & uFields, const char * szField, int iLen );
	bool			ParseFields ( DWORD & uFields, int & iMaxFieldPos );

	bool			IsSpecial ( char c );
	int				GetToken ( YYSTYPE * lvalp );

	void			AddQuery ( XQNode_t * pNode );
	XQNode_t *		AddKeyword ( const char * sKeyword );
	XQNode_t *		AddKeyword ( XQNode_t * pLeft, XQNode_t * pRight );
	XQNode_t *		AddOp ( XQOperator_e eOp, XQNode_t * pLeft, XQNode_t * pRight );

	void			Cleanup ();
	XQNode_t *		SweepNulls ( XQNode_t * pNode );
	bool			FixupNots ( XQNode_t * pNode );

public:
	XQQuery_t *				m_pParsed;

	BYTE *					m_sQuery;
	int						m_iQueryLen;
	const char *			m_pLastTokenStart;

	const CSphSchema *		m_pSchema;
	ISphTokenizer *			m_pTokenizer;
	CSphDict *				m_pDict;

	const char *			m_pCur;

	CSphVector<XQNode_t*>	m_dSpawned;
	XQNode_t *				m_pRoot;

	bool					m_bStopOnInvalid;
	int						m_iAtomPos;

	int						m_iPendingNulls;
	int						m_iPendingType;
	YYSTYPE					m_tPendingToken;

	bool					m_bEmpty;

	CSphVector<CSphString>	m_dIntTokens;
};

//////////////////////////////////////////////////////////////////////////

int yylex ( YYSTYPE * lvalp, XQParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( XQParser_t * pParser, const char * sMessage )
{
	if ( pParser->m_pParsed->m_sParseError.IsEmpty() )
		pParser->m_pParsed->m_sParseError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#include "yysphinxquery.c"

//////////////////////////////////////////////////////////////////////////

void XQNode_t::SetFieldSpec ( DWORD uMask, int iMaxPos )
{
	// set it, if we do not yet have one
	if ( !m_bFieldSpec )
	{
		m_bFieldSpec = true;
		m_uFieldMask = uMask;
		m_iFieldMaxPos = iMaxPos;
	}

	// some of the children might not yet have a spec, even if the node itself has
	// eg. 'hello @title world' (whole node has '@title' spec but 'hello' node does not have any!)
	ARRAY_FOREACH ( i, m_dChildren )
		m_dChildren[i]->SetFieldSpec ( uMask, iMaxPos );
}

//////////////////////////////////////////////////////////////////////////

XQParser_t::XQParser_t ()
	: m_pParsed ( NULL )
	, m_pLastTokenStart ( NULL )
	, m_pRoot ( NULL )
	, m_bStopOnInvalid ( true )
{
}


/// cleanup spawned nodes (for bailing out on errors)
void XQParser_t::Cleanup ()
{
	ARRAY_FOREACH ( i, m_dSpawned )
	{
		m_dSpawned[i]->m_dChildren.Reset ();
		SafeDelete ( m_dSpawned[i] );
	}
	m_dSpawned.Reset ();
}



bool XQParser_t::Error ( const char * sTemplate, ... )
{
	assert ( m_pParsed );
	char sBuf[256];

	const char * sPrefix = "query error: ";
	int iPrefix = strlen(sPrefix);
	strcpy ( sBuf, sPrefix );

	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( sBuf+iPrefix, sizeof(sBuf)-iPrefix, sTemplate, ap );
	va_end ( ap );

	m_pParsed->m_sParseError = sBuf;
	return false;
}


void XQParser_t::Warning ( const char * sTemplate, ... )
{
	assert ( m_pParsed );
	char sBuf[256];

	const char * sPrefix = "query warning: ";
	int iPrefix = strlen(sPrefix);
	strcpy ( sBuf, sPrefix );

	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( sBuf+iPrefix, sizeof(sBuf)-iPrefix, sTemplate, ap );
	va_end ( ap );

	m_pParsed->m_sParseWarning = sBuf;
}


/// my special chars
bool XQParser_t::IsSpecial ( char c )
{
	return c=='(' || c==')' || c=='|' || c=='-' || c=='!' || c=='@' || c=='~' || c=='"' || c=='/';
}


/// lookup field and add it into mask
bool XQParser_t::AddField ( DWORD & uFields, const char * szField, int iLen )
{
	CSphString sField;
	sField.SetBinary ( szField, iLen );

	int iField = m_pSchema->GetFieldIndex ( sField.cstr () );
	if ( iField < 0 )
	{
		if ( m_bStopOnInvalid )
			return Error ( "no field '%s' found in schema", sField.cstr () );
		else
			Warning ( "no field '%s' found in schema", sField.cstr () );
	}
	else
	{
		if ( iField >= 32 )
			return Error ( " max 32 fields allowed" );

		uFields |= 1 << iField;
	}

	return true;
}


/// parse fields block
bool XQParser_t::ParseFields ( DWORD & uFields, int & iMaxFieldPos )
{
	uFields = 0;
	iMaxFieldPos = 0;

	const char * pPtr = m_pTokenizer->GetBufferPtr ();
	const char * pLastPtr = m_pTokenizer->GetBufferEnd ();

	if ( pPtr==pLastPtr )
		return true; // silently ignore trailing field operator

	bool bNegate = false;
	bool bBlock = false;

	// handle special modifiers
	if ( *pPtr=='!' )
	{
		// handle @! and @!(
		bNegate = true; pPtr++;
		if ( *pPtr=='(' ) { bBlock = true; pPtr++; }

	} else if ( *pPtr=='*' )
	{
		// handle @*
		uFields = 0xFFFFFFFF;
		m_pTokenizer->SetBufferPtr ( pPtr+1 );
		return true;

	} else if ( *pPtr=='(' )
	{
		// handle @(
		bBlock = true; pPtr++;
	}

	// handle invalid chars
	if ( !sphIsAlpha(*pPtr) )
	{
		m_pTokenizer->SetBufferPtr ( pPtr ); // ignore and re-parse (FIXME! maybe warn?)
		return true;
	}
	assert ( sphIsAlpha(*pPtr) ); // i think i'm paranoid

	// handle field specification
	if ( !bBlock )
	{
		// handle standalone field specification
		const char * pFieldStart = pPtr;
		while ( sphIsAlpha(*pPtr) && pPtr<pLastPtr )
			pPtr++;

		assert ( pPtr-pFieldStart>0 );
		if ( !AddField ( uFields, pFieldStart, pPtr-pFieldStart ) )
			return false;

		m_pTokenizer->SetBufferPtr ( pPtr );
		if ( bNegate && uFields )
			uFields = ~uFields;

	} else
	{
		// handle fields block specification
		assert ( sphIsAlpha(*pPtr) && bBlock ); // and complicated

		bool bOK = false;
		const char * pFieldStart = NULL;
		while ( pPtr<pLastPtr )
		{
			// accumulate field name, while we can
			if ( sphIsAlpha(*pPtr) )
			{
				if ( !pFieldStart )
					pFieldStart = pPtr;
				pPtr++;
				continue;
			}

			// separator found
			if ( pFieldStart==NULL )
			{
				return Error ( "separator without preceding field name in field block operator", *pPtr );

			} if ( *pPtr==',' )
			{
				if ( !AddField ( uFields, pFieldStart, pPtr-pFieldStart ) )
					return false;

				pFieldStart = NULL;
				pPtr++;

			} else if ( *pPtr==')' )
			{
				if ( !AddField ( uFields, pFieldStart, pPtr-pFieldStart ) )
					return false;

				m_pTokenizer->SetBufferPtr ( ++pPtr );
				if ( bNegate && uFields )
					uFields = ~uFields;

				bOK = true;
				break;

			} else
			{
				return Error ( "invalid character '%c' in field block operator", *pPtr );
			}
		}
		if ( !bOK )
			return Error ( "missing closing ')' in field block operator" );
	}

	// handle optional position range modifier
	if ( pPtr[0]=='[' && isdigit(pPtr[1]) )
	{
		// skip '[' and digits
		const char * p = pPtr+1;
		while ( *p && isdigit(*p) ) p++;

		// check that the range ends with ']' (FIXME! maybe report an error if it does not?)
		if ( *p!=']' )
			return true;

		// fetch my value
		iMaxFieldPos = strtoul ( pPtr+1, NULL, 10 );
		m_pTokenizer->SetBufferPtr ( p+1 );
	}

	// well done
	return true;
}


/// a lexer of my own
int XQParser_t::GetToken ( YYSTYPE * lvalp )
{
	// what, noone's pending for a bending?!
	if ( !m_iPendingType )
		for ( ;; )
	{
		assert ( m_iPendingNulls==0 );

		// tricky stuff
		// we need to manually check for numbers in certain states (currently, just after proximity or quorum operator)
		// required because if 0-9 are not in charset_table, or min_word_len is too high,
		// the tokenizer will *not* return the number as a token!
		m_pLastTokenStart = m_pTokenizer->GetBufferPtr ();
		const char * sEnd = m_pTokenizer->GetBufferEnd ();

		const char * p = m_pLastTokenStart;
		while ( p<sEnd && isspace(*(BYTE*)p) ) p++; // to avoid CRT assertions on Windows

		const char * sToken = p;
		while ( p<sEnd && isdigit(*(BYTE*)p) ) p++;

		if ( p>sToken && ( *p=='\0' || isspace(*(BYTE*)p) || IsSpecial(*p) ) )
		{
			// got a number followed by a whitespace or special, handle it
			char sNumberBuf[16];

			int iNumberLen = Min ( (int)sizeof(sNumberBuf)-1, int(p-sToken) );
			memcpy ( sNumberBuf, sToken, iNumberLen );
			sNumberBuf[iNumberLen] = '\0';
			m_tPendingToken.tInt.iValue = atoi ( sNumberBuf );

			// check if it can be used as a keyword too
			m_pTokenizer->SetBuffer ( (BYTE*)sNumberBuf, iNumberLen );
			sToken = (const char*) m_pTokenizer->GetToken();
			m_pTokenizer->SetBuffer ( m_sQuery, m_iQueryLen );
			m_pTokenizer->SetBufferPtr ( p );

			m_tPendingToken.tInt.iStrIndex = -1;
			if ( sToken )
			{
				m_dIntTokens.Add ( sToken );
				if ( m_pDict->GetWordID((BYTE*)sToken) )
					m_tPendingToken.tInt.iStrIndex = m_dIntTokens.GetLength()-1;
				else
					m_dIntTokens.Pop();
				m_iAtomPos++;
			}

			m_iPendingNulls = 0;
			m_iPendingType = TOK_INT;
			break;
		}

		// not a number, or not followed by a whitespace, so fallback
		sToken = (const char *) m_pTokenizer->GetToken ();
		if ( !sToken )
			return 0;
		m_bEmpty = false;

		m_iPendingNulls = m_pTokenizer->GetOvershortCount ();
		m_iAtomPos += 1+m_iPendingNulls;

		if ( m_pTokenizer->WasTokenSpecial() )
		{
			// specials must not affect pos
			m_iAtomPos--;

			// some specials are especially special
			if ( sToken[0]=='@' )
			{
				// parse fields operator
				if ( !ParseFields ( m_tPendingToken.tFieldLimit.uMask, m_tPendingToken.tFieldLimit.iMaxPos ) )
					return -1;

				if ( m_pSchema->m_dFields.GetLength()!=32 )
					m_tPendingToken.tFieldLimit.uMask &= ( 1UL<<m_pSchema->m_dFields.GetLength() )-1;

				m_iPendingType = TOK_FIELDLIMIT;
				break;

			} else if ( sToken[0]=='<' )
			{
				if ( *m_pTokenizer->GetBufferPtr()=='<' )
				{
					// got "<<", aka operator BEFORE
					m_iPendingType = TOK_BEFORE;
					break;
				} else
				{
					// got stray '<', ignore
					continue;
				}

			} else
			{
				// all the other specials are passed to parser verbtaim
				m_iPendingType = sToken[0]=='!' ? '-' : sToken[0];
				break;
			}
		}

		// check for stopword, and create that node
		// temp buffer is required, because GetWordID() might expand (!) the keyword in-place
		const int MAX_BYTES = 3*SPH_MAX_WORD_LEN + 16;
		BYTE sTmp [ MAX_BYTES ];

		strncpy ( (char*)sTmp, sToken, MAX_BYTES );
		sTmp[MAX_BYTES-1] = '\0';

		if ( !m_pDict->GetWordID ( sTmp ) )
			sToken = NULL;

		m_tPendingToken.pNode = AddKeyword ( sToken );
		m_iPendingType = TOK_KEYWORD;
		break;
	}

	// someone must be pending now!
	assert ( m_iPendingType );
	m_bEmpty = false;

	// ladies first, though
	if ( m_iPendingNulls>0 )
	{
		m_iPendingNulls--;
		lvalp->pNode = AddKeyword ( NULL );
		return TOK_KEYWORD;
	}

	// pending the offending
	int iRes = m_iPendingType;
	m_iPendingType = 0;

	*lvalp = m_tPendingToken;
	return iRes;
}


void XQParser_t::AddQuery ( XQNode_t * pNode )
{
	m_pRoot = pNode;
}


XQNode_t * XQParser_t::AddKeyword ( const char * sKeyword )
{
	XQKeyword_t tAW ( sKeyword, m_iAtomPos );

	XQNode_t * pNode = new XQNode_t();
	pNode->m_dWords.Add ( tAW );

	m_dSpawned.Add ( pNode );
	return pNode;
}


XQNode_t * XQParser_t::AddKeyword ( XQNode_t * pLeft, XQNode_t * pRight )
{
	if ( !pLeft || !pRight )
		return pLeft ? pLeft : pRight;

	assert ( pLeft->IsPlain() );
	assert ( pRight->IsPlain() );
	assert ( pRight->m_dWords.GetLength()==1 );

	pLeft->m_dWords.Add ( pRight->m_dWords[0] );
	m_dSpawned.RemoveValue ( pRight );
	SafeDelete ( pRight );
	return pLeft;
}


XQNode_t * XQParser_t::AddOp ( XQOperator_e eOp, XQNode_t * pLeft, XQNode_t * pRight )
{
	/////////
	// unary
	/////////

	if ( eOp==SPH_QUERY_NOT )
	{
		XQNode_t * pNode = new XQNode_t();
		pNode->m_dChildren.Add ( pLeft );
		pNode->m_eOp = eOp;
		return pNode;
	}

	//////////
	// binary
	//////////

	if ( !pLeft || !pRight )
		return pLeft ? pLeft : pRight;

	// left spec always tries to infect the nodes to the right, only brackets can stop it
	// eg. '@title hello' vs 'world'
	if ( pLeft->m_bFieldSpec )
		pRight->SetFieldSpec ( pLeft->m_uFieldMask, pLeft->m_iFieldMaxPos );

	// build a new node
	XQNode_t * pResult = NULL;
	if ( !pLeft->IsPlain() && pLeft->m_eOp==eOp )
	{
		pLeft->m_dChildren.Add ( pRight );
		pResult = pLeft;
	} else
	{
		XQNode_t * pNode = new XQNode_t();
		pNode->m_dChildren.Add ( pLeft );
		pNode->m_dChildren.Add ( pRight );
		pNode->m_eOp = eOp;
		pResult = pNode;
	}

	// however, it's right (!) spec which is chosen for the resulting node,
	// eg. '@title hello' + 'world @body program'
	if ( pRight->m_bFieldSpec )
	{
		pResult->m_bFieldSpec = true;
		pResult->m_uFieldMask = pRight->m_uFieldMask;
		pResult->m_iFieldMaxPos = pRight->m_iFieldMaxPos;
	}

	return pResult;
}


XQNode_t * XQParser_t::SweepNulls ( XQNode_t * pNode )
{
	if ( !pNode )
		return NULL;

	// sweep plain node
	if ( pNode->IsPlain() )
	{
		ARRAY_FOREACH ( i, pNode->m_dWords )
			if ( pNode->m_dWords[i].m_sWord.cstr()==NULL )
				pNode->m_dWords.Remove ( i-- );

		if ( pNode->m_dWords.GetLength()==0 )
		{
			m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
			SafeDelete ( pNode );
			return NULL;
		}

		return pNode;
	}

	// sweep op node
	ARRAY_FOREACH ( i, pNode->m_dChildren )
	{
		pNode->m_dChildren[i] = SweepNulls ( pNode->m_dChildren[i] );
		if ( pNode->m_dChildren[i]==NULL )
			pNode->m_dChildren.Remove ( i-- );
	}

	if ( pNode->m_dChildren.GetLength()==0 )
	{
		m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
		SafeDelete ( pNode );
		return NULL;
	}

	// remove redundancies if needed
	if ( pNode->m_eOp!=SPH_QUERY_NOT && pNode->m_dChildren.GetLength()==1 )
	{
		XQNode_t * pRet = pNode->m_dChildren[0];
		pNode->m_dChildren.Reset ();

		m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
		SafeDelete ( pNode );
		return pRet;
	}

	// done
	return pNode;
}


bool XQParser_t::FixupNots ( XQNode_t * pNode )
{
	// no processing for plain nodes
	if ( !pNode || pNode->IsPlain() )
		return true;

	// process 'em children
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		if ( !FixupNots ( pNode->m_dChildren[i] ) )
			return false;

	// extract NOT subnodes
	CSphVector<XQNode_t*> dNots;
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		if ( pNode->m_dChildren[i]->m_eOp==SPH_QUERY_NOT )
	{
		dNots.Add ( pNode->m_dChildren[i] );
		pNode->m_dChildren.RemoveFast ( i-- );
	}

	// no NOTs? we're square
	if ( !dNots.GetLength() )
		return true;

	// nothing but NOTs? we can't compute that
	if ( !pNode->m_dChildren.GetLength() )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (node consists of NOT operators only)" );
		return false;
	}

	// NOT within OR? we can't compute that
	if ( pNode->m_eOp==SPH_QUERY_OR )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (NOT is not allowed within OR)" );
		return false;
	}

	// NOT used in before operator
	if ( pNode->m_eOp==SPH_QUERY_BEFORE )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (NOT cannot be used as before operand)" );
		return false;
	}

	// must be some NOTs within AND at this point, convert this node to ANDNOT
	assert ( pNode->m_eOp==SPH_QUERY_AND && pNode->m_dChildren.GetLength() && dNots.GetLength() );

	XQNode_t * pAnd = new XQNode_t();
	m_dSpawned.Add ( pAnd );
	pAnd->m_eOp = SPH_QUERY_AND;
	pAnd->m_dChildren = pNode->m_dChildren;

	XQNode_t * pNot = NULL;
	if ( dNots.GetLength()==1 )
	{
		pNot = dNots[0];
	} else
	{
		pNot = new XQNode_t();
		m_dSpawned.Add ( pNot );
		pNot->m_eOp = SPH_QUERY_OR;
		pNot->m_dChildren = dNots;
	}

	pNode->m_eOp = SPH_QUERY_ANDNOT;
	pNode->m_dChildren.Reset ();
	pNode->m_dChildren.Add ( pAnd );
	pNode->m_dChildren.Add ( pNot );
	return true;
}


static void DeleteNodesWOFields ( XQNode_t * pNode )
{
	if ( !pNode )
		return;

	for ( int i = 0; i < pNode->m_dChildren.GetLength (); )
	{
		if ( pNode->m_dChildren [i]->m_uFieldMask==0 )
		{
			// this should be a leaf node
			assert ( pNode->m_dChildren [i]->m_dChildren.GetLength () == 0 );
			SafeDelete ( pNode->m_dChildren [i] );
			pNode->m_dChildren.RemoveFast ( i );
		}
		else
		{
			DeleteNodesWOFields ( pNode->m_dChildren [i] );
			i++;
		}
	}
}


bool XQParser_t::Parse ( XQQuery_t & tParsed, const char * sQuery, const ISphTokenizer * pTokenizer, const CSphSchema * pSchema, CSphDict * pDict )
{
	CSphScopedPtr<ISphTokenizer> pMyTokenizer ( pTokenizer->Clone ( true ) );
	pMyTokenizer->AddSpecials ( "()|-!@~\"/^$<" );
	pMyTokenizer->EnableQueryParserMode ( true );

	// check for relaxed syntax
	const char * OPTION_RELAXED = "@@relaxed";
	const int OPTION_RELAXED_LEN = strlen ( OPTION_RELAXED );

	m_bStopOnInvalid = true;
	if ( strncmp ( sQuery, OPTION_RELAXED, OPTION_RELAXED_LEN )==0 && !sphIsAlpha ( sQuery[OPTION_RELAXED_LEN]) )
	{
		sQuery += OPTION_RELAXED_LEN;
		m_bStopOnInvalid = false;
	}

	// setup parser
	m_pParsed = &tParsed;
	m_sQuery = (BYTE*) sQuery;
	m_iQueryLen = strlen(sQuery);
	m_pTokenizer = pMyTokenizer.Ptr();
	m_pSchema = pSchema;
	m_pDict = pDict;
	m_pCur = sQuery;
	m_iAtomPos = 0;
	m_iPendingNulls = 0;
	m_iPendingType = 0;
	m_pRoot = NULL;
	m_bEmpty = true;

	m_pTokenizer->SetBuffer ( m_sQuery, m_iQueryLen );
	int iRes = yyparse ( this );

	if ( iRes && !m_bEmpty )
	{
		Cleanup ();
		return false;
	}

	DeleteNodesWOFields ( m_pRoot );
	m_pRoot = SweepNulls ( m_pRoot );

	if ( !FixupNots ( m_pRoot ) )
	{
		Cleanup ();
		return false;
	}

	if ( m_pRoot && m_pRoot->m_eOp==SPH_QUERY_NOT )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (single NOT operator)" );
		return false;
	}

	m_dSpawned.Reset();
	if ( m_pRoot )
	{
		SafeDelete ( tParsed.m_pRoot );
		tParsed.m_pRoot = m_pRoot;
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////

#define XQDEBUG 0

#if XQDEBUG
static void xqIndent ( int iIndent )
{
	iIndent *= 2;
	while ( iIndent--)
		printf ( " " );
}


static void xqDump ( XQNode_t * pNode, const CSphSchema & tSch, int iIndent )
{
	if ( !pNode->IsPlain() )
	{
		xqIndent ( iIndent );
		switch ( pNode->m_eOp )
		{
			case SPH_QUERY_AND: printf ( "AND:\n" ); break;
			case SPH_QUERY_OR: printf ( "OR:\n" ); break;
			case SPH_QUERY_NOT: printf ( "NOT:\n" ); break;
			case SPH_QUERY_ANDNOT: printf ( "ANDNOT:\n" ); break;
			case SPH_QUERY_BEFORE: printf ( "BEFORE:\n" ); break;
			default: printf ( "unknown-op-%d:\n", pNode->m_eOp ); break;
		}
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			xqDump ( pNode->m_dChildren[i], tSch, iIndent+1 );
	} else
	{
		xqIndent ( iIndent );
		printf ( "MATCH(%d,%d):", pNode->m_uFieldMask, pNode->m_iMaxDistance );

		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			const XQKeyword_t & tWord = pNode->m_dWords[i];

			const char * sLocTag = "";
			if ( tWord.m_bFieldStart ) sLocTag = ", start";
			if ( tWord.m_bFieldEnd ) sLocTag = ", end";

			printf ( " %s (qpos %d%s)", tWord.m_sWord.cstr(), tWord.m_iAtomPos, sLocTag );
		}
		printf ( "\n" );
	}
}
#endif


bool sphParseExtendedQuery ( XQQuery_t & tParsed, const char * sQuery, const ISphTokenizer * pTokenizer, const CSphSchema * pSchema, CSphDict * pDict )
{
	XQParser_t qp;
	bool bRes = qp.Parse ( tParsed, sQuery, pTokenizer, pSchema, pDict );

#if XQDEBUG
	if ( bRes )
	{
		printf ( "--- query ---\n" );
		xqDump ( tParsed.m_pRoot, *pSchema, 0 );
		printf ( "---\n" );
	}
#endif

	return bRes;
}

//
// $Id: sphinxquery.cpp 1851 2009-06-21 23:41:00Z shodan $
//
