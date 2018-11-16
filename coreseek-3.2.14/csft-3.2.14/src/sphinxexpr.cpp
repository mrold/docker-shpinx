//
// $Id: sphinxexpr.cpp 2026 2009-10-22 00:36:37Z shodan $
//

//
// Copyright (c) 2001-2008, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxexpr.h"
#include <time.h>
#include <math.h>

#if !USE_WINDOWS
#include <unistd.h>
#include <sys/time.h>
#endif

//////////////////////////////////////////////////////////////////////////

#ifndef M_LOG2E
#define M_LOG2E    1.44269504088896340736
#endif

#ifndef M_LOG10E
#define M_LOG10E   0.434294481903251827651
#endif

//////////////////////////////////////////////////////////////////////////
// EVALUATION ENGINE
//////////////////////////////////////////////////////////////////////////

struct Expr_GetInt_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetInt_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) tMatch.GetAttr ( m_tLocator ); } // FIXME! OPTIMIZE!!! we can go the short route here
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.GetAttr ( m_tLocator ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.GetAttr ( m_tLocator ); }
};


struct Expr_GetBits_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetBits_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) tMatch.GetAttr ( m_tLocator ); }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.GetAttr ( m_tLocator ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.GetAttr ( m_tLocator ); }
};


struct Expr_GetFloat_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetFloat_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return tMatch.GetAttrFloat ( m_tLocator ); }
};


struct Expr_GetConst_c : public ISphExpr
{
	float m_fValue;
	Expr_GetConst_c ( float fValue ) : m_fValue ( fValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return m_fValue; }
};


struct Expr_GetIntConst_c : public ISphExpr
{
	int m_iValue;
	Expr_GetIntConst_c ( int iValue ) : m_iValue ( iValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	virtual int IntEval ( const CSphMatch & ) const { return m_iValue; }
	virtual int64_t Int64Eval ( const CSphMatch & ) const { return m_iValue; }
};


struct Expr_GetInt64Const_c : public ISphExpr
{
	int64_t m_iValue;
	Expr_GetInt64Const_c ( int64_t iValue ) : m_iValue ( iValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	virtual int IntEval ( const CSphMatch & ) const { assert ( 0 ); return (int)m_iValue; }
	virtual int64_t Int64Eval ( const CSphMatch & ) const { return m_iValue; }
};


struct Expr_GetId_c : public ISphExpr
{
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float)tMatch.m_iDocID; }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.m_iDocID; }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.m_iDocID; }
};


struct Expr_GetWeight_c : public ISphExpr
{
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float)tMatch.m_iWeight; }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.m_iWeight; }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.m_iWeight; }
};

//////////////////////////////////////////////////////////////////////////

struct Expr_Arglist_c : public ISphExpr
{
	CSphVector<ISphExpr *> m_dArgs;

	Expr_Arglist_c ( ISphExpr * pLeft, ISphExpr * pRight )
	{
		AddArgs ( pLeft );
		AddArgs ( pRight );
	}

	~Expr_Arglist_c ()
	{
		ARRAY_FOREACH ( i, m_dArgs )
			SafeRelease ( m_dArgs[i] );
	}

	void AddArgs ( ISphExpr * pExpr )
	{
		// not an arglist? just add it
		if ( !pExpr->IsArglist() )
		{
			m_dArgs.Add ( pExpr );
			return;
		}

		// arglist? take ownership of its args, and dismiss it
		Expr_Arglist_c * pArgs = (Expr_Arglist_c *) pExpr;
		ARRAY_FOREACH ( i, pArgs->m_dArgs )
		{
			m_dArgs.Add ( pArgs->m_dArgs[i] );
			pArgs->m_dArgs[i] = NULL;
		}
		SafeRelease ( pExpr );
	}

	virtual bool IsArglist () const
	{
		return true;
	}

	virtual float Eval ( const CSphMatch & ) const
	{
		assert ( 0 && "internal error: Eval() must not be explicitly called on arglist" );
		return 0.0f;
	}
};

//////////////////////////////////////////////////////////////////////////

#define FIRST	m_pFirst->Eval(tMatch)
#define SECOND	m_pSecond->Eval(tMatch)
#define THIRD	m_pThird->Eval(tMatch)

#define INTFIRST	m_pFirst->IntEval(tMatch)
#define INTSECOND	m_pSecond->IntEval(tMatch)
#define INTTHIRD	m_pThird->IntEval(tMatch)

#define INT64FIRST	m_pFirst->Int64Eval(tMatch)
#define INT64SECOND	m_pSecond->Int64Eval(tMatch)
#define INT64THIRD	m_pThird->Int64Eval(tMatch)

#define DECLARE_UNARY_TRAITS(_classname,_expr) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		_classname ( ISphExpr * pFirst ) : m_pFirst ( pFirst ) {}; \
		~_classname () { SafeRelease ( m_pFirst ); } \
		virtual void SetMVAPool ( const DWORD * pMvaPool ) { m_pFirst->SetMVAPool ( pMvaPool ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \

#define DECLARE_UNARY_FLT(_classname,_expr) \
		DECLARE_UNARY_TRAITS(_classname,_expr) \
	};

#define DECLARE_UNARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_UNARY_TRAITS(_classname,_expr) \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

#define IABS(_arg) ( (_arg)>0 ? (_arg) : (-_arg) )

DECLARE_UNARY_INT ( Expr_Neg_c,		-FIRST,			-INTFIRST,		-INT64FIRST )
DECLARE_UNARY_INT ( Expr_Abs_c,		fabs(FIRST),	IABS(INTFIRST),	IABS(INT64FIRST) )
DECLARE_UNARY_FLT ( Expr_Ceil_c,	float(ceil(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Floor_c,	float(floor(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Sin_c,		float(sin(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Cos_c,		float(cos(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Ln_c,		float(log(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Log2_c,	float(log(FIRST)*M_LOG2E) )
DECLARE_UNARY_FLT ( Expr_Log10_c,	float(log(FIRST)*M_LOG10E) )
DECLARE_UNARY_FLT ( Expr_Exp_c,		float(exp(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Sqrt_c,	float(sqrt(FIRST)) )

DECLARE_UNARY_INT ( Expr_NotInt_c,		float(INTFIRST?0:1),	INTFIRST?0:1,	INTFIRST?0:1 );
DECLARE_UNARY_INT ( Expr_NotInt64_c,	float(INT64FIRST?0:1),	INT64FIRST?0:1,	INT64FIRST?0:1 );

//////////////////////////////////////////////////////////////////////////

#define DECLARE_BINARY_TRAITS(_classname,_expr) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		ISphExpr * m_pSecond; \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond ) : m_pFirst ( pFirst ), m_pSecond ( pSecond ) {} \
		~_classname () { SafeRelease ( m_pFirst ); SafeRelease ( m_pSecond ); } \
		virtual void SetMVAPool ( const DWORD * pMvaPool ) { m_pFirst->SetMVAPool ( pMvaPool ); m_pSecond->SetMVAPool ( pMvaPool ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \

#define DECLARE_BINARY_FLT(_classname,_expr) \
		DECLARE_BINARY_TRAITS(_classname,_expr) \
	};

#define DECLARE_BINARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_BINARY_TRAITS(_classname,_expr) \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

#define DECLARE_BINARY_POLY(_classname,_expr,_expr2,_expr3) \
	DECLARE_BINARY_INT ( _classname##Float_c,	_expr,						(int)Eval(tMatch),		(int64_t)Eval(tMatch ) ) \
	DECLARE_BINARY_INT ( _classname##Int_c,		(float)IntEval(tMatch),		_expr2,					(int64_t)IntEval(tMatch) ) \
	DECLARE_BINARY_INT ( _classname##Int64_c,	(float)Int64Eval(tMatch),	(int)Int64Eval(tMatch),	_expr3 )

#define IFFLT(_expr)	( (_expr) ? 1.0f : 0.0f )
#define IFINT(_expr)	( (_expr) ? 1 : 0 )

DECLARE_BINARY_INT ( Expr_Add_c,	FIRST + SECOND,						INTFIRST + INTSECOND,				INT64FIRST + INT64SECOND )
DECLARE_BINARY_INT ( Expr_Sub_c,	FIRST - SECOND,						INTFIRST - INTSECOND,				INT64FIRST - INT64SECOND )
DECLARE_BINARY_INT ( Expr_Mul_c,	FIRST * SECOND,						INTFIRST * INTSECOND,				INT64FIRST * INT64SECOND )
DECLARE_BINARY_FLT ( Expr_Div_c,	FIRST / SECOND )
DECLARE_BINARY_INT ( Expr_Idiv_c,	(float)(int(FIRST)/int(SECOND)),	INTFIRST / INTSECOND,				INT64FIRST / INT64SECOND )

DECLARE_BINARY_POLY ( Expr_Lt,		IFFLT ( FIRST < SECOND ),			IFINT ( INTFIRST < INTSECOND ),		IFINT ( INT64FIRST < INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Gt,		IFFLT ( FIRST > SECOND ),			IFINT ( INTFIRST > INTSECOND ),		IFINT ( INT64FIRST > INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Lte,		IFFLT ( FIRST <= SECOND ),			IFINT ( INTFIRST <= INTSECOND ),	IFINT ( INT64FIRST <= INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Gte,		IFFLT ( FIRST >= SECOND ),			IFINT ( INTFIRST >= INTSECOND ),	IFINT ( INT64FIRST >= INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Eq,		IFFLT ( fabs(FIRST-SECOND)<=1e-6 ),	IFINT ( INTFIRST == INTSECOND ),	IFINT ( INT64FIRST == INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Ne,		IFFLT ( fabs(FIRST-SECOND)>1e-6 ),	IFINT ( INTFIRST != INTSECOND ),	IFINT ( INT64FIRST != INT64SECOND ) )

DECLARE_BINARY_INT ( Expr_Min_c,	Min(FIRST,SECOND),					Min(INTFIRST,INTSECOND),			Min(INT64FIRST,INT64SECOND) )
DECLARE_BINARY_INT ( Expr_Max_c,	Max(FIRST,SECOND),					Max(INTFIRST,INTSECOND),			Max(INT64FIRST,INT64SECOND) )
DECLARE_BINARY_FLT ( Expr_Pow_c,	float(pow(FIRST,SECOND)) )

DECLARE_BINARY_POLY ( Expr_And,		FIRST!=0.0f && SECOND!=0.0f,		IFINT ( INTFIRST && INTSECOND ),	IFINT ( INT64FIRST && INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Or,		FIRST!=0.0f || SECOND!=0.0f,		IFINT ( INTFIRST || INTSECOND ),	IFINT ( INT64FIRST || INT64SECOND ) )

//////////////////////////////////////////////////////////////////////////

#define DECLARE_TERNARY(_classname,_expr,_expr2,_expr3) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		ISphExpr * m_pSecond; \
		ISphExpr * m_pThird; \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond, ISphExpr * pThird ) : m_pFirst ( pFirst ), m_pSecond ( pSecond ), m_pThird ( pThird ) {} \
		~_classname () { SafeRelease ( m_pFirst ); SafeRelease ( m_pSecond ); SafeRelease ( m_pThird ); } \
		virtual void SetMVAPool ( const DWORD * pMvaPool ) { m_pFirst->SetMVAPool ( pMvaPool ); m_pSecond->SetMVAPool ( pMvaPool ); m_pThird->SetMVAPool ( pMvaPool ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

DECLARE_TERNARY ( Expr_If_c,	( FIRST!=0.0f ) ? SECOND : THIRD,	INTFIRST ? INTSECOND : INTTHIRD,	INT64FIRST ? INT64SECOND : INT64THIRD )
DECLARE_TERNARY ( Expr_Madd_c,	FIRST*SECOND+THIRD,					INTFIRST*INTSECOND + INTTHIRD,		INT64FIRST*INT64SECOND + INT64THIRD )
DECLARE_TERNARY ( Expr_Mul3_c,	FIRST*SECOND*THIRD,					INTFIRST*INTSECOND*INTTHIRD,		INT64FIRST*INT64SECOND*INT64THIRD )

//////////////////////////////////////////////////////////////////////////
// PARSER INTERNALS
//////////////////////////////////////////////////////////////////////////

#include "yysphinxexpr.h"

/// known functions
enum Func_e
{
	FUNC_NOW,

	FUNC_ABS,
	FUNC_CEIL,
	FUNC_FLOOR,
	FUNC_SIN,
	FUNC_COS,
	FUNC_LN,
	FUNC_LOG2,
	FUNC_LOG10,
	FUNC_EXP,
	FUNC_SQRT,
	FUNC_BIGINT,

	FUNC_MIN,
	FUNC_MAX,
	FUNC_POW,
	FUNC_IDIV,

	FUNC_IF,
	FUNC_MADD,
	FUNC_MUL3,

	FUNC_INTERVAL,
	FUNC_IN,

	FUNC_GEODIST
};


struct FuncDesc_t
{
	const char *	m_sName;
	int				m_iArgs;
	Func_e			m_eFunc;
};


static FuncDesc_t g_dFuncs[] =
{
	{ "now",	0,	FUNC_NOW },

	{ "abs",	1,	FUNC_ABS },
	{ "ceil",	1,	FUNC_CEIL },
	{ "floor",	1,	FUNC_FLOOR },
	{ "sin",	1,	FUNC_SIN },
	{ "cos",	1,	FUNC_COS },
	{ "ln",		1,	FUNC_LN },
	{ "log2",	1,	FUNC_LOG2 },
	{ "log10",	1,	FUNC_LOG10 },
	{ "exp",	1,	FUNC_EXP },
	{ "sqrt",	1,	FUNC_SQRT },
	{ "bigint",	1,	FUNC_BIGINT },	// type-enforcer special as-if-function

	{ "min",	2,	FUNC_MIN },
	{ "max",	2,	FUNC_MAX },
	{ "pow",	2,	FUNC_POW },
	{ "idiv",	2,	FUNC_IDIV },

	{ "if",		3,	FUNC_IF },
	{ "madd",	3,	FUNC_MADD },
	{ "mul3",	3,	FUNC_MUL3 },

	{ "interval",	-2,	FUNC_INTERVAL },
	{ "in",			-1, FUNC_IN },

	{ "geodist",	4,	FUNC_GEODIST }
};

//////////////////////////////////////////////////////////////////////////

/// check for type based on int value
static inline DWORD GetIntType ( int64_t iValue )
{
	return ( iValue>=(int64_t)INT_MIN && iValue<=(int64_t)INT_MAX ) ? SPH_ATTR_INTEGER : SPH_ATTR_BIGINT;
}

/// list of constants
class ConstList_c
{
public:
	CSphVector<int64_t>		m_dInts;		///< dword/int64 storage
	CSphVector<float>		m_dFloats;		///< float storage
	DWORD					m_uRetType;		///< SPH_ATTR_INTEGER, SPH_ATTR_BIGINT, or SPH_ATTR_FLOAT

public:
	ConstList_c ()
		: m_uRetType ( SPH_ATTR_INTEGER )
	{}

	void Add ( int64_t iValue )
	{
		if ( m_uRetType!=SPH_ATTR_FLOAT )
		{
			m_uRetType = GetIntType ( iValue );
			m_dInts.Add ( iValue );
		} else
		{
			m_dFloats.Add ( (float)iValue );
		}
	}

	void Add ( float fValue )
	{
		if ( m_uRetType!=SPH_ATTR_FLOAT )
		{
			assert ( m_dFloats.GetLength()==0 );
			ARRAY_FOREACH ( i, m_dInts )
				m_dFloats.Add ( (float)m_dInts[i] );
			m_dInts.Reset ();
			m_uRetType = SPH_ATTR_FLOAT;
		}
		m_dFloats.Add ( fValue );
	}
};

/// expression tree node
struct ExprNode_t
{
	int				m_iToken;	///< token type, including operators
	DWORD			m_uRetType;	///< result type
	DWORD			m_uArgType;	///< args type
	CSphAttrLocator	m_tLocator;	///< attribute locator, for TOK_ATTR type
	union
	{
		int64_t			m_iConst;		///< constant value, for TOK_CONST_INT type
		float			m_fConst;		///< constant value, for TOK_CONST_FLOAT type
		int				m_iFunc;		///< built-in function id, for TOK_FUNC type
		int				m_iArgs;		///< args count, for arglist (token==',') type
		ConstList_c *	m_pConsts;		///< constants list, for TOK_CONST_LIST type
	};
	int				m_iLeft;
	int				m_iRight;

	ExprNode_t () : m_iToken ( 0 ), m_uRetType ( SPH_ATTR_NONE ), m_uArgType ( SPH_ATTR_NONE ), m_iLeft ( -1 ), m_iRight ( -1 ) {}

	float FloatVal()
	{
		assert ( m_iToken==TOK_CONST_INT || m_iToken==TOK_CONST_FLOAT );
		return  ( m_iToken==TOK_CONST_INT ) ? (float)m_iConst : m_fConst;
	}
};

/// expression parser
class ExprParser_t
{
	friend int				yylex ( YYSTYPE * lvalp, ExprParser_t * pParser );
	friend int				yyparse ( ExprParser_t * pParser );
	friend void				yyerror ( ExprParser_t * pParser, const char * sMessage );

public:
							ExprParser_t () {}
							~ExprParser_t ();

	ISphExpr *				Parse ( const char * sExpr, const CSphSchema & tSchema, DWORD * pAttrType, bool * pUsesWeight, CSphString & sError );

protected:
	int						m_iParsed;	///< filled by yyparse() at the very end
	CSphString				m_sLexerError;
	CSphString				m_sParserError;
	CSphString				m_sCreateError;

protected:
	DWORD					GetWidestRet ( int iLeft, int iRight );

	int						AddNodeInt ( int64_t iValue );
	int						AddNodeFloat ( float fValue );
	int						AddNodeAttr ( int iTokenType, int iAttrLocator );
	int						AddNodeID ();
	int						AddNodeWeight ();
	int						AddNodeOp ( int iOp, int iLeft, int iRight );
	int						AddNodeFunc ( int iFunc, int iLeft, int iRight=-1 );
	int						AddNodeConstlist ( int64_t iValue );
	int						AddNodeConstlist ( float iValue );
	void					AppendToConstlist ( int iNode, int64_t iValue );
	void					AppendToConstlist ( int iNode, float iValue );

private:
	const char *			m_sExpr;
	const char *			m_pCur;
	const char *			m_pLastTokenStart;
	const CSphSchema *		m_pSchema;
	CSphVector<ExprNode_t>	m_dNodes;

	int						m_iConstNow;

private:
	int						GetToken ( YYSTYPE * lvalp );

	void					GatherArgTypes ( int iNode, CSphVector<int> & dTypes );
	void					GatherArgNodes ( int iNode, CSphVector<int> & dNodes );

	bool					CheckForConstSet ( int iArgsNode, int iSkip );

	template < typename T >
	void					WalkTree ( int iRoot, T & FUNCTOR );

	void					Optimize ( int iNode );

	ISphExpr *				CreateTree ( int iNode );
	ISphExpr *				CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs );
	ISphExpr *				CreateInNode ( int iNode );
	ISphExpr *				CreateGeodistNode ( int iArgs );
};

//////////////////////////////////////////////////////////////////////////

/// parse that numeric constant
static int ParseNumeric ( YYSTYPE * lvalp, const char ** ppStr )
{
	assert ( lvalp && ppStr && *ppStr );

	// try float route
	char * pEnd = NULL;
	float fRes = (float) strtod ( *ppStr, &pEnd );

	// try int route
	int64_t iRes = 0;
	bool bInt = true;
	for ( const char * p=(*ppStr); p<pEnd; p++ && bInt )
	{
		if ( isdigit(*p) )
			iRes = iRes*10 + (int)( (*p)-'0' ); // FIXME! missing overflow check, missing octal/hex handling
		else
			bInt = false;
	}

	// choose your destiny
	*ppStr = pEnd;
	if ( bInt )
	{
		lvalp->iConst = iRes;
		return TOK_CONST_INT;
	} else
	{
		lvalp->fConst = fRes;
		return TOK_CONST_FLOAT;
	}
}


static bool IsNumericAttrType ( DWORD eType )
{
	return eType==SPH_ATTR_INTEGER
		|| eType==SPH_ATTR_TIMESTAMP
		|| eType==SPH_ATTR_BOOL
		|| eType==SPH_ATTR_FLOAT
		|| eType==SPH_ATTR_BIGINT;
}


/// a lexer of my own
/// returns token id and fills lvalp on success
/// returns -1 and fills sError on failure
int ExprParser_t::GetToken ( YYSTYPE * lvalp )
{
	// skip whitespace, check eof
	while ( isspace(*m_pCur) ) m_pCur++;
	m_pLastTokenStart = m_pCur;
	if ( !*m_pCur ) return 0;

	// check for constant
	if ( isdigit(*m_pCur) )
		return ParseNumeric ( lvalp, &m_pCur );

	// check for field, function, or magic name
	if ( sphIsAttr(m_pCur[0])
		|| ( m_pCur[0]=='@' && sphIsAttr(m_pCur[1]) && !isdigit(m_pCur[1]) ) )
	{
		// get token
		const char * pStart = m_pCur++;
		while ( sphIsAttr(*m_pCur) ) m_pCur++;

		CSphString sTok;
		sTok.SetBinary ( pStart, m_pCur-pStart );
		sTok.ToLower ();

		// check for magic name
		if ( sTok=="@id" )		{ return TOK_ID; }
		if ( sTok=="@weight" )	{ return TOK_WEIGHT; }
		if ( sTok=="@geodist" )
		{
			int iGeodist = m_pSchema->GetAttrIndex("@geodist");
			if ( iGeodist==-1 )
			{
				m_sLexerError = "geoanchor is not set, @geodist expression unavailable";
				return -1;
			}
			const CSphAttrLocator & tLoc = m_pSchema->GetAttr(iGeodist).m_tLocator;
			lvalp->iAttrLocator = ( tLoc.m_iBitOffset << 16 ) + tLoc.m_iBitCount;
			return TOK_ATTR_FLOAT;
		}

		// check for keyword
		if ( sTok=="and" )		{ return TOK_AND; }
		if ( sTok=="or" )		{ return TOK_OR; }
		if ( sTok=="not" )		{ return TOK_NOT; }

		// check for attribute
		int iAttr = m_pSchema->GetAttrIndex ( sTok.cstr() );
		if ( iAttr>=0 )
		{
			// check attribute type and width
			const CSphColumnInfo & tCol = m_pSchema->GetAttr ( iAttr );
			if ( IsNumericAttrType(tCol.m_eAttrType) || ( tCol.m_eAttrType & SPH_ATTR_MULTI ) )
			{
				lvalp->iAttrLocator = ( tCol.m_tLocator.m_iBitOffset<<16 ) + tCol.m_tLocator.m_iBitCount;

				if ( tCol.m_eAttrType==SPH_ATTR_FLOAT )			return TOK_ATTR_FLOAT;
				else if ( tCol.m_eAttrType & SPH_ATTR_MULTI )	return TOK_ATTR_MVA;
				else if ( tCol.m_tLocator.IsBitfield() )		return TOK_ATTR_BITS;
				else											return TOK_ATTR_INT;

			} else
			{
				m_sLexerError.SetSprintf ( "attribute '%s' is of unsupported type (type=%d)", sTok.cstr(), tCol.m_eAttrType );
				return -1;
			}
		}

		// check for function
		sTok.ToLower();
		for ( int i=0; i<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])); i++ )
			if ( sTok==g_dFuncs[i].m_sName )
		{
			lvalp->iFunc = i;
			return g_dFuncs[i].m_eFunc==FUNC_IN ? TOK_FUNC_IN : TOK_FUNC;
		}

		m_sLexerError.SetSprintf ( "unknown identifier '%s' (not an attribute, not a function)", sTok.cstr() );
		return -1;
	}

	// check for known operators, then
	switch ( *m_pCur )
	{
		case '+':
		case '-':
		case '*':
		case '/':
		case '(':
		case ')':
		case ',':
			return *m_pCur++;

		case '<':
			m_pCur++;
			if ( *m_pCur=='>' ) { m_pCur++; return TOK_NE; }
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_LTE; }
			return '<';

		case '>':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_GTE; }
			return '>';

		case '=':
			m_pCur++;
			if ( *m_pCur=='=' ) m_pCur++;
			return TOK_EQ;

		// special case for float values without leading zero
		case '.':
			char * pEnd = NULL;
			lvalp->fConst = (float) strtod ( m_pCur, &pEnd );
			if ( pEnd )
			{
				m_pCur = pEnd;
				return TOK_CONST_FLOAT;
			}
	}

	m_sLexerError.SetSprintf ( "unknown operator '%c' near '%s'", *m_pCur, m_pCur );
	return -1;
}

/// is arithmetic?
static inline bool IsAri ( int iTok )
{
	return iTok=='+' || iTok=='-' || iTok=='*' || iTok=='/';
}

/// is constant?
static inline bool IsConst ( int iTok )
{
	return iTok==TOK_CONST_INT || iTok==TOK_CONST_FLOAT;
}

/// optimize subtree
void ExprParser_t::Optimize ( int iNode )
{
	if ( iNode<0 )
		return;

	Optimize ( m_dNodes[iNode].m_iLeft );
	Optimize ( m_dNodes[iNode].m_iRight );

	ExprNode_t * pRoot = &m_dNodes[iNode];

	// madd, mul3
	if ( ( pRoot->m_iToken=='+' || pRoot->m_iToken=='*' )
		&& ( m_dNodes[pRoot->m_iLeft].m_iToken=='*' || m_dNodes[pRoot->m_iRight].m_iToken=='*' ) )
	{
		if ( m_dNodes[pRoot->m_iLeft].m_iToken!='*' )
			Swap ( pRoot->m_iLeft, pRoot->m_iRight );
		assert ( m_dNodes[pRoot->m_iLeft].m_iToken=='*' );

		m_dNodes.Resize ( m_dNodes.GetLength()+1 );
		pRoot = &m_dNodes[iNode];

		m_dNodes[pRoot->m_iLeft].m_iToken = ',';

		m_dNodes.Last().m_iToken = ',';
		m_dNodes.Last().m_iLeft = pRoot->m_iLeft;
		m_dNodes.Last().m_iRight = pRoot->m_iRight;

		pRoot->m_iFunc = ( pRoot->m_iToken=='+' ) ? FUNC_MADD : FUNC_MUL3;
		pRoot->m_iToken = TOK_FUNC;
		assert ( g_dFuncs[pRoot->m_iFunc].m_eFunc==pRoot->m_iFunc );

		pRoot->m_iLeft = m_dNodes.GetLength()-1;
		pRoot->m_iRight = -1;
		return;
	}

	// constant arithmetic expression
	if ( IsAri ( pRoot->m_iToken ) )
	{
		const ExprNode_t & tLeft = m_dNodes[pRoot->m_iLeft];
		const ExprNode_t & tRight = m_dNodes[pRoot->m_iRight];

		if ( IsConst(tLeft.m_iToken) && IsConst(tRight.m_iToken) )
		{
			if ( tLeft.m_iToken==TOK_CONST_INT && tRight.m_iToken==TOK_CONST_INT && pRoot->m_iToken!='/' )
			{
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_iConst = tLeft.m_iConst + tRight.m_iConst; break;
					case '-':	pRoot->m_iConst = tLeft.m_iConst - tRight.m_iConst; break;
					case '*':	pRoot->m_iConst = tLeft.m_iConst * tRight.m_iConst; break;
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-int optimization" );
				}
				pRoot->m_iToken = TOK_CONST_INT;

			} else
			{
				float fLeft = ( tLeft.m_iToken==TOK_CONST_FLOAT ) ? tLeft.m_fConst : float(tLeft.m_iConst);
				float fRight = ( tRight.m_iToken==TOK_CONST_FLOAT ) ? tRight.m_fConst : float(tRight.m_iConst);
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_fConst = fLeft + fRight; break;
					case '-':	pRoot->m_fConst = fLeft - fRight; break;
					case '*':	pRoot->m_fConst = fLeft * fRight; break;
					case '/':	pRoot->m_fConst = fLeft / fRight; break;
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-float optimization" );
				}
				pRoot->m_iToken = TOK_CONST_FLOAT;
			}
			return;
		}
	}

	// division by a constant (replace with multiplication by inverse)
	if ( pRoot->m_iToken=='/' && m_dNodes[pRoot->m_iRight].m_iToken==TOK_CONST_FLOAT )
	{
		m_dNodes[pRoot->m_iRight].m_fConst = 1.0f / m_dNodes[pRoot->m_iRight].m_fConst;
		pRoot->m_iToken = '*';
		return;
	}

	// unary function from a constant
	if ( pRoot->m_iToken==TOK_FUNC && g_dFuncs[pRoot->m_iFunc].m_iArgs==1 )
	{
		const ExprNode_t & tArg = m_dNodes[pRoot->m_iLeft];
		if ( tArg.m_iToken==TOK_CONST_FLOAT || tArg.m_iToken==TOK_CONST_INT )
		{
			float fArg = tArg.m_iToken==TOK_CONST_FLOAT ? tArg.m_fConst : float(tArg.m_iConst);
			switch ( g_dFuncs[pRoot->m_iFunc].m_eFunc )
			{
				case FUNC_ABS:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = fabs(fArg); break;
				case FUNC_CEIL:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(ceil(fArg)); break;
				case FUNC_FLOOR:	pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(floor(fArg)); break;
				case FUNC_SIN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(sin(fArg)); break;
				case FUNC_COS:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(cos(fArg)); break;
				case FUNC_LN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)); break;
				case FUNC_LOG2:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)*M_LOG2E); break;
				case FUNC_LOG10:	pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)*M_LOG10E); break;
				case FUNC_EXP:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(exp(fArg)); break;
				case FUNC_SQRT:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(sqrt(fArg)); break;
				default:			break;
			}
			return;
		}
	}

	// constant function (such as NOW())
	if ( pRoot->m_iToken==TOK_FUNC && pRoot->m_iFunc==FUNC_NOW )
	{
		pRoot->m_iToken = TOK_CONST_INT;
		pRoot->m_iConst = m_iConstNow;
		return;
	}
}


/// fold arglist into array
static void FoldArglist ( ISphExpr * pLeft, CSphVector<ISphExpr *> & dArgs )
{
	if ( !pLeft || !pLeft->IsArglist() )
	{
		dArgs.Add ( pLeft );
		return;
	}

	Expr_Arglist_c * pArgs = dynamic_cast<Expr_Arglist_c *> ( pLeft );
	assert ( pLeft );

	Swap ( dArgs, pArgs->m_dArgs );
	SafeRelease ( pLeft );
}


/// fold nodes subtree into opcodes
ISphExpr * ExprParser_t::CreateTree ( int iNode )
{
	if ( iNode<0 )
		return NULL;

	const ExprNode_t & tNode = m_dNodes[iNode];
	
	// avoid spawning argument node in some cases
	bool bSkipLeft = false;
	bool bSkipRight = false;
	if ( tNode.m_iToken==TOK_FUNC )
	{
		Func_e eFunc = g_dFuncs[tNode.m_iFunc].m_eFunc;
		if ( eFunc==FUNC_GEODIST || eFunc==FUNC_IN )
			bSkipLeft = true;
		if ( eFunc==FUNC_IN )
			bSkipRight = true;
	}

	ISphExpr * pLeft = bSkipLeft ? NULL : CreateTree ( tNode.m_iLeft );
	ISphExpr * pRight = bSkipRight ? NULL : CreateTree ( tNode.m_iRight );

#define LOC_SPAWN_POLY(_classname) \
	if ( tNode.m_uArgType==SPH_ATTR_INTEGER )		return new _classname##Int_c ( pLeft, pRight ); \
	else if ( tNode.m_uArgType==SPH_ATTR_BIGINT )	return new _classname##Int64_c ( pLeft, pRight ); \
	else											return new _classname##Float_c ( pLeft, pRight );

	switch ( tNode.m_iToken )
	{
		case TOK_ATTR_INT:		return new Expr_GetInt_c ( tNode.m_tLocator );
		case TOK_ATTR_BITS:		return new Expr_GetBits_c ( tNode.m_tLocator );
		case TOK_ATTR_FLOAT:	return new Expr_GetFloat_c ( tNode.m_tLocator );
		case TOK_CONST_FLOAT:	return new Expr_GetConst_c ( tNode.m_fConst );
		case TOK_CONST_INT:
			if ( tNode.m_uRetType==SPH_ATTR_INTEGER )
				return new Expr_GetIntConst_c ( (int)tNode.m_iConst );
			else if ( tNode.m_uRetType==SPH_ATTR_BIGINT )
				return new Expr_GetInt64Const_c ( tNode.m_iConst );
			else
				return new Expr_GetConst_c ( float(tNode.m_iConst) );
			break;

		case TOK_ID:			return new Expr_GetId_c ();
		case TOK_WEIGHT:		return new Expr_GetWeight_c ();

		case '+':				return new Expr_Add_c ( pLeft, pRight ); break;
		case '-':				return new Expr_Sub_c ( pLeft, pRight ); break;
		case '*':				return new Expr_Mul_c ( pLeft, pRight ); break;
		case '/':				return new Expr_Div_c ( pLeft, pRight ); break;

		case '<':				LOC_SPAWN_POLY ( Expr_Lt ); break;
		case '>':				LOC_SPAWN_POLY ( Expr_Gt ); break;
		case TOK_LTE:			LOC_SPAWN_POLY ( Expr_Lte ); break;
		case TOK_GTE:			LOC_SPAWN_POLY ( Expr_Gte ); break;
		case TOK_EQ:			LOC_SPAWN_POLY ( Expr_Eq ); break;
		case TOK_NE:			LOC_SPAWN_POLY ( Expr_Ne ); break;
		case TOK_AND:			LOC_SPAWN_POLY ( Expr_And ); break;
		case TOK_OR:			LOC_SPAWN_POLY ( Expr_Or ); break;
		case TOK_NOT:			if ( tNode.m_uArgType==SPH_ATTR_BIGINT ) return new Expr_NotInt64_c ( pLeft ); else return new Expr_NotInt_c ( pLeft );  break;

		case ',':				return new Expr_Arglist_c ( pLeft, pRight ); break;
		case TOK_NEG:			assert ( pRight==NULL ); return new Expr_Neg_c ( pLeft ); break;
		case TOK_FUNC:
			{
				// fold arglist to array
				Func_e eFunc = g_dFuncs[tNode.m_iFunc].m_eFunc;

				CSphVector<ISphExpr *> dArgs;
				if ( !bSkipLeft )
					FoldArglist ( pLeft, dArgs );

				// spawn proper function
				assert ( tNode.m_iFunc>=0 && tNode.m_iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );
				assert (
					( bSkipLeft ) || // function will handle its arglist,
					( g_dFuncs[tNode.m_iFunc].m_iArgs>=0 && g_dFuncs[tNode.m_iFunc].m_iArgs==dArgs.GetLength() ) || // arg count matches,
					( g_dFuncs[tNode.m_iFunc].m_iArgs<0 && -g_dFuncs[tNode.m_iFunc].m_iArgs<=dArgs.GetLength() ) ); // or min vararg count reached

				switch ( eFunc )
				{
					case FUNC_NOW:		assert ( 0 ); break; // prevent gcc bitching

					case FUNC_ABS:		return new Expr_Abs_c ( dArgs[0] );
					case FUNC_CEIL:		return new Expr_Ceil_c ( dArgs[0] );
					case FUNC_FLOOR:	return new Expr_Floor_c ( dArgs[0] );
					case FUNC_SIN:		return new Expr_Sin_c ( dArgs[0] );
					case FUNC_COS:		return new Expr_Cos_c ( dArgs[0] );
					case FUNC_LN:		return new Expr_Ln_c ( dArgs[0] );
					case FUNC_LOG2:		return new Expr_Log2_c ( dArgs[0] );
					case FUNC_LOG10:	return new Expr_Log10_c ( dArgs[0] );
					case FUNC_EXP:		return new Expr_Exp_c ( dArgs[0] );
					case FUNC_SQRT:		return new Expr_Sqrt_c ( dArgs[0] );
					case FUNC_BIGINT:	return dArgs[0];

					case FUNC_MIN:		return new Expr_Min_c ( dArgs[0], dArgs[1] );
					case FUNC_MAX:		return new Expr_Max_c ( dArgs[0], dArgs[1] );
					case FUNC_POW:		return new Expr_Pow_c ( dArgs[0], dArgs[1] );
					case FUNC_IDIV:		return new Expr_Idiv_c ( dArgs[0], dArgs[1] );

					case FUNC_IF:		return new Expr_If_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MADD:		return new Expr_Madd_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MUL3:		return new Expr_Mul3_c ( dArgs[0], dArgs[1], dArgs[2] );

					case FUNC_INTERVAL:	return CreateIntervalNode ( tNode.m_iLeft, dArgs );
					case FUNC_IN:		return CreateInNode ( iNode );

					case FUNC_GEODIST:	return CreateGeodistNode ( tNode.m_iLeft );
				}
				assert ( 0 && "unhandled function id" );
				break;
			}

		default:				assert ( 0 && "unhandled token type" ); break;
	}

#undef LOC_SPAWN_POLY

	// fire exit
	SafeRelease ( pLeft );
	SafeRelease ( pRight );
	return NULL;
}

//////////////////////////////////////////////////////////////////////////

/// arg-vs-set function (currently, IN or INTERVAL) evaluator traits
template < typename T >
class Expr_ArgVsSet_c : public ISphExpr
{
protected:
	ISphExpr *			m_pArg;

public:
	Expr_ArgVsSet_c ( ISphExpr * pArg ) : m_pArg ( pArg ) {}
	~Expr_ArgVsSet_c () { SafeRelease ( m_pArg ); }

	virtual int IntEval ( const CSphMatch & tMatch ) const = 0;
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) IntEval ( tMatch ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return IntEval ( tMatch ); }

protected:
	T ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const;
};

template<> int Expr_ArgVsSet_c<int>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const			{ return pArg->IntEval ( tMatch ); }
template<> DWORD Expr_ArgVsSet_c<DWORD>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const		{ return (DWORD)pArg->IntEval ( tMatch ); }
template<> float Expr_ArgVsSet_c<float>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const		{ return pArg->Eval ( tMatch ); }
template<> int64_t Expr_ArgVsSet_c<int64_t>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const	{ return pArg->Int64Eval ( tMatch ); }


/// arg-vs-constant-set
template < typename T >
class Expr_ArgVsConstSet_c : public Expr_ArgVsSet_c<T>
{
protected:
	CSphVector<T> m_dValues;

public:
	/// take ownership of arg, pre-evaluate and dismiss turn points
	Expr_ArgVsConstSet_c ( ISphExpr * pArg, CSphVector<ISphExpr *> & dArgs, int iSkip )
		: Expr_ArgVsSet_c<T> ( pArg )
	{
		CSphMatch tDummy;
		for ( int i=iSkip; i<dArgs.GetLength(); i++ )
		{
			m_dValues.Add ( Expr_ArgVsSet_c<T>::ExprEval ( dArgs[i], tDummy ) );
			SafeRelease ( dArgs[i] );
		}
	}

	/// take ownership of arg, and copy that constlist
	Expr_ArgVsConstSet_c ( ISphExpr * pArg, ConstList_c * pConsts )
		: Expr_ArgVsSet_c<T> ( pArg )
	{
		if ( pConsts->m_uRetType==SPH_ATTR_FLOAT )
		{
			m_dValues.Reserve ( pConsts->m_dFloats.GetLength() );
			ARRAY_FOREACH ( i, pConsts->m_dFloats )
				m_dValues.Add ( (T)pConsts->m_dFloats[i] );
		} else
		{
			m_dValues.Reserve ( pConsts->m_dInts.GetLength() );
			ARRAY_FOREACH ( i, pConsts->m_dInts  )
				m_dValues.Add ( (T)pConsts->m_dInts[i] );
		}
	}
};

//////////////////////////////////////////////////////////////////////////

/// INTERVAL() evaluator for constant turn point values case
template < typename T >
class Expr_IntervalConst_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// take ownership of arg, pre-evaluate and dismiss turn points
	Expr_IntervalConst_c ( CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsConstSet_c<T> ( dArgs[0], dArgs, 1 )
	{}

	/// evaluate arg, return interval id
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, this->m_dValues ) // FIXME! OPTIMIZE! perform binary search here
			if ( val<this->m_dValues[i] )
				return i;
		return this->m_dValues.GetLength();
	}

	/// set MVA pool
	virtual void SetMVAPool ( const DWORD * pMvaPool )
	{
		this->m_pArg->SetMVAPool ( pMvaPool );
	}
};


/// INTERVAL() evaluator for generic case
template < typename T >
class Expr_Interval_c : public Expr_ArgVsSet_c<T>
{
protected:
	CSphVector<ISphExpr *> m_dTurnPoints;

public:
	/// take ownership of arg and turn points
	Expr_Interval_c ( const CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsSet_c<T> ( dArgs[0] )
	{
		for ( int i=1; i<dArgs.GetLength(); i++ )
			m_dTurnPoints.Add ( dArgs[i] );
	}

	/// evaluate arg, return interval id
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, m_dTurnPoints )
			if ( val < Expr_ArgVsSet_c<T>::ExprEval ( m_dTurnPoints[i], tMatch ) )
				return i;
		return m_dTurnPoints.GetLength();
	}

	/// set MVA pool
	virtual void SetMVAPool ( const DWORD * pMvaPool )
	{
		this->m_pArg->SetMVAPool ( pMvaPool );
		ARRAY_FOREACH ( i, m_dTurnPoints )
			m_dTurnPoints[i]->SetMVAPool ( pMvaPool );
	}
};

//////////////////////////////////////////////////////////////////////////

/// IN() evaluator, arbitrary scalar expression vs. constant values
template < typename T >
class Expr_In_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// pre-sort values for binary search
	Expr_In_c ( ISphExpr * pArg, ConstList_c * pConsts ) :
		Expr_ArgVsConstSet_c<T> ( pArg, pConsts )
	{
		this->m_dValues.Sort();
	}

	/// evaluate arg, check if the value is within set
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		return this->m_dValues.BinarySearch ( val )!=NULL;
	}

	/// set MVA pool
	virtual void SetMVAPool ( const DWORD * pMvaPool )
	{
		this->m_pArg->SetMVAPool ( pMvaPool );
	}
};


/// IN() evaluator, MVA attribute vs. constant values
class Expr_MVAIn_c : public Expr_ArgVsConstSet_c<DWORD>
{
public:
	/// pre-sort values for binary search
	Expr_MVAIn_c ( const CSphAttrLocator & tLoc, ConstList_c * pConsts )
		: Expr_ArgVsConstSet_c<DWORD> ( NULL, pConsts )
		, m_tLocator ( tLoc )
		, m_pMvaPool ( NULL )
	{
		assert ( tLoc.m_iBitOffset>=0 && tLoc.m_iBitCount>0 );
		this->m_dValues.Sort();
	}

	/// evaluate arg, check if any values are within set
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		const DWORD * pMva = tMatch.GetAttrMVA ( m_tLocator, m_pMvaPool );
		if ( !pMva )
			return 0;

		// OPTIMIZE! FIXME! factor out a common function with Filter_MVAValues::Eval()
		DWORD uLen = *pMva++;
		const DWORD * pMvaMax = pMva+uLen;

		const DWORD * pFilter = &m_dValues[0];
		const DWORD * pFilterMax = pFilter + m_dValues.GetLength();

		const DWORD * L = pMva;
		const DWORD * R = pMvaMax - 1;
		for ( ; pFilter < pFilterMax; pFilter++ )
		{
			while ( L <= R )
			{
				const DWORD * m = L + (R - L) / 2;
				if ( *pFilter > *m )
					L = m + 1;
				else if ( *pFilter < *m )
					R = m - 1;
				else
					return 1;
			}
			R = pMvaMax - 1;
		}
		return 0;
	}

	/// set MVA pool
	virtual void SetMVAPool ( const DWORD * pMvaPool )
	{
		m_pMvaPool = pMvaPool; // finally, some real setup work!!!
	}

protected:
	CSphAttrLocator	m_tLocator;
	const DWORD *	m_pMvaPool;
};

//////////////////////////////////////////////////////////////////////////

static inline double sphSqr ( double v ) { return v * v; }

static inline float CalcGeodist ( float fPointLat, float fPointLon, float fAnchorLat, float fAnchorLon )
{
	const double R = 6384000;
	double dlat = fPointLat - fAnchorLat;
	double dlon = fPointLon - fAnchorLon;
	double a = sphSqr(sin(dlat/2)) + cos(fPointLat)*cos(fAnchorLat)*sphSqr(sin(dlon/2));
	double c = 2*asin ( Min ( 1, sqrt(a) ) );
	return float(R*c);
}

/// geodist() - attr point, constant anchor
class Expr_GeodistAttrConst_c: public ISphExpr
{
public:
	Expr_GeodistAttrConst_c ( CSphAttrLocator tLat, CSphAttrLocator tLon, float fAnchorLat, float fAnchorLon )
		: m_tLat ( tLat )
		, m_tLon ( tLon )
		, m_fAnchorLat ( fAnchorLat )
		, m_fAnchorLon ( fAnchorLon )
	{}

	virtual float Eval ( const CSphMatch & tMatch ) const
	{
		return CalcGeodist ( tMatch.GetAttrFloat ( m_tLat ), tMatch.GetAttrFloat ( m_tLon ), m_fAnchorLat, m_fAnchorLon );
	}

private:
	CSphAttrLocator	m_tLat;
	CSphAttrLocator	m_tLon;
	
	float		m_fAnchorLat;
	float		m_fAnchorLon;
};

/// geodist() - expr point, constant anchor
class Expr_GeodistConst_c: public ISphExpr
{
public:
	Expr_GeodistConst_c ( ISphExpr * pLat, ISphExpr * pLon, float fAnchorLat, float fAnchorLon )
		: m_pLat ( pLat )
		, m_pLon ( pLon )
		, m_fAnchorLat ( fAnchorLat )
		, m_fAnchorLon ( fAnchorLon )
	{}

	~Expr_GeodistConst_c ()
	{
		SafeRelease ( m_pLon );
		SafeRelease ( m_pLat );
	}

	virtual float Eval ( const CSphMatch & tMatch ) const
	{
		return CalcGeodist ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), m_fAnchorLat, m_fAnchorLon );
	}

private:
	ISphExpr *	m_pLat;
	ISphExpr *	m_pLon;
	
	float		m_fAnchorLat;
	float		m_fAnchorLon;
};

/// geodist() - expr point, expr anchor
class Expr_Geodist_c: public ISphExpr
{
public:
	Expr_Geodist_c ( ISphExpr * pLat, ISphExpr * pLon, ISphExpr * pAnchorLat, ISphExpr * pAnchorLon )
		: m_pLat ( pLat )
		, m_pLon ( pLon )
		, m_pAnchorLat ( pAnchorLat )
		, m_pAnchorLon ( pAnchorLon )
	{}

	~Expr_Geodist_c ()
	{
		SafeRelease ( m_pAnchorLon );
		SafeRelease ( m_pAnchorLat );
		SafeRelease ( m_pLon );
		SafeRelease ( m_pLat );
	}

	virtual float Eval ( const CSphMatch & tMatch ) const
	{
		return CalcGeodist ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), m_pAnchorLat->Eval(tMatch), m_pAnchorLon->Eval(tMatch) );
	}

private:
	ISphExpr *	m_pLat;
	ISphExpr *	m_pLon;

	ISphExpr *	m_pAnchorLat;
	ISphExpr *	m_pAnchorLon;
};

//////////////////////////////////////////////////////////////////////////

void ExprParser_t::GatherArgTypes ( int iNode, CSphVector<int> & dTypes )
{
	if ( iNode<0 )
		return;

	const ExprNode_t & tNode = m_dNodes[iNode];
	if ( tNode.m_iToken==',' )
	{
		GatherArgTypes ( tNode.m_iLeft, dTypes );
		GatherArgTypes ( tNode.m_iRight, dTypes );
	} else
	{
		dTypes.Add ( tNode.m_iToken );
	}
}

void ExprParser_t::GatherArgNodes ( int iNode, CSphVector<int> & dNodes )
{
	if ( iNode<0 )
		return;

	const ExprNode_t & tNode = m_dNodes[iNode];
	if ( tNode.m_iToken==',' )
	{
		GatherArgNodes ( tNode.m_iLeft, dNodes );
		GatherArgNodes ( tNode.m_iRight, dNodes );
	}
	else
		dNodes.Add ( iNode );
}

bool ExprParser_t::CheckForConstSet ( int iArgsNode, int iSkip )
{
	CSphVector<int> dTypes;
	GatherArgTypes ( iArgsNode, dTypes );

	for ( int i=iSkip; i<dTypes.GetLength(); i++ )
		if ( dTypes[i]!=TOK_CONST_INT && dTypes[i]!=TOK_CONST_FLOAT )
			return false;
	return true;
}


template < typename T >
void ExprParser_t::WalkTree ( int iRoot, T & FUNCTOR )
{
	if ( iRoot>=0 )
	{
		const ExprNode_t & tNode = m_dNodes[iRoot];
		FUNCTOR.Process ( tNode );
		WalkTree ( tNode.m_iLeft, FUNCTOR );
		WalkTree ( tNode.m_iRight, FUNCTOR );
	}
}


ISphExpr * ExprParser_t::CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs )
{
	assert ( dArgs.GetLength()>=2 );

	bool bConst = CheckForConstSet ( iArgsNode, 1 );
	DWORD uAttrType = m_dNodes[iArgsNode].m_uArgType;
	if ( bConst )
	{
		switch ( uAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_IntervalConst_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_IntervalConst_c<int64_t> ( dArgs ); break;
			default:				return new Expr_IntervalConst_c<float> ( dArgs ); break;
		}
	} else
	{
		switch ( uAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_Interval_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_Interval_c<int64_t> ( dArgs ); break;
			default:				return new Expr_Interval_c<float> ( dArgs ); break;
		}
	}
}


ISphExpr * ExprParser_t::CreateInNode ( int iNode )
{
	const ExprNode_t & tNode = m_dNodes[iNode];

	if ( m_dNodes[tNode.m_iRight].m_iToken!=TOK_CONST_LIST )
	{
		m_sCreateError = "IN() arguments must be constants (except the 1st one)";
		return NULL;
	}

	assert ( m_dNodes[tNode.m_iRight].m_iToken==TOK_CONST_LIST );
	ConstList_c * pConst = m_dNodes[tNode.m_iRight].m_pConsts;

	bool bMVA = ( m_dNodes[tNode.m_iLeft].m_iToken==TOK_ATTR_MVA );
	if ( bMVA )
	{
		return new Expr_MVAIn_c ( m_dNodes[tNode.m_iLeft].m_tLocator, pConst );

	} else
	{
		ISphExpr * pArg = CreateTree ( tNode.m_iLeft );
		switch ( pConst->m_uRetType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_In_c<int> ( pArg, pConst ); break;
			case SPH_ATTR_BIGINT:	return new Expr_In_c<int64_t> ( pArg, pConst ); break;
			default:				return new Expr_In_c<float> ( pArg, pConst ); break;
		}
	}
}

ISphExpr * ExprParser_t::CreateGeodistNode ( int iArgs )
{
	CSphVector<int> dArgs;
	GatherArgNodes ( iArgs, dArgs );
	assert ( dArgs.GetLength() == 4 );

	bool bConst1 = ( IsConst ( m_dNodes[dArgs[0]].m_iToken ) && IsConst ( m_dNodes[dArgs[1]].m_iToken ) );
	bool bConst2 = ( IsConst ( m_dNodes[dArgs[2]].m_iToken ) && IsConst ( m_dNodes[dArgs[3]].m_iToken ) );

	if ( bConst1 && bConst2 )
	{
		return new Expr_GetConst_c ( CalcGeodist (
			m_dNodes[dArgs[0]].FloatVal(), m_dNodes[dArgs[1]].FloatVal(),
			m_dNodes[dArgs[2]].FloatVal(), m_dNodes[dArgs[3]].FloatVal() ) );
	}

	if ( bConst1 )
	{
		Swap ( dArgs[0], dArgs[2] );
		Swap ( dArgs[1], dArgs[3] );
		Swap ( bConst1, bConst2 );
	}

	if ( bConst2 )
	{
		// constant anchor
		if ( m_dNodes[dArgs[0]].m_iToken==TOK_ATTR_FLOAT && m_dNodes[dArgs[1]].m_iToken==TOK_ATTR_FLOAT )
		{
			// attr point
			return new Expr_GeodistAttrConst_c (
				m_dNodes[dArgs[0]].m_tLocator, m_dNodes[dArgs[1]].m_tLocator,
				m_dNodes[dArgs[2]].FloatVal(), m_dNodes[dArgs[3]].FloatVal() );
		} else
		{
			// expr point
			return new Expr_GeodistConst_c (
				CreateTree ( dArgs[0] ), CreateTree ( dArgs[1] ),
				m_dNodes[dArgs[2]].FloatVal(), m_dNodes[dArgs[3]].FloatVal() );
		}
	}

	// four expressions
	CSphVector<ISphExpr *> dExpr;
	FoldArglist ( CreateTree ( iArgs ), dExpr );
	assert ( dExpr.GetLength() == 4 );
	return new Expr_Geodist_c ( dExpr[0], dExpr[1], dExpr[2], dExpr[3] );
}

//////////////////////////////////////////////////////////////////////////

int yylex ( YYSTYPE * lvalp, ExprParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( ExprParser_t * pParser, const char * sMessage )
{
	pParser->m_sParserError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#if USE_WINDOWS
#pragma warning(push,1)
#endif

#include "yysphinxexpr.c"

#if USE_WINDOWS
#pragma warning(pop)
#endif

//////////////////////////////////////////////////////////////////////////

ExprParser_t::~ExprParser_t ()
{
	// i kinda own those constlists
	ARRAY_FOREACH ( i, m_dNodes )
		if ( m_dNodes[i].m_iToken==TOK_CONST_LIST )
			SafeDelete ( m_dNodes[i].m_pConsts );
}

DWORD ExprParser_t::GetWidestRet ( int iLeft, int iRight )
{
	DWORD uLeftType = ( iLeft<0 ) ? SPH_ATTR_INTEGER : m_dNodes[iLeft].m_uRetType;
	DWORD uRightType = ( iRight<0 ) ? SPH_ATTR_INTEGER : m_dNodes[iRight].m_uRetType;

	DWORD uRes = SPH_ATTR_FLOAT; // default is float
	if ( ( uLeftType==SPH_ATTR_INTEGER || uLeftType==SPH_ATTR_BIGINT ) &&
		( uRightType==SPH_ATTR_INTEGER || uRightType==SPH_ATTR_BIGINT ) )
	{
		// both types are integer (int32 or int64), compute in integers
		uRes = ( uLeftType==SPH_ATTR_INTEGER && uRightType==SPH_ATTR_INTEGER )
			? SPH_ATTR_INTEGER
			: SPH_ATTR_BIGINT;
	}
	return uRes;
}

int ExprParser_t::AddNodeInt ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_INT;
	tNode.m_uRetType = GetIntType ( iValue );
	tNode.m_iConst = iValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeFloat ( float fValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_FLOAT;
	tNode.m_uRetType = SPH_ATTR_FLOAT;
	tNode.m_fConst = fValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeAttr ( int iTokenType, int iAttrLocator )
{
	assert ( iTokenType==TOK_ATTR_INT || iTokenType==TOK_ATTR_BITS || iTokenType==TOK_ATTR_FLOAT || iTokenType==TOK_ATTR_MVA );
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iTokenType;
	tNode.m_tLocator.m_iBitOffset = iAttrLocator>>16;
	tNode.m_tLocator.m_iBitCount = iAttrLocator&0xffff;

	if ( iTokenType==TOK_ATTR_FLOAT )			tNode.m_uRetType = SPH_ATTR_FLOAT;
	else if ( iTokenType==TOK_ATTR_MVA )		tNode.m_uRetType = SPH_ATTR_MULTI;
	else if ( tNode.m_tLocator.m_iBitCount>32 )	tNode.m_uRetType = SPH_ATTR_BIGINT;
	else										tNode.m_uRetType = SPH_ATTR_INTEGER;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeID ()
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_ID;
	tNode.m_uRetType = USE_64BIT ? SPH_ATTR_BIGINT : SPH_ATTR_INTEGER;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeWeight ()
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_WEIGHT;
	tNode.m_uRetType = SPH_ATTR_INTEGER;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeOp ( int iOp, int iLeft, int iRight )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iOp;

	// deduce type
	tNode.m_uRetType = SPH_ATTR_FLOAT; // default to float
	if ( iOp==TOK_NEG )
	{
		// NEG just inherits the type
		tNode.m_uArgType = m_dNodes[iLeft].m_uRetType;
		tNode.m_uRetType = tNode.m_uArgType;

	} else if ( iOp==TOK_NOT )
	{
		// NOT result is integer, and its argument must be integer
		tNode.m_uArgType = m_dNodes[iLeft].m_uRetType;
		tNode.m_uRetType = SPH_ATTR_INTEGER;
		if (!( tNode.m_uArgType==SPH_ATTR_INTEGER || tNode.m_uArgType==SPH_ATTR_BIGINT ))
		{
			m_sParserError.SetSprintf ( "NOT argument must be integer" );
			return -1;
		}

	} else if ( iOp==TOK_LTE || iOp==TOK_GTE || iOp==TOK_EQ || iOp==TOK_NE
		|| iOp=='<' || iOp=='>' || iOp==TOK_AND || iOp==TOK_OR
		|| iOp=='+' || iOp=='-' || iOp=='*' || iOp==',' )
	{
		tNode.m_uArgType = GetWidestRet ( iLeft, iRight );

		// arithmetical operations return arg type, logical return int
		tNode.m_uRetType = ( iOp=='+' || iOp=='-' || iOp=='*' || iOp==',' )
			? tNode.m_uArgType
			: SPH_ATTR_INTEGER;

		// AND/OR can only be over ints
		if ( ( iOp==TOK_AND || iOp==TOK_OR ) && !( tNode.m_uArgType==SPH_ATTR_INTEGER || tNode.m_uArgType==SPH_ATTR_BIGINT ))
		{
			m_sParserError.SetSprintf ( "%s arguments must be integer", ( iOp==TOK_AND ) ? "AND" : "OR" );
			return -1;
		}

	} else
	{
		// check for unknown op
		assert ( iOp=='/' && "unknown op in AddNodeOp() type deducer" );
	}

	tNode.m_iArgs = 0;
	if ( iOp==',' )
	{
		if ( iLeft>=0 )		tNode.m_iArgs += ( m_dNodes[iLeft].m_iToken==',' ) ? m_dNodes[iLeft].m_iArgs : 1;
		if ( iRight>=0 )	tNode.m_iArgs += ( m_dNodes[iRight].m_iToken==',' ) ? m_dNodes[iRight].m_iArgs : 1;
	}
	tNode.m_iLeft = iLeft;
	tNode.m_iRight = iRight;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeFunc ( int iFunc, int iLeft, int iRight )
{
	assert ( iFunc>=0 && iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );

	// check args count if needed
	if ( iRight<0 )
	{
		int iExpectedArgc = g_dFuncs[iFunc].m_iArgs;
		int iArgc = 0;
		if ( iLeft>=0 )
			iArgc = ( m_dNodes[iLeft].m_iToken==',' ) ? m_dNodes[iLeft].m_iArgs : 1;
		if ( iExpectedArgc<0 )
		{
			if ( iArgc<-iExpectedArgc )
			{
				m_sParserError.SetSprintf ( "%s() called with %d args, at least %d args expected", g_dFuncs[iFunc].m_sName, iArgc, -iExpectedArgc );
				return -1;
			}
		} else if ( iArgc!=iExpectedArgc )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, %d args expected", g_dFuncs[iFunc].m_sName, iArgc, iExpectedArgc );
			return -1;
		}
	}

	// do add
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = iLeft;
	tNode.m_iRight = iRight;

	// deduce type
	tNode.m_uArgType = ( iLeft>=0 ) ? m_dNodes[iLeft].m_uRetType : SPH_ATTR_INTEGER;
	tNode.m_uRetType = SPH_ATTR_FLOAT; // by default, functions return floats

	Func_e eFunc = g_dFuncs[iFunc].m_eFunc;
	if ( eFunc==FUNC_NOW || eFunc==FUNC_INTERVAL || eFunc==FUNC_IN )
	{
		tNode.m_uRetType = SPH_ATTR_INTEGER;

	} else if ( eFunc==FUNC_MIN || eFunc==FUNC_MAX || eFunc==FUNC_MADD || eFunc==FUNC_MUL3 || eFunc==FUNC_ABS || eFunc==FUNC_BIGINT || eFunc==FUNC_IDIV )
	{
		tNode.m_uRetType = tNode.m_uArgType;

		if ( eFunc==FUNC_BIGINT && tNode.m_uRetType!=SPH_ATTR_FLOAT )
			tNode.m_uRetType = SPH_ATTR_BIGINT; // enforce if we can; FIXME! silently ignores BIGINT() on floats; should warn or raise an error

	} else if ( eFunc==FUNC_IF )
	{
		tNode.m_uRetType = GetWidestRet ( iLeft, iRight );
	}

	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeConstlist ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_CONST_LIST;
	tNode.m_pConsts = new ConstList_c();
	tNode.m_pConsts->Add ( iValue );
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeConstlist ( float iValue )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_CONST_LIST;
	tNode.m_pConsts = new ConstList_c();
	tNode.m_pConsts->Add ( iValue );
	return m_dNodes.GetLength()-1;
}

void ExprParser_t::AppendToConstlist ( int iNode, int64_t iValue )
{
	m_dNodes[iNode].m_pConsts->Add ( iValue );
}

void ExprParser_t::AppendToConstlist ( int iNode, float iValue )
{
	m_dNodes[iNode].m_pConsts->Add ( iValue );
}


struct WeightCheck_fn
{
	bool * m_pRes;

	WeightCheck_fn ( bool * pRes )
		: m_pRes ( pRes )
	{
		assert ( m_pRes );
		*m_pRes = false;
	}

	void Process ( const ExprNode_t & tNode )
	{
		if ( tNode.m_iToken==TOK_WEIGHT )
			*m_pRes = true;
	}
};


ISphExpr * ExprParser_t::Parse ( const char * sExpr, const CSphSchema & tSchema, DWORD * pAttrType, bool * pUsesWeight, CSphString & sError )
{
	m_sLexerError = "";
	m_sParserError = "";
	m_sCreateError = "";

	// setup lexer
	m_sExpr = sExpr;
	m_pCur = sExpr;
	m_pSchema = &tSchema;

	// setup constant functions
	m_iConstNow = (int) time ( NULL );

	// build tree
	m_iParsed = -1;
	yyparse ( this );

	// handle errors
	if ( m_iParsed<0 || !m_sLexerError.IsEmpty() || !m_sParserError.IsEmpty() )
	{
		sError = !m_sLexerError.IsEmpty() ? m_sLexerError : m_sParserError;
		if ( sError.IsEmpty() ) sError = "general parsing error";
		return NULL;
	}

	// deduce return type
	DWORD uAttrType = m_dNodes[m_iParsed].m_uRetType;
	assert ( uAttrType==SPH_ATTR_INTEGER || uAttrType==SPH_ATTR_BIGINT || uAttrType==SPH_ATTR_FLOAT );

	// perform optimizations
	Optimize ( m_iParsed );

	// create evaluator
	ISphExpr * pRes = CreateTree ( m_iParsed );
	if ( !m_sCreateError.IsEmpty() )
	{
		sError = m_sCreateError;
		SafeRelease ( pRes );
	} else if ( !pRes )
	{
		sError.SetSprintf ( "empty expression" );
	}

	if ( pAttrType )
		*pAttrType = uAttrType;

	if ( pUsesWeight )
	{
		WeightCheck_fn tFunctor ( pUsesWeight );
		WalkTree ( m_iParsed, tFunctor );
	}

	return pRes;
}

//////////////////////////////////////////////////////////////////////////
// PUBLIC STUFF
//////////////////////////////////////////////////////////////////////////

/// parser entry point
ISphExpr * sphExprParse ( const char * sExpr, const CSphSchema & tSchema, DWORD * pAttrType, bool * pUsesWeight, CSphString & sError )
{
	// parse into opcodes
	ExprParser_t tParser;
	return tParser.Parse ( sExpr, tSchema, pAttrType, pUsesWeight, sError );
}

//
// $Id: sphinxexpr.cpp 2026 2009-10-22 00:36:37Z shodan $
//
