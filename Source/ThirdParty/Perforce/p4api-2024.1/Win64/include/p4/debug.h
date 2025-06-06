/*
 * Copyright 1995, 2003 Perforce Software.  All rights reserved.
 *
 * This file is part of Perforce - the FAST SCM System.
 */

// Note: not all debug type below (P4DebugType) uses 
// this debuglevel enum.
enum P4DebugLevel {
	DL_NONE,		// 0 means no debugging output
	DL_ERROR,		// 1
	DL_WARNING,		// 2
	DL_INFO,		// 3
	DL_DETAILED,		// 4
	DL_DEBUG,		// 5
} ;

class StrPtr;
class StrBuf;
class ErrorLog;
class Error;
struct ErrorId;

enum P4DebugType {
	DT_DB,		// DbOpen
	DT_DIFF,	// Diff
	DT_DM,		// Dm
	DT_DMC,		// Dm commands
	DT_FTP,		// Ftp Server
	DT_HANDLE,	// Handles
	DT_LBR,		// Lbr
	DT_MAP,		// MapTable
	DT_NET,		// Net
	DT_OPTIONS,	// Optional behavior
	DT_PEEK,	// Peeking
	DT_RCS,		// RCS
	DT_RECORDS,	// VarRecords
	DT_RPC,		// Rpc
	DT_SERVER,	// Server
	DT_SPEC,	// Spec
	DT_TRACK,	// Track
	DT_OB,		// Offline Broker
	DT_VIEWGEN,     // Streamw view generator
	DT_RPL,		// Distributed functionality related
	DT_SSL,		// SSL related
	DT_TIME,	// Add timestamps to debug output
	DT_CLUSTER,	// Cluster related
	DT_ZK,		// p4zk related
	DT_LDAP,	// LDAP related
	DT_DVCS,	// DVCS related
	DT_GRAPH,	// GRAPH related
	DT_GCONN,	// gconn related
	DT_FOVR,	// Failover related
	DT_SCRIPT,	// scripting support
	DT_STG,         // Tracking storage records.
	DT_THREAD,	// threading
	DT_EXTS,	// exts (extension)
	DT_PROTECT,	// protections stats
	DT_HEARTBEAT,	// Heartbeat related
	DT_SHELVE,	// Shelving related
	DT_SQW,		// StreamQWorker related
	DT_STM,		// Stream materialize for fstat,files,dirs
	DT_PCHECK,	// Parallel checkpoint
	DT_TOPOLOGY,	// Topology
	DT_RESOURCE,	// OS resources
	DT_S3,		// S3 cURL client
	DT_SUPTOOLS,    // Support Tools 
	DT_LAST
}  ;

enum P4TunableType {
	DTT_NONE,	// Unknown tuneable
	DTT_INT,	// Numeric tuneable
	DTT_STR,	// String tuneable
} ;

enum P4TunableApplicability {
	CONFIG_APPLY_NONE = 0x0000,
	CONFIG_APPLY_CLIENT = 0x0001,
	CONFIG_APPLY_SERVER = 0x0002,
	CONFIG_APPLY_PROXY = 0x0004,
	CONFIG_APPLY_BROKER = 0x0008
	// When you add new types, update the string array in userconfig.cc
} ;

enum P4TunableRestart {
	CONFIG_RESTART_NONE,
	CONFIG_RESTART_NO_RESTART,
	CONFIG_RESTART_RESTART,
	CONFIG_RESTART_STOP,
	CONFIG_RESTART_REF_DOC
	// When you add new types, update the string array in userconfig.cc
} ;

enum P4TunableSupport {
	CONFIG_SUPPORT_NONE,
	CONFIG_SUPPORT_NODOC,
	CONFIG_SUPPORT_UNDOC,
	CONFIG_SUPPORT_DOC
	// When you add new types, update the string array in userconfig.cc
} ;

enum P4TunableCategory {
	CONFIG_CAT_NONE		= 0x0000,
	CONFIG_CAT_MISC		= 0x0001,
	CONFIG_CAT_SECURITY	= 0x0002,
	CONFIG_CAT_STREAMS	= 0x0004,
	CONFIG_CAT_REPLICATION	= 0x0008,
	CONFIG_CAT_NETWORK	= 0x0010,
	CONFIG_CAT_PERFORMANCE	= 0x0020,
	CONFIG_CAT_MONITORING	= 0x0040,
	CONFIG_CAT_TRIGGERS	= 0x0080,
	CONFIG_CAT_EXTENSIONS	= 0x0100,
	CONFIG_CAT_LICENSING	= 0x0200,
	CONFIG_CAT_ARCHIVE_MANAGEMENT = 0x0400,
	CONFIG_CAT_DVCS		= 0x0800
	// When you add new types, update the string array in userconfig.cc
} ;

extern P4MT int list2[];

class P4Tunable {

    public:
	struct tunable {
	    const char *name;
	    int isSet;		
	    int value;
	    int minVal;
	    int maxVal;
	    int modVal;
	    int k;		// what's 1k? 1000 or 1024?
	    int original;
	    int sensitive;

	    const ErrorId *description;
	    const char *recVal;	// Recommended value
	    int svr;		// Applicability - server/client/proxy/broker
	    int restart;	// Restart requirement
	    int support;	// Support level
	    int cat;		// Category
	};

	struct stunable {
	    const char *name;
	    int isSet;
	    const char *def;
	    char *value;
	    int sensitive;

	    const ErrorId *description;
	    const char *recVal;	// Recommended value
	    int svr;		// Applicability - server/client/proxy/broker
	    int restart;	// Restart requirement
	    int support;	// Support level
	    int cat;		// Category
	    const char *accepted; // Comma separated accepted values
	};

	void		Set( const char *set );
	void		SetTLocal( const char *set );
	void		Unset( const char *set );
	int		Get( int t ) const {
	    return t < DT_LAST && list2[t] != -1 && list2[t] > list[t].value ?
	        list2[t] : list[t].value;
	}

	const tunable	*GetTunable( int i ) const;

	const stunable	*GetStringTunable( int i ) const;

	int		GetOriginalValue( int t ) const {
	    return list[t].original;
	}
	int		GetLevel( const char *n ) const;
	StrBuf		GetString( const char *n ) const;
	StrBuf		GetString( int t ) const;
	int		GetIndex( const char *n ) const;
	const char	*GetName( int t ) const { return list[t].name; }
	int		IsSet( int t ) const { return list[t].isSet; }
	int		IsSet( const char * n ) const;
	int		IsKnown( const char * n );
	int		IsNumeric( const char * n );
	void		IsValid( const char * n, const char * v, Error *e );
	int		IsSensitive( int t ) const { return list[t].sensitive;}
	void		Unbuffer();
	void		UnsetAll();

	/*
	 * The intended use for this method, which only sets the active
	 * value for a tunable, is to set a specific value that will be used
	 * at a lower layer and the value cannot otherwise be passed into the
	 * lower layer. The need to use this method should be rare.
	 */
	void		SetActive( int t, int v );

    protected:

	static tunable list[];
	static stunable slist[];
} ;

typedef void (*DebugOutputHook)( void *context, const StrPtr *buffer );

class P4DebugConfig {
    public:
	P4DebugConfig();
	virtual ~P4DebugConfig();
	virtual void Output();
	virtual StrBuf *Buffer();
	virtual int Alloc( int );
	virtual P4DebugConfig *Clone();
	void Install();
	void SetErrorLog( ErrorLog *e ) { elog = e; }
	void SetOutputHook( void *ctx, DebugOutputHook hk )
		{ hook = hk; context = ctx; }

	static void TsPid2StrBuf( StrBuf &prefix );
	static P4DebugConfig *ThreadClone();

    protected:
	StrBuf *buf;
	int msz;
	ErrorLog *elog;
	DebugOutputHook hook;
	void		*context;
	int cloned;
};

class P4Debug : private P4Tunable {

    public:

	void		SetLevel( int l );
	void		SetLevel( const char *set );
	void		SetLevel( P4DebugType t, int l ) { list[t].value = l ;}

	int		GetLevel( P4DebugType t ) const { return Get(t); }

	int		IsSet( P4DebugType t ) const { return P4Tunable::IsSet( t ); }

	void		ShowLevels( int showAll, StrBuf &buf );

	void		Event();

	void		printf( const char *fmt, ... );

};

/*
 * DEBUGPRINT and DEBUGPRINTF are generic debug macros.
 * These macros simply check to see if the passed condition
 * is true and if so prints out the message. The latter macro
 * takes arguments.
 *
 * It is expected that the underlying sub-project will
 * construct macros that that encapsulate the comparison
 * of their area's debug flag against specific levels:
 * e.g. # define DEBUG_SVR_ERROR ( p4debug.GetLevel( DT_SERVER ) >= 1 )
 *      # define DEBUG_SVR_WARN	 ( p4debug.GetLevel( DT_SERVER ) >= 2 )
 *      # define DEBUG_SVR_INFO  ( p4debug.GetLevel( DT_SERVER ) >= 4 )
 */
# define DEBUGPRINT(level, msg) \
	do \
	{ \
	    if( level ) \
		p4debug.printf( msg "\n" ); \
	} while(0);

# define DEBUGPRINTF( level, msg, ... ) \
	do \
	{ \
	    if( level ) \
		p4debug.printf(  msg "\n", __VA_ARGS__ ); \
	} while(0);

extern P4Debug p4debug;
extern P4Tunable p4tunable;
