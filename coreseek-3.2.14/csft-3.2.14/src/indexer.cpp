//
// $Id: indexer.cpp 2096 2009-11-24 06:54:21Z tomat $
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
#include "sphinxutils.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>

#include "py_layer.h"

#if USE_WINDOWS
	#define snprintf	_snprintf

	#include <io.h>
	#include <tlhelp32.h>
#else
	#include <unistd.h>
#endif

/////////////////////////////////////////////////////////////////////////////

bool			g_bQuiet		= false;
bool			g_bProgress		= true;

const char *	g_sBuildStops	= NULL;
int				g_iTopStops		= 100;
bool			g_bRotate		= false;
bool			g_bBuildFreqs	= false;

int				g_iMemLimit				= 0;
int				g_iMaxXmlpipe2Field		= 0;
int				g_iWriteBuffer			= 0;

const int		EXT_COUNT = 7;
const char *	g_dExt[EXT_COUNT] = { "sph", "spa", "spi", "spd", "spp", "spm", "spk" };



struct Word_t
{
	const char *	m_sWord;
	int				m_iCount;
};


inline bool operator < ( const Word_t & a, const Word_t & b)
{
	return a.m_iCount < b.m_iCount;
};





void CSphStopwordBuilderDict::Save ( const char * sOutput, int iTop, bool bFreqs )
{
	FILE * fp = fopen ( sOutput, "w+" );
	if ( !fp )
		return;

	CSphVector<Word_t> dTop;
	dTop.Reserve ( 1024 );

	const CSphMTFHashEntry<int> * it;
	HASH_FOREACH ( it, m_hWords )
	{
		Word_t t;
		t.m_sWord = it->m_sKey.cstr();
		t.m_iCount = it->m_tValue;
		dTop.Add ( t );
	}

	dTop.RSort ();

	ARRAY_FOREACH ( i, dTop )
	{
		if ( i>=iTop )
			break;
		if ( bFreqs )
			fprintf ( fp, "%s %d\n", dTop[i].m_sWord, dTop[i].m_iCount );
		else
			fprintf ( fp, "%s\n", dTop[i].m_sWord );
	}

	fclose ( fp );
}


SphWordID_t CSphStopwordBuilderDict::GetWordID ( BYTE * pWord )
{
	int iZero = 0;
	m_hWords.Add ( (const char *)pWord, 0, iZero )++;
	return 1;
}


SphWordID_t CSphStopwordBuilderDict::GetWordID ( const BYTE * pWord, int iLen, bool )
{
	int iZero = 0;
	m_hWords.Add ( (const char *)pWord, iLen, iZero )++;
	return 1;
}

/////////////////////////////////////////////////////////////////////////////

void ShowProgress ( const CSphIndexProgress * pProgress, bool bPhaseEnd )
{
	// if in quiet mode, do not show anything at all
	// if in no-progress mode, only show phase ends
	if ( g_bQuiet || ( !g_bProgress && !bPhaseEnd ) )
		return;

	switch ( pProgress->m_ePhase )
	{
		case CSphIndexProgress::PHASE_COLLECT:
			fprintf ( stdout, "collected %d docs, %.1f MB", pProgress->m_iDocuments, float(pProgress->m_iBytes)/1000000.0f );
			break;

		case CSphIndexProgress::PHASE_SORT:
			fprintf ( stdout, "sorted %.1f Mhits, %.1f%% done", float(pProgress->m_iHits)/1000000,
				100.0f*float(pProgress->m_iHits) / float(pProgress->m_iHitsTotal) );
			break;

		case CSphIndexProgress::PHASE_COLLECT_MVA:
			fprintf ( stdout, "collected %"PRIu64" attr values", pProgress->m_iAttrs );
			break;

		case CSphIndexProgress::PHASE_SORT_MVA:
			fprintf ( stdout, "sorted %.1f Mvalues, %.1f%% done", float(pProgress->m_iAttrs)/1000000,
				100.0f*float(pProgress->m_iAttrs) / float(pProgress->m_iAttrsTotal) );
			break;

		case CSphIndexProgress::PHASE_MERGE:
			fprintf ( stdout, "merged %.1f Kwords", float(pProgress->m_iWords)/1000 );
			break;
	}

	fprintf ( stdout, bPhaseEnd ? "\n" : "\r" );
	fflush ( stdout );
}

/////////////////////////////////////////////////////////////////////////////

/// parse multi-valued attr definition
bool ParseMultiAttr ( const char * sBuf, CSphColumnInfo & tAttr, const char * sSourceName )
{
	// format is as follows:
	//
	// multi-valued-attr := ATTR-TYPE ATTR-NAME 'from' SOURCE-TYPE [;QUERY] [;RANGE-QUERY]
	// ATTR-TYPE := 'uint' | 'timestamp'
	// SOURCE-TYPE := 'field' | 'query' | 'ranged-query'

	const char * sTok = NULL;
	int iTokLen = -1;

#define LOC_ERR(_arg,_pos) \
	{ \
		if ( !*(_pos) ) \
			fprintf ( stdout, "ERROR: source '%s': unexpected end of line in sql_attr_multi.\n", sSourceName ); \
		else \
			fprintf ( stdout, "ERROR: source '%s': expected " _arg " in sql_attr_multi, got '%s'.\n", sSourceName, _pos ); \
		return false; \
	}
#define LOC_SPACE0()		{ while ( isspace(*sBuf) ) sBuf++; }
#define LOC_SPACE1()		{ if ( !isspace(*sBuf) ) LOC_ERR ( "token", sBuf ) ; LOC_SPACE0(); }
#define LOC_TOK()			{ sTok = sBuf; while ( sphIsAlpha(*sBuf) ) sBuf++; iTokLen = sBuf-sTok; }
#define LOC_TOKEQ(_arg)		( iTokLen==(int)strlen(_arg) && strncasecmp ( sTok, _arg, iTokLen )==0 )
#define LOC_TEXT()			{ if ( *sBuf!=';') LOC_ERR ( "';'", sBuf ); sTok = ++sBuf; while ( *sBuf && *sBuf!=';' ) sBuf++; iTokLen = sBuf-sTok; }

	// handle ATTR-TYPE
	LOC_SPACE0(); LOC_TOK();
	if ( LOC_TOKEQ("uint") )				tAttr.m_eAttrType = SPH_ATTR_INTEGER | SPH_ATTR_MULTI;
	else if ( LOC_TOKEQ("timestamp") )		tAttr.m_eAttrType = SPH_ATTR_INTEGER | SPH_ATTR_MULTI;
	else									LOC_ERR ( "attr type ('uint' or 'timestamp')", sTok );

	// handle ATTR-NAME
	LOC_SPACE1(); LOC_TOK ();
	if ( iTokLen )							tAttr.m_sName.SetBinary ( sTok, iTokLen );
	else									LOC_ERR ( "attr name", sTok );

	// handle 'from'
	LOC_SPACE1(); LOC_TOK();
	if ( !LOC_TOKEQ("from") )				LOC_ERR ( "'from' keyword", sTok );

	// handle SOURCE-TYPE
	LOC_SPACE1(); LOC_TOK(); LOC_SPACE0();
	if ( LOC_TOKEQ("field") )				tAttr.m_eSrc = SPH_ATTRSRC_FIELD;
	else if ( LOC_TOKEQ("query") )			tAttr.m_eSrc = SPH_ATTRSRC_QUERY;
	else if ( LOC_TOKEQ("ranged-query") )	tAttr.m_eSrc = SPH_ATTRSRC_RANGEDQUERY;
	else									LOC_ERR ( "value source type ('field', or 'query', or 'ranged-query')", sTok );

	if ( tAttr.m_eSrc==SPH_ATTRSRC_FIELD )	return true;

	// handle QUERY
	LOC_TEXT();
	if ( iTokLen )							tAttr.m_sQuery.SetBinary ( sTok, iTokLen );
	else									LOC_ERR ( "query", sTok );

	if ( tAttr.m_eSrc==SPH_ATTRSRC_QUERY )	return true;

	// handle RANGE-QUERY
	LOC_TEXT();
	if ( iTokLen )							tAttr.m_sQueryRange.SetBinary ( sTok, iTokLen );
	else									LOC_ERR ( "range query", sTok );

#undef LOC_ERR
#undef LOC_SPACE0
#undef LOC_SPACE1
#undef LOC_TOK
#undef LOC_TOKEQ
#undef LOC_TEXT

	return true;
}


#define LOC_CHECK(_hash,_key,_msg,_add) \
	if (!( _hash.Exists ( _key ) )) \
	{ \
		fprintf ( stdout, "ERROR: key '%s' not found " _msg "\n", _key, _add ); \
		return false; \
	}

// get string
#define LOC_GETS(_arg,_key) \
	if ( hSource.Exists(_key) ) \
		_arg = hSource[_key];

// get int
#define LOC_GETI(_arg,_key) \
	if ( hSource.Exists(_key) && hSource[_key].intval() ) \
		_arg = hSource[_key].intval();

// get bool
#define LOC_GETB(_arg,_key) \
	if ( hSource.Exists(_key) ) \
		_arg = ( hSource[_key].intval()!=0 );

// get array of strings
#define LOC_GETAS(_arg,_key) \
	for ( CSphVariant * pVal = hSource(_key); pVal; pVal = pVal->m_pNext ) \
		_arg.Add ( pVal->cstr() );


void SqlAttrsConfigure ( CSphSourceParams_SQL & tParams, const CSphVariant * pHead, DWORD uAttrType, const char * sSourceName )
{
	for ( const CSphVariant * pCur = pHead; pCur; pCur= pCur->m_pNext )
	{
		CSphColumnInfo tCol ( pCur->cstr(), uAttrType );
		char * pColon = strchr ( const_cast<char*>(tCol.m_sName.cstr()), ':' );
		if ( pColon )
		{
			*pColon = '\0';

			if ( uAttrType==SPH_ATTR_INTEGER )
			{
				int iBits = strtol ( pColon+1, NULL, 10 );
				if ( iBits<=0 || iBits>ROWITEM_BITS )
				{
					fprintf ( stdout, "WARNING: source '%s': attribute '%s': invalid bitcount=%d (bitcount ignored)\n",
						sSourceName, tCol.m_sName.cstr(), iBits );
					iBits = -1;
				}
				tCol.m_tLocator.m_iBitCount = iBits;

			} else
			{
				fprintf ( stdout, "WARNING: source '%s': attribute '%s': bitcount is only supported for integer types\n",
					sSourceName, tCol.m_sName.cstr() );
			}
		}
		tParams.m_dAttrs.Add ( tCol );
	}
}


#if USE_ZLIB
bool ConfigureUnpack ( CSphVariant * pHead, ESphUnpackFormat eFormat, CSphSourceParams_SQL & tParams, const char * sSourceName )
{
	for ( CSphVariant * pVal = pHead; pVal; pVal = pVal->m_pNext )
	{
		CSphUnpackInfo & tUnpack = tParams.m_dUnpack.Add();
		tUnpack.m_sName = CSphString( pVal->cstr() );
		tUnpack.m_eFormat = eFormat;
	}
	return true;
}

#else

bool ConfigureUnpack ( CSphVariant * pHead, ESphUnpackFormat, CSphSourceParams_SQL &, const char * sSourceName )
{
	if ( pHead )
	{
		fprintf ( stdout, "ERROR: source '%s': unpack is not supported, rebuild with zlib\n", sSourceName );
		return false;
	}
	return true;
}
#endif // USE_ZLIB


bool SqlParamsConfigure ( CSphSourceParams_SQL & tParams, const CSphConfigSection & hSource, const char * sSourceName )
{
	LOC_CHECK ( hSource, "sql_host", "in source '%s'", sSourceName );
	LOC_CHECK ( hSource, "sql_user", "in source '%s'", sSourceName );
	LOC_CHECK ( hSource, "sql_pass", "in source '%s'", sSourceName );
	LOC_CHECK ( hSource, "sql_db", "in source '%s'", sSourceName );
	LOC_CHECK ( hSource, "sql_query", "in source '%s'", sSourceName );

	LOC_GETS ( tParams.m_sHost,				"sql_host" );
	LOC_GETS ( tParams.m_sUser,				"sql_user" );
	LOC_GETS ( tParams.m_sPass,				"sql_pass" );
	LOC_GETS ( tParams.m_sDB,				"sql_db" );
	LOC_GETI ( tParams.m_iPort,				"sql_port");

	LOC_GETS ( tParams.m_sQuery,			"sql_query" );
	LOC_GETAS( tParams.m_dQueryPre,			"sql_query_pre" );
	LOC_GETAS( tParams.m_dQueryPost,		"sql_query_post" );
	LOC_GETS ( tParams.m_sQueryRange,		"sql_query_range" );
	LOC_GETAS( tParams.m_dQueryPostIndex,	"sql_query_post_index" );
	LOC_GETI ( tParams.m_iRangeStep,		"sql_range_step" );
	LOC_GETS ( tParams.m_sQueryKilllist,	"sql_query_killlist" );

	LOC_GETI ( tParams.m_iRangedThrottle,	"sql_ranged_throttle" );

	SqlAttrsConfigure ( tParams,	hSource("sql_group_column"),		SPH_ATTR_INTEGER,	sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_date_column"),			SPH_ATTR_TIMESTAMP,	sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_str2ordinal_column"),	SPH_ATTR_ORDINAL,	sSourceName );

	SqlAttrsConfigure ( tParams,	hSource("sql_attr_uint"),			SPH_ATTR_INTEGER,	sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_attr_timestamp"),		SPH_ATTR_TIMESTAMP,	sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_attr_str2ordinal"),	SPH_ATTR_ORDINAL,	sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_attr_bool"),			SPH_ATTR_BOOL,		sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_attr_float"),			SPH_ATTR_FLOAT,		sSourceName );
	SqlAttrsConfigure ( tParams,	hSource("sql_attr_bigint"),			SPH_ATTR_BIGINT,	sSourceName );

	// unpack
	if ( !ConfigureUnpack ( hSource("unpack_zlib"), SPH_UNPACK_ZLIB, tParams, sSourceName ) )
		return false;

	if ( !ConfigureUnpack ( hSource("unpack_mysqlcompress"), SPH_UNPACK_MYSQL_COMPRESS, tParams, sSourceName ) )
		return false;

	tParams.m_uUnpackMemoryLimit = hSource.GetSize ( "unpack_mysqlcompress_maxsize", 16777216 );

	// parse multi-attrs
	for ( CSphVariant * pVal = hSource("sql_attr_multi"); pVal; pVal = pVal->m_pNext )
	{
		CSphColumnInfo tAttr;
		if ( !ParseMultiAttr ( pVal->cstr(), tAttr, sSourceName ) )
			return false;
		tParams.m_dAttrs.Add ( tAttr );
	}

	// additional checks
	if ( tParams.m_iRangedThrottle<0 )
	{
		fprintf ( stdout, "WARNING: sql_ranged_throttle must not be negative; throttling disabled\n" );
		tParams.m_iRangedThrottle = 0;
	}

	return true;
}


#if USE_PGSQL
CSphSource * SpawnSourcePgSQL ( const CSphConfigSection & hSource, const char * sSourceName )
{
	assert ( hSource["type"]=="pgsql" );

	CSphSourceParams_PgSQL tParams;
	if ( !SqlParamsConfigure ( tParams, hSource, sSourceName ) )
		return NULL;

	LOC_GETS ( tParams.m_sClientEncoding,	"sql_client_encoding" );

	CSphSource_PgSQL * pSrcPgSQL = new CSphSource_PgSQL ( sSourceName );
	if ( !pSrcPgSQL->Setup ( tParams ) )
		SafeDelete ( pSrcPgSQL );

	return pSrcPgSQL;
}
#endif // USE_PGSQL


#if USE_MYSQL
CSphSource * SpawnSourceMySQL ( const CSphConfigSection & hSource, const char * sSourceName )
{
	assert ( hSource["type"]=="mysql" );

	CSphSourceParams_MySQL tParams;
	if ( !SqlParamsConfigure ( tParams, hSource, sSourceName ) )
		return NULL;

	LOC_GETS ( tParams.m_sUsock,			"sql_sock" );
	LOC_GETI ( tParams.m_iFlags,			"mysql_connect_flags" );
	LOC_GETS ( tParams.m_sSslKey,			"mysql_ssl_key" );
	LOC_GETS ( tParams.m_sSslCert,			"mysql_ssl_cert" );
	LOC_GETS ( tParams.m_sSslCA,			"mysql_ssl_ca" );

	CSphSource_MySQL * pSrcMySQL = new CSphSource_MySQL ( sSourceName );
	if ( !pSrcMySQL->Setup ( tParams ) )
		SafeDelete ( pSrcMySQL );

	return pSrcMySQL;
}
#endif // USE_MYSQL


#if USE_ODBC
CSphSource * SpawnSourceODBC ( const CSphConfigSection & hSource, const char * sSourceName )
{
	assert ( hSource["type"]=="odbc" );

	CSphSourceParams_ODBC tParams;
	if ( !SqlParamsConfigure ( tParams, hSource, sSourceName ) )
		return NULL;

	LOC_GETS ( tParams.m_sOdbcDSN, "odbc_dsn" );

	CSphSource_ODBC * pSrc = new CSphSource_ODBC ( sSourceName );
	if ( !pSrc->Setup ( tParams ) )
		SafeDelete ( pSrc );

	return pSrc;
}


CSphSource * SpawnSourceMSSQL ( const CSphConfigSection & hSource, const char * sSourceName )
{
	assert ( hSource["type"]=="mssql" );

	CSphSourceParams_ODBC tParams;
	if ( !SqlParamsConfigure ( tParams, hSource, sSourceName ) )
		return NULL;

	LOC_GETB ( tParams.m_bWinAuth, "mssql_winauth" );
	LOC_GETB ( tParams.m_bUnicode, "mssql_unicode" );

	CSphSource_MSSQL * pSrc = new CSphSource_MSSQL ( sSourceName );
	if ( !pSrc->Setup ( tParams ) )
		SafeDelete ( pSrc );

	return pSrc;
}
#endif // USE_ODBC


CSphSource * SpawnSourceXMLPipe ( const CSphConfigSection & hSource, const char * sSourceName, bool bUTF8 )
{
	assert ( hSource["type"]=="xmlpipe" || hSource["type"]=="xmlpipe2" );

	LOC_CHECK ( hSource, "xmlpipe_command", "in source '%s'.", sSourceName );

	CSphSource * pSrcXML = NULL;

	CSphString sCommand = hSource["xmlpipe_command"];
	const int MAX_BUF_SIZE = 1024;
	BYTE dBuffer [MAX_BUF_SIZE];
	int iBufSize = 0;
	bool bUsePipe2 = true;

	FILE * pPipe = sphDetectXMLPipe ( sCommand.cstr (), dBuffer, iBufSize, MAX_BUF_SIZE, bUsePipe2 );
	if ( !pPipe )
	{
		fprintf ( stdout, "ERROR: xmlpipe: failed to popen '%s'", sCommand.cstr() );
		return NULL;
	}

	if ( bUsePipe2 )
	{
#if USE_LIBEXPAT || USE_LIBXML
		pSrcXML = sphCreateSourceXmlpipe2 ( &hSource, pPipe, dBuffer, iBufSize, sSourceName, g_iMaxXmlpipe2Field );

		if ( !bUTF8 )
		{
			SafeDelete ( pSrcXML );
			fprintf ( stdout, "ERROR: source '%s': xmlpipe2 should only be used with charset_type=utf-8\n", sSourceName );
		}
#else
		fprintf ( stdout, "WARNING: source '%s': xmlpipe2 support NOT compiled in. To use xmlpipe2, install missing XML libraries, reconfigure, and rebuild Sphinx\n", sSourceName );
#endif
	}
	else
	{
		CSphSource_XMLPipe * pXmlPipe = new CSphSource_XMLPipe ( dBuffer, iBufSize, sSourceName );
		if ( !pXmlPipe->Setup ( pPipe, sCommand.cstr () ) )
			SafeDelete ( pXmlPipe );

		pSrcXML = pXmlPipe;
	}

	return pSrcXML;
}


CSphSource * SpawnSource ( const CSphConfigSection & hSource, const char * sSourceName, bool bUTF8 )
{
	if ( !hSource.Exists ( "type" ) )
	{
		fprintf ( stdout, "ERROR: source '%s': type not found; skipping.\n", sSourceName );
		return NULL;
	}

	#if USE_PGSQL
	if ( hSource["type"]=="pgsql")
		return SpawnSourcePgSQL ( hSource, sSourceName );
	#endif

	#if USE_MYSQL
	if ( hSource["type"]=="mysql")
		return SpawnSourceMySQL ( hSource, sSourceName );
	#endif

	#if USE_PYTHON
		if ( hSource["type"]=="python")
			return SpawnSourcePython ( hSource, sSourceName );
	#endif

	#if USE_ODBC
	if ( hSource["type"]=="odbc")
		return SpawnSourceODBC ( hSource, sSourceName );

	if ( hSource["type"]=="mssql")
		return SpawnSourceMSSQL ( hSource, sSourceName );
	#endif

	if ( hSource["type"]=="xmlpipe" || hSource["type"]=="xmlpipe2" )
		return SpawnSourceXMLPipe ( hSource, sSourceName, bUTF8 );

	fprintf ( stdout, "ERROR: source '%s': unknown type '%s'; skipping.\n", sSourceName,
		hSource["type"].cstr() );
	return NULL;
}

#undef LOC_CHECK
#undef LOC_GETS
#undef LOC_GETI
#undef LOC_GETAS
#undef LOC_GETAA

//////////////////////////////////////////////////////////////////////////
// INDEXING
//////////////////////////////////////////////////////////////////////////

bool DoIndex ( const CSphConfigSection & hIndex, const char * sIndexName, const CSphConfigType & hSources )
{
	if ( hIndex("type") && hIndex["type"]=="distributed" )
	{
		if ( !g_bQuiet )
		{
			fprintf ( stdout, "distributed index '%s' can not be directly indexed; skipping.\n", sIndexName );
			fflush ( stdout );
		}
		return false;
	}


	if ( !g_bQuiet )
	{
		fprintf ( stdout, "indexing index '%s'...\n", sIndexName );
		fflush ( stdout );
	}

	// check config
	if ( !hIndex("path") )
	{
		fprintf ( stdout, "ERROR: index '%s': key 'path' not found.\n", sIndexName );
		return false;
	}

	if ( ( hIndex.GetInt ( "min_prefix_len", 0 ) > 0 || hIndex.GetInt ( "min_infix_len", 0 ) > 0 )
		&& hIndex.GetInt ( "enable_star" ) == 0 )
	{
		const char * szMorph = hIndex.GetStr ( "morphology", "" );
		if ( szMorph && *szMorph && strcmp ( szMorph, "none" ) )
		{
			fprintf ( stdout, "ERROR: index '%s': infixes and morphology are enabled, enable_star=0\n", sIndexName );
			return false;
		}
	}

	///////////////////
	// spawn tokenizer
	///////////////////

	CSphString sError;
	CSphTokenizerSettings tTokSettings;
	if ( !sphConfTokenizer ( hIndex, tTokSettings, sError ) )
		sphDie ( "index '%s': %s", sIndexName, sError.cstr() );

	ISphTokenizer * pTokenizer = ISphTokenizer::Create ( tTokSettings, sError );
	if ( !pTokenizer )
		sphDie ( "index '%s': %s", sIndexName, sError.cstr() );

	CSphDict * pDict = NULL;
	CSphDictSettings tDictSettings;

	if ( !g_sBuildStops )
	{
		ISphTokenizer * pTokenFilter = NULL;

		sphConfDictionary ( hIndex, tDictSettings );
		pDict = sphCreateDictionaryCRC ( tDictSettings, pTokenizer, sError );
		if ( !pDict )
			sphDie ( "index '%s': %s", sIndexName, sError.cstr() );

		if ( !sError.IsEmpty () )
			fprintf ( stdout, "WARNING: index '%s': %s\n", sIndexName, sError.cstr() );

		pTokenFilter = ISphTokenizer::CreateTokenFilter ( pTokenizer, pDict->GetMultiWordforms () );
		pTokenizer = pTokenFilter ? pTokenFilter : pTokenizer;
	}

	// prefix/infix indexing
	int iPrefix = hIndex("min_prefix_len") ? hIndex["min_prefix_len"].intval() : 0;
	int iInfix = hIndex("min_infix_len") ? hIndex["min_infix_len"].intval() : 0;
	iPrefix = Max ( iPrefix, 0 );
	iInfix = Max ( iInfix, 0 );

	CSphString sPrefixFields, sInfixFields;

	if ( hIndex.Exists ( "prefix_fields" ) )
		sPrefixFields = hIndex ["prefix_fields"].cstr ();

	if ( hIndex.Exists ( "infix_fields" ) )
		sInfixFields = hIndex ["infix_fields"].cstr ();

	if ( iPrefix == 0 && !sPrefixFields.IsEmpty () )
		fprintf ( stdout, "WARNING: min_prefix_len = 0. prefix_fields are ignored\n" );

	if ( iInfix == 0 && !sInfixFields.IsEmpty () )
		fprintf ( stdout, "WARNING: min_infix_len = 0. infix_fields are ignored\n" );

	// boundary
	bool bInplaceEnable	= hIndex.GetInt ( "inplace_enable", 0 ) != 0;
	int iHitGap			= hIndex.GetSize ( "inplace_hit_gap", 0 );
	int iDocinfoGap		= hIndex.GetSize ( "inplace_docinfo_gap", 0 );
	float fRelocFactor	= hIndex.GetFloat ( "inplace_reloc_factor", 0.1f );
	float fWriteFactor	= hIndex.GetFloat ( "inplace_write_factor", 0.1f );

	if ( bInplaceEnable )
	{
		if ( fRelocFactor < 0.01f || fRelocFactor > 0.9f )
		{
			fprintf ( stdout, "WARNING: inplace_reloc_factor must be 0.01 to 0.9, clamped\n" );
			fRelocFactor = Min ( Max ( fRelocFactor, 0.01f ), 0.9f );
		}

		if ( fWriteFactor < 0.01f || fWriteFactor > 0.9f )
		{
			fprintf ( stdout, "WARNING: inplace_write_factor must be 0.01 to 0.9, clamped\n" );
			fWriteFactor = Min ( Max ( fWriteFactor, 0.01f ), 0.9f );
		}

		if ( fWriteFactor+fRelocFactor > 1.0f )
		{
			fprintf ( stdout, "WARNING: inplace_write_factor+inplace_reloc_factor must be less than 0.9, scaled\n" );
			float fScale = 0.9f/(fWriteFactor+fRelocFactor);
			fRelocFactor *= fScale;
			fWriteFactor *= fScale;
		}
	}

	/////////////////////
	// spawn datasources
	/////////////////////

	// check for per-index HTML stipping override
	bool bStripOverride = false;

	bool bHtmlStrip = false;
	CSphString sHtmlIndexAttrs, sHtmlRemoveElements;

	if ( hIndex("html_strip") )
	{
		bStripOverride = true;
		bHtmlStrip = hIndex.GetInt ( "html_strip" )!=0;
		sHtmlIndexAttrs = hIndex.GetStr ( "html_index_attrs" );
		sHtmlRemoveElements = hIndex.GetStr ( "html_remove_elements" );
	}

	// parse all sources
	CSphVector<CSphSource*> dSources;
	bool bGotAttrs = false;
	bool bSpawnFailed = false;

	for ( CSphVariant * pSourceName = hIndex("source"); pSourceName; pSourceName = pSourceName->m_pNext )
	{
		if ( !hSources ( pSourceName->cstr() ) )
		{
			fprintf ( stdout, "ERROR: index '%s': source '%s' not found.\n", sIndexName, pSourceName->cstr() );
			continue;
		}
		const CSphConfigSection & hSource = hSources [ pSourceName->cstr() ];

		CSphSource * pSource = SpawnSource ( hSource, pSourceName->cstr(), pTokenizer->IsUtf8 () );
		if ( !pSource )
		{
			bSpawnFailed = true;
			continue;
		}

		if ( pSource->HasAttrsConfigured() )
			bGotAttrs = true;

		pSource->SetupFieldMatch ( sPrefixFields.cstr (), sInfixFields.cstr () );

		// strip_html, index_html_attrs
		CSphString sError;
		if ( bStripOverride )
		{
			// apply per-index overrides
			if ( bHtmlStrip )
			{
				if ( !pSource->SetStripHTML ( sHtmlIndexAttrs.cstr(), sHtmlRemoveElements.cstr(), sError ) )
				{
					fprintf ( stdout, "ERROR: source '%s': %s.\n", pSourceName->cstr(), sError.cstr() );
					return false;
				}
			}

		} else if ( hSource.GetInt ( "strip_html" ) )
		{
			// apply deprecated per-source settings if there are no overrides
			if ( !pSource->SetStripHTML ( hSource.GetStr ( "index_html_attrs" ), "", sError ) )
			{
				fprintf ( stdout, "ERROR: source '%s': %s.\n", pSourceName->cstr(), sError.cstr() );
				return false;
			}
		}

		pSource->SetTokenizer ( pTokenizer );
		dSources.Add ( pSource );
	}

	if ( bSpawnFailed )
	{
		fprintf ( stdout, "ERROR: index '%s': failed to configure some of the sources, will not index.\n", sIndexName );
		return false;
	}

	if ( !dSources.GetLength() )
	{
		fprintf ( stdout, "ERROR: index '%s': no valid sources configured; skipping.\n", sIndexName );
		return false;
	}

	///////////
	// do work
	///////////

	int64_t tmTime = sphMicroTimer();
	bool bOK = false;

	if ( g_sBuildStops )
	{
		///////////////////
		// build stopwords
		///////////////////

		if ( !g_bQuiet )
		{
			fprintf ( stdout, "building stopwords list...\n" );
			fflush ( stdout );
		}

		CSphStopwordBuilderDict tDict;
		ARRAY_FOREACH ( i, dSources )
		{
			CSphString sError;
			dSources[i]->SetDict ( &tDict );
			if ( !dSources[i]->Connect ( sError ) || !dSources[i]->IterateHitsStart ( sError ) )
				continue;
			while ( dSources[i]->IterateHitsNext ( sError ) && dSources[i]->m_tDocInfo.m_iDocID );
		}
		tDict.Save ( g_sBuildStops, g_iTopStops, g_bBuildFreqs );

		SafeDelete ( pTokenizer );
	}
	else
	{
		//////////
		// index!
		//////////

		// if searchd is running, we want to reindex to .tmp files
		CSphString sIndexPath;
		sIndexPath.SetSprintf ( g_bRotate ? "%s.tmp" : "%s", hIndex["path"].cstr() );

		// do index
		CSphIndex * pIndex = sphCreateIndexPhrase ( sIndexPath.cstr() );
		assert ( pIndex );

		// check lock file
		if ( !pIndex->Lock() )
		{
			fprintf ( stdout, "FATAL: %s, will not index. Try --rotate option.\n", pIndex->GetLastError().cstr() );
			exit ( 1 );
		}

		CSphIndexSettings tSettings;
		sphConfIndex ( hIndex, tSettings );

		if ( tSettings.m_bIndexExactWords && !tDictSettings.HasMorphology () )
		{
			tSettings.m_bIndexExactWords = false;
			fprintf ( stdout, "WARNING: index '%s': no morphology, index_exact_words=1 has no effect, ignoring\n", sIndexName );
		}

		if ( bGotAttrs && tSettings.m_eDocinfo==SPH_DOCINFO_NONE )
		{
			fprintf ( stdout, "FATAL: index '%s': got attributes, but docinfo is 'none' (fix your config file).\n", sIndexName );
			exit ( 1 );
		}

		pIndex->SetProgressCallback ( ShowProgress );
		if ( bInplaceEnable )
			pIndex->SetInplaceSettings ( iHitGap, iDocinfoGap, fRelocFactor, fWriteFactor );

		pIndex->SetTokenizer ( pTokenizer );
		pIndex->SetDictionary ( pDict );
		pIndex->Setup ( tSettings );

		bOK = pIndex->Build ( dSources, g_iMemLimit, g_iWriteBuffer )!=0;
		if ( bOK && g_bRotate )
		{
			sIndexPath.SetSprintf ( "%s.new", hIndex["path"].cstr() );
			bOK = pIndex->Rename ( sIndexPath.cstr() );
		}

		if ( !bOK )
			fprintf ( stdout, "ERROR: index '%s': %s.\n", sIndexName, pIndex->GetLastError().cstr() );

		pIndex->Unlock ();

		SafeDelete ( pIndex );
	}

	// trip report
	tmTime = sphMicroTimer() - tmTime;
	if ( !g_bQuiet )
	{
		tmTime = Max ( tmTime, 1 );
		int64_t iTotalDocs = 0;
		int64_t iTotalBytes = 0;

		ARRAY_FOREACH ( i, dSources )
		{
			const CSphSourceStats & tSource = dSources[i]->GetStats();
			iTotalDocs += tSource.m_iTotalDocuments;
			iTotalBytes += tSource.m_iTotalBytes;
		}

		fprintf ( stdout, "total %d docs, %"PRIi64" bytes\n", (int)iTotalDocs, iTotalBytes );

		fprintf ( stdout, "total %d.%03d sec, %d bytes/sec, %d.%02d docs/sec\n",
			int(tmTime/1000000), int(tmTime%1000000)/1000, // sec
			int(iTotalBytes*1000000/tmTime), // bytes/sec
			int(iTotalDocs*1000000/tmTime), int(iTotalDocs*1000000*100/tmTime)%100 ); // docs/sec
	}

	// cleanup and go on
	ARRAY_FOREACH ( i, dSources )
		SafeDelete ( dSources[i] );

	return bOK;
}

//////////////////////////////////////////////////////////////////////////
// MERGING
//////////////////////////////////////////////////////////////////////////

bool DoMerge ( const CSphConfigSection & hDst, const char * sDst,
	const CSphConfigSection & hSrc, const char * sSrc, CSphVector<CSphFilterSettings> & tPurge, bool bRotate, bool bMergeKillLists )
{
	// check config
	if ( !hDst("path") )
	{
		fprintf ( stdout, "ERROR: index '%s': key 'path' not found.\n", sDst );
		return false;
	}
	if ( !hSrc("path") )
	{
		fprintf ( stdout, "ERROR: index '%s': key 'path' not found.\n", sSrc );
		return false;
	}

	// do the merge
	CSphIndex * pSrc = sphCreateIndexPhrase ( hSrc["path"].cstr() );
	CSphIndex * pDst = sphCreateIndexPhrase ( hDst["path"].cstr() );
	assert ( pSrc );
	assert ( pDst );

	CSphString sError;
	if ( !sphFixupIndexSettings ( pSrc, hSrc, sError ) )
	{
		fprintf ( stdout, "ERROR: index '%s': %s\n", sSrc, sError.cstr () );
		return false;
	}

	if ( !sphFixupIndexSettings ( pDst, hDst, sError ) )
	{
		fprintf ( stdout, "ERROR: index '%s': %s\n", sDst, sError.cstr () );
		return false;
	}

	pSrc->SetWordlistPreload ( hSrc.GetInt ( "ondisk_dict" ) == 0 );
	pDst->SetWordlistPreload ( hDst.GetInt ( "ondisk_dict" ) == 0 );

	if ( !pSrc->Lock() && !bRotate )
	{
		fprintf ( stdout, "ERROR: index '%s' is already locked; lock: %s\n", sSrc, pSrc->GetLastError().cstr() );
		return false;
	}

	if ( !pDst->Lock() && !bRotate )
	{
		fprintf ( stdout, "ERROR: index '%s' is already locked; lock: %s\n", sDst, pDst->GetLastError().cstr() );
		return false;
	}

	pDst->SetProgressCallback ( ShowProgress );

	int64_t tmMergeTime = sphMicroTimer();
	if ( !pDst->Merge ( pSrc, tPurge, bMergeKillLists ) )
		sphDie ( "failed to merge index '%s' into index '%s': %s", sSrc, sDst, pDst->GetLastError().cstr() );
	tmMergeTime = sphMicroTimer() - tmMergeTime;
	if ( !g_bQuiet )
		printf ( "merged in %d.%03d sec\n", int(tmMergeTime/1000000), int(tmMergeTime%1000000)/1000 );

	// pick up merge result
	const char * sPath = hDst["path"].cstr();
	char sFrom [ SPH_MAX_FILENAME_LEN ];
	char sTo [ SPH_MAX_FILENAME_LEN ];
	struct stat tFileInfo;

	int iExt;
	for ( iExt=0; iExt<EXT_COUNT; iExt++ )
	{
		snprintf ( sFrom, sizeof(sFrom), "%s.%s.tmp", sPath, g_dExt[iExt] );
		sFrom [ sizeof(sFrom)-1 ] = '\0';

		if ( g_bRotate )
			snprintf ( sTo, sizeof(sTo), "%s.new.%s", sPath, g_dExt[iExt] );
		else
			snprintf ( sTo, sizeof(sTo), "%s.%s", sPath, g_dExt[iExt] );

		sTo [ sizeof(sTo)-1 ] = '\0';

		if ( !stat( sTo, &tFileInfo ) )
		{
			if ( remove ( sTo ) )
			{
				fprintf ( stdout, "ERROR: index '%s': failed to delete '%s': %s",
					sDst, sTo, strerror(errno) );
				break;
			}
		}

		if ( rename ( sFrom, sTo ) )
		{
			fprintf ( stdout, "ERROR: index '%s': failed to rename '%s' to '%s': %s",
				sDst, sFrom, sTo, strerror(errno) );
			break;
		}
	}

	if ( !bRotate )
	{
		pSrc->Unlock();
		pDst->Unlock();
	}

	SafeDelete ( pSrc );
	SafeDelete ( pDst );

	// all good?
	return ( iExt==EXT_COUNT );
}

//////////////////////////////////////////////////////////////////////////
// ENTRY
//////////////////////////////////////////////////////////////////////////

void ReportIOStats ( const char * sType, int iReads, int64_t iReadTime, int64_t iReadBytes )
{
	if ( iReads==0 )
	{
		fprintf ( stdout, "total %d %s, %d.%03d sec, 0.0 kb/call avg, 0.0 msec/call avg\n",
			iReads, sType,
			int(iReadTime/1000000), int(iReadTime%1000000)/1000 );
	} else
	{
		iReadBytes /= iReads;
		fprintf ( stdout, "total %d %s, %d.%03d sec, %d.%d kb/call avg, %d.%d msec/call avg\n",
			iReads, sType,
			int(iReadTime/1000000), int(iReadTime%1000000)/1000,
			int(iReadBytes/1024), int(iReadBytes%1024)*10/1024,
			int(iReadTime/iReads/1000), int(iReadTime/iReads/100)%10 );
	}
}


int main ( int argc, char ** argv )
{
	const char * sOptConfig = NULL;
	bool bMerge = false;
	CSphVector<CSphFilterSettings> dMergeDstFilters;

	CSphVector<const char *> dIndexes;
	bool bIndexAll = false;
	bool bMergeKillLists = false;

	int i;
	for ( i=1; i<argc; i++ )
	{
		if ( ( !strcmp ( argv[i], "--config" ) || !strcmp ( argv[i], "-c" ) ) && (i+1)<argc )
		{
			sOptConfig = argv[++i];
			if ( !sphIsReadable ( sOptConfig ) )
				sphDie ( "config file '%s' does not exist or is not readable", sOptConfig );
		}
		else if ( strcasecmp ( argv[i], "--merge" )==0 && (i+2)<argc )
		{
			bMerge = true;
			dIndexes.Add ( argv[i+1] );
			dIndexes.Add ( argv[i+2] );
			i += 2;
		}
		else if ( bMerge && strcasecmp ( argv[i], "--merge-dst-range" )==0 && (i+3)<argc )
		{
			dMergeDstFilters.Resize ( dMergeDstFilters.GetLength()+1 );
			dMergeDstFilters.Last().m_eType = SPH_FILTER_RANGE;
			dMergeDstFilters.Last().m_sAttrName = argv[i+1];
			dMergeDstFilters.Last().m_uMinValue = (SphAttr_t) strtoull ( argv[i+2], NULL, 10 );
			dMergeDstFilters.Last().m_uMaxValue = (SphAttr_t) strtoull ( argv[i+3], NULL, 10 );
			i += 3;
		}
		else if ( strcasecmp ( argv[i], "--buildstops" )==0 && (i+2)<argc )
		{
			g_sBuildStops = argv[i+1];
			g_iTopStops = atoi ( argv[i+2] );
			if ( g_iTopStops<=0 )
				break;
			i += 2;

		} else if ( strcasecmp ( argv[i], "--rotate" )==0 )
		{
			g_bRotate = true;

		} else if ( strcasecmp ( argv[i], "--buildfreqs" )==0 )
		{
			g_bBuildFreqs = true;

		} else if ( strcasecmp ( argv[i], "--quiet" )==0 )
		{
			g_bQuiet = true;
			sphSetQuiet ( true );

		} else if ( strcasecmp ( argv[i], "--noprogress" )==0 )
		{
			g_bProgress = false;

		} else if ( strcasecmp ( argv[i], "--all" )==0 )
		{
			bIndexAll = true;

		} else if ( strcasecmp ( argv[i], "--merge-killlists" )==0 )
		{
			bMergeKillLists = true;

		} else if ( sphIsAlpha(argv[i][0]) )
		{
			dIndexes.Add ( argv[i] );

		} else
		{
			break;
		}
	}

	if ( !g_bQuiet )
		fprintf ( stdout, SPHINX_BANNER );

	if ( !isatty ( fileno(stdout) ) )
		g_bProgress = false;

	if ( i!=argc || argc<2 )
	{
		if ( argc>1 )
		{
			fprintf ( stdout, "ERROR: malformed or unknown option near '%s'.\n", argv[i] );

		} else
		{
			fprintf ( stdout,
				"Usage: indexer [OPTIONS] [indexname1 [indexname2 [...]]]\n"
				"\n"
				"Options are:\n"
				"--config <file>\t\tread configuration from specified file\n"
				"\t\t\t(default is csft.conf)\n"
				"--all\t\t\treindex all configured indexes\n"
				"--quiet\t\t\tbe quiet, only print errors\n"
				"--noprogress\t\tdo not display progress\n"
				"\t\t\t(automatically on if output is not to a tty)\n"
#if !USE_WINDOWS
				"--rotate\t\tsend SIGHUP to searchd when indexing is over\n"
				"\t\t\tto rotate updated indexes automatically\n"
#endif
				"--buildstops <output.txt> <N>\n"
				"\t\t\tbuild top N stopwords and write them to given file\n"
				"--buildfreqs\t\tstore words frequencies to output.txt\n"
				"\t\t\t(used with --buildstops only)\n"
				"--merge <dst-index> <src-index>\n"
				"\t\t\tmerge 'src-index' into 'dst-index'\n"
				"\t\t\t'dst-index' will receive merge result\n"
				"\t\t\t'src-index' will not be modified\n"
				"--merge-dst-range <attr> <min> <max>\n"
				"\t\t\tfilter 'dst-index' on merge, keep only those documents\n"
				"\t\t\twhere 'attr' is between 'min' and 'max' (inclusive)\n"
				"--merge-killlists"
				"\t\t\tmerge src and dst killlists instead of applying src killlist to dst"
				"\n"
				"Examples:\n"
				"indexer --quiet myidx1\treindex 'myidx1' defined in 'csft.conf'\n"
				"indexer --all\t\treindex all indexes defined in 'csft.conf'\n" );
		}

		return 1;
	}

	///////////////
	// load config
	///////////////

	CSphConfigParser cp;
	CSphConfig & hConf = cp.m_tConf;
	sOptConfig = sphLoadConfig ( sOptConfig, g_bQuiet, cp );

	if ( !hConf ( "source" ) )
		sphDie ( "no indexes found in config file '%s'", sOptConfig );

	g_iMemLimit = 0;
	if ( hConf("indexer") && hConf["indexer"]("indexer") )
	{
		CSphConfigSection & hIndexer = hConf["indexer"]["indexer"];

		g_iMemLimit = hIndexer.GetSize ( "mem_limit", 0 );
		g_iMaxXmlpipe2Field = hIndexer.GetSize ( "max_xmlpipe2_field", 2*1024*1024 );
		g_iWriteBuffer = hIndexer.GetSize ( "write_buffer", 1024*1024 );

		sphSetThrottling ( hIndexer.GetInt ( "max_iops", 0 ), hIndexer.GetSize ( "max_iosize", 0 ) );
	}

 	/////////////////////
	// init python layer
	////////////////////
	if ( hConf("python") && hConf["python"]("python") )
	{
#if USE_PYTHON
		CSphConfigSection & hPython = hConf["python"]["python"];
		if(!cftInitialize(hPython)) 
			sphDie ( "Python layer's initiation failed.");
#else
		sphDie ( "Python layer defined, but indexer does Not supports python. used --enbale-python to recompile.");
#endif
	}	


	/////////////////////
	// index each index
	////////////////////

	sphStartIOStats ();

	bool bIndexedOk = false; // if any of the indexes are ok
	if ( bMerge )
	{
		if ( dIndexes.GetLength()!=2 )
			sphDie ( "there must be 2 indexes to merge specified" );

		if ( !hConf["index"](dIndexes[0]) )
			sphDie ( "no merge destination index '%s'", dIndexes[0] );

		if ( !hConf["index"](dIndexes[1]) )
			sphDie ( "no merge source index '%s'", dIndexes[1] );

		bIndexedOk = DoMerge (
			hConf["index"][dIndexes[0]], dIndexes[0],
			hConf["index"][dIndexes[1]], dIndexes[1], dMergeDstFilters, g_bRotate, bMergeKillLists );
	} else if ( bIndexAll )
	{
		hConf["index"].IterateStart ();
		while ( hConf["index"].IterateNext() )
			bIndexedOk |= DoIndex ( hConf["index"].IterateGet (), hConf["index"].IterateGetKey().cstr(), hConf["source"] );
	} else
	{
		ARRAY_FOREACH ( i, dIndexes )
		{
			if ( !hConf["index"](dIndexes[i]) )
				fprintf ( stdout, "WARNING: no such index '%s', skipping.\n", dIndexes[i] );
			else
				bIndexedOk |= DoIndex ( hConf["index"][dIndexes[i]], dIndexes[i], hConf["source"] );
		}
	}

	sphShutdownWordforms ();

	const CSphIOStats & tStats = sphStopIOStats ();

	if ( !g_bQuiet )
	{
		ReportIOStats ( "reads", tStats.m_iReadOps, tStats.m_iReadTime, tStats.m_iReadBytes );
		ReportIOStats ( "writes", tStats.m_iWriteOps, tStats.m_iWriteTime, tStats.m_iWriteBytes );
	}

	////////////////////////////
	// rotating searchd indices
	////////////////////////////

	if ( bIndexedOk )
	{
		bool bOK = false;
		while ( g_bRotate )
		{
			// load config
			if ( !hConf.Exists ( "searchd" ) )
			{
				fprintf ( stdout, "WARNING: 'searchd' section not found in config file.\n" );
				break;
			}

			const CSphConfigSection & hSearchd = hConf["searchd"]["searchd"];
			if ( !hSearchd.Exists ( "pid_file" ) )
			{
				fprintf ( stdout, "WARNING: 'pid_file' parameter not found in 'searchd' config section.\n" );
				break;
			}

			// read in PID
			int iPID;
			FILE * fp = fopen ( hSearchd["pid_file"].cstr(), "r" );
			if ( !fp )
			{
				fprintf ( stdout, "WARNING: failed to open pid_file '%s'.\n", hSearchd["pid_file"].cstr() );
				break;
			}
			if ( fscanf ( fp, "%d", &iPID )!=1 || iPID<=0 )
			{
				fprintf ( stdout, "WARNING: failed to scanf pid from pid_file '%s'.\n", hSearchd["pid_file"].cstr() );
				break;
			}
			fclose ( fp );

#if USE_WINDOWS
			char szPipeName [64];
			sprintf ( szPipeName, "\\\\.\\pipe\\searchd_%d", iPID );

			HANDLE hPipe = INVALID_HANDLE_VALUE;

			while ( hPipe == INVALID_HANDLE_VALUE )
			{
				hPipe = CreateFile ( szPipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );

				if ( hPipe == INVALID_HANDLE_VALUE )
				{
					if ( GetLastError () != ERROR_PIPE_BUSY )
					{
						fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
						break;
					}

					if ( !WaitNamedPipe ( szPipeName, 1000 ) )
					{
						fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
						break;
					}
				}
			}

			if ( hPipe != INVALID_HANDLE_VALUE )
			{
				DWORD uWritten = 0;
				BYTE uWrite = 0;
				BOOL bResult = WriteFile ( hPipe, &uWrite, 1, &uWritten, NULL );
				if ( bResult )
					fprintf ( stdout, "rotating indices: succesfully sent SIGHUP to searchd (pid=%d).\n", iPID );
				else
					fprintf ( stdout, "WARNING: failed to send SIGHUP to searchd (pid=%d, GetLastError()=%d)\n", iPID, GetLastError () );

				CloseHandle ( hPipe );
			}
#else
			// signal
			int iErr = kill ( iPID, SIGHUP );
			if ( iErr==0 )
			{
				if ( !g_bQuiet )
					fprintf ( stdout, "rotating indices: succesfully sent SIGHUP to searchd (pid=%d).\n", iPID );
			} else
			{
				switch ( errno )
				{
					case ESRCH:	fprintf ( stdout, "WARNING: no process found by PID %d.\n", iPID ); break;
					case EPERM:	fprintf ( stdout, "WARNING: access denied to PID %d.\n", iPID ); break;
					default:	fprintf ( stdout, "WARNING: kill() error: %s.\n", strerror(errno) ); break;
				}
				break;
			}
#endif

			// all ok
			bOK = true;
			break;
		}

		if ( g_bRotate )
		{
			if ( !bOK )
				fprintf ( stdout, "WARNING: indices NOT rotated.\n" );
		}
	}

#if SPH_DEBUG_LEAKS
	sphAllocsStats ();
#endif
#if USE_PYTHON
	cftShutdown(); //clean up 
#endif	
	return bIndexedOk ? 0 : 1;
}

//
// $Id: indexer.cpp 2096 2009-11-24 06:54:21Z tomat $
//
