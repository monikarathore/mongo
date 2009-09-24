// engine_spidermonkey.cpp

#include "stdafx.h"
#include "engine_spidermonkey.h"

#include "../client/dbclient.h"

#ifndef _WIN32
#include <boost/date_time/posix_time/posix_time.hpp>
#endif

namespace mongo {

    string trim( string s ){
        while ( s.size() && isspace( s[0] ) )
            s = s.substr( 1 );
        
        while ( s.size() && isspace( s[s.size()-1] ) )
            s = s.substr( 0 , s.size() - 1 );
        
        return s;
    }

    boost::thread_specific_ptr<SMScope> currentScope( dontDeleteScope );
    boost::recursive_mutex smmutex;
#define smlock recursive_boostlock ___lk( smmutex );

#define GETHOLDER(x,o) ((BSONHolder*)JS_GetPrivate( x , o ))

    class BSONFieldIterator;

    class BSONHolder {
    public:

        BSONHolder( BSONObj obj ){
            _obj = obj.getOwned();
            _inResolve = false;
            _modified = false;
            _magic = 17;
        }
        
        ~BSONHolder(){
            _magic = 18;
        }

        void check(){
            uassert( "holder magic value is wrong" , _magic == 17 && _obj.isValid() );
        }

        BSONFieldIterator * it();

        BSONObj _obj;
        bool _inResolve;
        char _magic;
        list<string> _extra;
        set<string> _removed;
        bool _modified;
    };
    
    class BSONFieldIterator {
    public:

        BSONFieldIterator( BSONHolder * holder ){

            set<string> added;

            BSONObjIterator it( holder->_obj );
            while ( it.more() ){
                BSONElement e = it.next();
                if ( holder->_removed.count( e.fieldName() ) )
                    continue;
                _names.push_back( e.fieldName() );
                added.insert( e.fieldName() );
            }
            
            for ( list<string>::iterator i = holder->_extra.begin(); i != holder->_extra.end(); i++ ){
                if ( ! added.count( *i ) )
                    _names.push_back( *i );
            }

            _it = _names.begin();
        }

        bool more(){
            return _it != _names.end();
        }

        string next(){
            string s = *_it;
            _it++;
            return s;
        }

    private:
        list<string> _names;
        list<string>::iterator _it;
    };

    BSONFieldIterator * BSONHolder::it(){
        return new BSONFieldIterator( this );
    }


    class Convertor : boost::noncopyable {
    public:
        Convertor( JSContext * cx ){
            _context = cx;
        }

        string toString( JSString * so ){
            jschar * s = JS_GetStringChars( so );
            size_t srclen = JS_GetStringLength( so );
            if( srclen == 0 )
                return "";

            size_t len = srclen * 6; // we only need *3, but see note on len below
            char * dst = (char*)malloc( len );

            len /= 2;
            // doc re weird JS_EncodeCharacters api claims len expected in 16bit
            // units, but experiments suggest 8bit units expected.  We allocate
            // enough memory that either will work.

            assert( JS_EncodeCharacters( _context , s , srclen , dst , &len) );

            string ss( dst , len );
            free( dst );
            if ( !JS_CStringsAreUTF8() )
                for( string::const_iterator i = ss.begin(); i != ss.end(); ++i )
                    uassert( "non ascii character detected", (unsigned char)(*i) <= 127 );
            return ss;
        }

        string toString( jsval v ){
            return toString( JS_ValueToString( _context , v ) );
        }

        double toNumber( jsval v ){
            double d;
            uassert( "not a number" , JS_ValueToNumber( _context , v , &d ) );
            return d;
        }

        bool toBoolean( jsval v ){
            JSBool b;
            assert( JS_ValueToBoolean( _context, v , &b ) );
            return b;
        }

        OID toOID( jsval v ){
            JSContext * cx = _context;
            assert( JSVAL_IS_OID( v ) );

            JSObject * o = JSVAL_TO_OBJECT( v );
            OID oid;
            oid.init( getString( o , "str" ) );
            return oid;
        }

        BSONObj toObject( JSObject * o ){
            if ( ! o )
                return BSONObj();

            if ( JS_InstanceOf( _context , o , &bson_ro_class , 0 ) ){
                return GETHOLDER( _context , o )->_obj.getOwned();
            }

            BSONObj orig;
            if ( JS_InstanceOf( _context , o , &bson_class , 0 ) ){
                BSONHolder * holder = GETHOLDER(_context,o);
                if ( ! holder->_modified ){
                    return holder->_obj;
                }
                orig = holder->_obj;
            }

            BSONObjBuilder b;

            if ( ! appendSpecialDBObject( this , b , "value" , OBJECT_TO_JSVAL( o ) , o ) ){

                jsval theid = getProperty( o , "_id" );
                if ( ! JSVAL_IS_VOID( theid ) ){
                    append( b , "_id" , theid );
                }
                
                JSIdArray * properties = JS_Enumerate( _context , o );
                assert( properties );
                
                for ( jsint i=0; i<properties->length; i++ ){
                    jsid id = properties->vector[i];
                    jsval nameval;
                    assert( JS_IdToValue( _context ,id , &nameval ) );
                    string name = toString( nameval );
                    if ( name == "_id" )
                        continue;
                    
                    append( b , name , getProperty( o , name.c_str() ) , orig[name].type() );
                }

                JS_DestroyIdArray( _context , properties );
            }

            return b.obj();
        }

        BSONObj toObject( jsval v ){
            if ( JSVAL_IS_NULL( v ) ||
                 JSVAL_IS_VOID( v ) )
                return BSONObj();

            uassert( "not an object" , JSVAL_IS_OBJECT( v ) );
            return toObject( JSVAL_TO_OBJECT( v ) );
        }

        string getFunctionCode( JSFunction * func ){
            return toString( JS_DecompileFunction( _context , func , 0 ) );
        }

        string getFunctionCode( jsval v ){
            uassert( "not a function" , JS_TypeOfValue( _context , v ) == JSTYPE_FUNCTION );
            return getFunctionCode( JS_ValueToFunction( _context , v ) );
        }
        
        void appendRegex( BSONObjBuilder& b , const string& name , string s ){
            assert( s[0] == '/' );
            s = s.substr(1);
            string::size_type end = s.rfind( '/' );
            b.appendRegex( name.c_str() , s.substr( 0 , end ).c_str() , s.substr( end + 1 ).c_str() );
        }

        void append( BSONObjBuilder& b , string name , jsval val , BSONType oldType = EOO  ){
            //cout << "name: " << name << "\t" << typeString( val ) << " oldType: " << oldType << endl;
            switch ( JS_TypeOfValue( _context , val ) ){

            case JSTYPE_VOID: b.appendUndefined( name.c_str() ); break;
            case JSTYPE_NULL: b.appendNull( name.c_str() ); break;

            case JSTYPE_NUMBER: {
                double d = toNumber( val );
                if ( oldType == NumberInt && ((int)d) == d )
                    b.append( name.c_str() , (int)d );
                else
                    b.append( name.c_str() , d );
                break;
            }
            case JSTYPE_STRING: b.append( name.c_str() , toString( val ) ); break;
            case JSTYPE_BOOLEAN: b.appendBool( name.c_str() , toBoolean( val ) ); break;

            case JSTYPE_OBJECT: {
                JSObject * o = JSVAL_TO_OBJECT( val );
                if ( ! o || o == JSVAL_NULL ){
                    b.appendNull( name.c_str() );
                }
                else if ( ! appendSpecialDBObject( this , b , name , val , o ) ){
                    BSONObj sub = toObject( o );
                    if ( JS_IsArrayObject( _context , o ) ){
                        b.appendArray( name.c_str() , sub );
                    }
                    else {
                        b.append( name.c_str() , sub );
                    }
                }
                break;
            }

            case JSTYPE_FUNCTION: {
                string s = toString(val);
                if ( s[0] == '/' ){
                    appendRegex( b , name , s );
                }
                else {
                    b.appendCode( name.c_str() , getFunctionCode( val ).c_str() );
                }
                break;
            }

            default: uassert( (string)"can't append field.  name:" + name + " type: " + typeString( val ) , 0 );
            }
        }

        // ---------- to spider monkey ---------

        bool hasFunctionIdentifier( const string& code ){
            if ( code.size() < 9 || code.find( "function" ) != 0  )
                return false;

            return code[8] == ' ' || code[8] == '(';
        }

        bool isSimpleStatement( const string& code ){
            if ( code.find( "return" ) != string::npos )
                return false;

            if ( code.find( ";" ) != string::npos &&
                 code.find( ";" ) != code.rfind( ";" ) )
                return false;

            if ( code.find( "for(" ) != string::npos ||
                 code.find( "for (" ) != string::npos ||
                 code.find( "while (" ) != string::npos ||
                 code.find( "while(" ) != string::npos )
                return false;

            return true;
        }

        void addRoot( JSFunction * f , const char * name );

        JSFunction * compileFunction( const char * code, JSObject * assoc = 0 ){
            const char * gcName = "unknown";
            JSFunction * f = _compileFunction( code , assoc , gcName );
            //addRoot( f , gcName );
            return f;
        }

        JSFunction * _compileFunction( const char * raw , JSObject * assoc , const char *& gcName ){
            if ( ! assoc )
                assoc = JS_GetGlobalObject( _context );

            while (isspace(*raw)) {
                raw++;
            }

            stringstream fname;
            fname << "cf_";
            static int fnum = 1;
            fname << "_" << fnum++ << "_";


            if ( ! hasFunctionIdentifier( raw ) ){
                string s = raw;
                if ( isSimpleStatement( s ) ){
                    s = "return " + s;
                }
                gcName = "cf anon";
                fname << "anon";
                return JS_CompileFunction( _context , assoc , fname.str().c_str() , 0 , 0 , s.c_str() , strlen( s.c_str() ) , "nofile_a" , 0 );
            }

            string code = raw;
            
            size_t start = code.find( '(' );
            assert( start != string::npos );
            
            fname << "_f_" << trim( code.substr( 9 , start - 9 ) );

            code = code.substr( start + 1 );
            size_t end = code.find( ')' );
            assert( end != string::npos );
            
            string paramString = trim( code.substr( 0 , end ) );
            code = code.substr( end + 1 );
            
            vector<string> params;
            while ( paramString.size() ){
                size_t c = paramString.find( ',' );
                if ( c == string::npos ){
                    params.push_back( paramString );
                    break;
                }
                params.push_back( trim( paramString.substr( 0 , c ) ) );
                paramString = trim( paramString.substr( c + 1 ) );
                paramString = trim( paramString );
            }
            
            const char ** paramArray = new const char*[params.size()];
            for ( size_t i=0; i<params.size(); i++ )
                paramArray[i] = params[i].c_str();
            
            JSFunction * func = JS_CompileFunction( _context , assoc , fname.str().c_str() , params.size() , paramArray , code.c_str() , strlen( code.c_str() ) , "nofile_b" , 0 );
            delete paramArray;
            if ( ! func ){
                cerr << "compile failed for: " << raw << endl;
                return 0;
            }
            gcName = "cf normal";
            return func;
        }


        jsval toval( double d ){
            jsval val;
            assert( JS_NewNumberValue( _context, d , &val ) );
            return val;
        }

        jsval toval( const char * c ){
            JSString * s = JS_NewStringCopyZ( _context , c );
            if ( s )
                return STRING_TO_JSVAL( s );
            
            // possibly unicode, try manual
            
            size_t len = strlen( c );
            size_t dstlen = len * 4;
            jschar * dst = (jschar*)malloc( dstlen );
            
            JSBool res = JS_DecodeBytes( _context , c , len , dst, &dstlen );
            if ( res ){
                s = JS_NewUCStringCopyN( _context , dst , dstlen );
            }

            free( dst );

            if ( ! res ){
                cerr << "decode failed. probably invalid utf-8 string [" << c << "]" << endl;
                jsval v;
                if ( JS_GetPendingException( _context , &v ) )
                    cout << "\t why: " << toString( v ) << endl;
                throw UserException( "invalid utf8" );
            }

            assert( s );
            return STRING_TO_JSVAL( s );
        }

        JSObject * toJSObject( const BSONObj * obj , bool readOnly=false ){
            JSObject * o = JS_NewObject( _context , readOnly ? &bson_ro_class : &bson_class , NULL, NULL);
            assert( o );
            assert( JS_SetPrivate( _context , o , (void*)(new BSONHolder( obj->getOwned() ) ) ) );
            return o;
        }

        jsval toval( const BSONObj* obj , bool readOnly=false ){
            JSObject * o = toJSObject( obj , readOnly );
            return OBJECT_TO_JSVAL( o );
        }

        jsval toval( const BSONElement& e ){

            switch( e.type() ){
            case EOO:
            case jstNULL:
            case Undefined:
                return JSVAL_NULL;
            case NumberDouble:
            case NumberInt:
            case NumberLong:
                return toval( e.number() );
            case Symbol: // TODO: should we make a special class for this
            case String:
                return toval( e.valuestr() );
            case Bool:
                return e.boolean() ? JSVAL_TRUE : JSVAL_FALSE;
            case Object:{
                BSONObj embed = e.embeddedObject().getOwned();
                return toval( &embed );
            }
            case Array:{

                BSONObj embed = e.embeddedObject().getOwned();

                if ( embed.isEmpty() ){
                    return OBJECT_TO_JSVAL( JS_NewArrayObject( _context , 0 , 0 ) );
                }

                int n = embed.nFields();
                assert( n > 0 );

                JSObject * array = JS_NewArrayObject( _context , embed.nFields() , 0 );
                assert( array );

                jsval myarray = OBJECT_TO_JSVAL( array );

                for ( int i=0; i<n; i++ ){
                    jsval v = toval( embed[i] );
                    assert( JS_SetElement( _context , array , i , &v ) );
                }

                return myarray;
            }
            case jstOID:{
                OID oid = e.__oid();
                JSObject * o = JS_NewObject( _context , &object_id_class , 0 , 0 );
                setProperty( o , "str" , toval( oid.str().c_str() ) );
                return OBJECT_TO_JSVAL( o );
            }
            case RegEx:{
                const char * flags = e.regexFlags();
                uintN flagNumber = 0;
                while ( *flags ){
                    switch ( *flags ){
                    case 'g': flagNumber |= JSREG_GLOB; break;
                    case 'i': flagNumber |= JSREG_FOLD; break;
                    case 'm': flagNumber |= JSREG_MULTILINE; break;
                        //case 'y': flagNumber |= JSREG_STICKY; break;
                        
                    default: 
                        log() << "warning: unknown regex flag:" << *flags << endl;
                    }
                    flags++;
                }

                JSObject * r = JS_NewRegExpObject( _context , (char*)e.regex() , strlen( e.regex() ) , flagNumber );
                assert( r );
                return OBJECT_TO_JSVAL( r );
            }
            case Code:{
                JSFunction * func = compileFunction( e.valuestr() );
                return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
            }
            case CodeWScope:{
                JSFunction * func = compileFunction( e.codeWScopeCode() );

                BSONObj extraScope = e.codeWScopeObject();
                if ( ! extraScope.isEmpty() ){
                    log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
                }

                return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
            }
            case Date:
                return OBJECT_TO_JSVAL( js_NewDateObjectMsec( _context , (jsdouble) e.date() ) );

            case MinKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &minkey_class , 0 , 0 ) );

            case MaxKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &maxkey_class , 0 , 0 ) );

            case Timestamp: {
                JSObject * o = JS_NewObject( _context , &timestamp_class , 0 , 0 );
                setProperty( o , "t" , toval( (double)(e.timestampTime()) ) );
                setProperty( o , "i" , toval( (double)(e.timestampInc()) ) );
                return OBJECT_TO_JSVAL( o );
            }

            case DBRef: {
                JSObject * o = JS_NewObject( _context , &dbref_class , 0 , 0 );
                setProperty( o , "ns" , toval( e.dbrefNS() ) );

                JSObject * oid = JS_NewObject( _context , &object_id_class , 0 , 0 );
                setProperty( oid , "str" , toval( e.dbrefOID().str().c_str() ) );

                setProperty( o , "id" , OBJECT_TO_JSVAL( oid ) );
                return OBJECT_TO_JSVAL( o );
            }
            case BinData:{
                JSObject * o = JS_NewObject( _context , &bindata_class , 0 , 0 );
                int len;
                void * data = (void*)e.binData( len );
                assert( JS_SetPrivate( _context , o , data ) );

                setProperty( o , "len" , toval( len ) );
                setProperty( o , "type" , toval( (int)e.binDataType() ) );
                return OBJECT_TO_JSVAL( o );
            }
            }

            cout << "toval: unknown type: " << e.type() << endl;
            uassert( "not done: toval" , 0 );
            return 0;
        }

        // ------- object helpers ------

        JSObject * getJSObject( JSObject * o , const char * name ){
            jsval v;
            assert( JS_GetProperty( _context , o , name , &v ) );
            return JSVAL_TO_OBJECT( v );
        }

        JSObject * getGlobalObject( const char * name ){
            return getJSObject( JS_GetGlobalObject( _context ) , name );
        }

        JSObject * getGlobalPrototype( const char * name ){
            return getJSObject( getGlobalObject( name ) , "prototype" );
        }

        bool hasProperty( JSObject * o , const char * name ){
            JSBool res;
            assert( JS_HasProperty( _context , o , name , & res ) );
            return res;
        }

        jsval getProperty( JSObject * o , const char * field ){
            uassert( "object passed to getPropery is null" , o );
            jsval v;
            assert( JS_GetProperty( _context , o , field , &v ) );
            return v;
        }

        void setProperty( JSObject * o , const char * field , jsval v ){
            assert( JS_SetProperty( _context , o , field , &v ) );
        }

        string typeString( jsval v ){
            JSType t = JS_TypeOfValue( _context , v );
            return JS_GetTypeName( _context , t );
        }

        bool getBoolean( JSObject * o , const char * field ){
            return toBoolean( getProperty( o , field ) );
        }

        double getNumber( JSObject * o , const char * field ){
            return toNumber( getProperty( o , field ) );
        }

        string getString( JSObject * o , const char * field ){
            return toString( getProperty( o , field ) );
        }

        JSClass * getClass( JSObject * o , const char * field ){
            jsval v;
            assert( JS_GetProperty( _context , o , field , &v ) );
            if ( ! JSVAL_IS_OBJECT( v ) )
                return 0;
            return JS_GET_CLASS( _context , JSVAL_TO_OBJECT( v ) );
        }

        JSContext * _context;


    };


    void bson_finalize( JSContext * cx , JSObject * obj ){
        BSONHolder * o = GETHOLDER( cx , obj );
        if ( o ){
            delete o;
            assert( JS_SetPrivate( cx , obj , 0 ) );
        }
    }

    JSBool bson_enumerate( JSContext *cx, JSObject *obj, JSIterateOp enum_op, jsval *statep, jsid *idp ){

        if ( enum_op == JSENUMERATE_INIT ){
            BSONFieldIterator * it = GETHOLDER( cx , obj )->it();
            *statep = PRIVATE_TO_JSVAL( it );
            if ( idp )
                *idp = JSVAL_ZERO;
            return JS_TRUE;
        }

        BSONFieldIterator * it = (BSONFieldIterator*)JSVAL_TO_PRIVATE( *statep );

        if ( enum_op == JSENUMERATE_NEXT ){
            if ( it->more() ){
                string name = it->next();
                Convertor c(cx);
                assert( JS_ValueToId( cx , c.toval( name.c_str() ) , idp ) );
            }
            else {
                delete it;
                *statep = 0;
            }
            return JS_TRUE;
        }

        if ( enum_op == JSENUMERATE_DESTROY ){
            if ( it )
                delete it;
            return JS_TRUE;
        }

        uassert( "don't know what to do with this op" , 0 );
        return JS_FALSE;
    }

    JSBool noaccess( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( holder->_inResolve )
            return JS_TRUE;
        JS_ReportError( cx , "doing write op on read only operation" );
        return JS_FALSE;
    }

    JSClass bson_ro_class = {
        "bson_ro_object" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE ,
        noaccess, noaccess, JS_PropertyStub, noaccess,
        (JSEnumerateOp)bson_enumerate, (JSResolveOp)(&resolveBSONField) , JS_ConvertStub, bson_finalize ,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };
    
    JSBool bson_add_prop( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( ! holder->_inResolve ){
            Convertor c(cx);
            string name = c.toString( idval );
            if ( holder->_obj[name].eoo() ){
                holder->_extra.push_back( name );
            }
            holder->_modified = true;
        }
        return JS_TRUE;
    }
    

    JSBool mark_modified( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        Convertor c(cx);
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( holder->_inResolve )
            return JS_TRUE;
        holder->_modified = true;
        holder->_removed.erase( c.toString( idval ) );
        return JS_TRUE;
    }
    
    JSBool mark_modified_remove( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        Convertor c(cx);
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( holder->_inResolve )
            return JS_TRUE;
        holder->_modified = true;
        holder->_removed.insert( c.toString( idval ) );
        return JS_TRUE;
    }

    JSClass bson_class = {
        "bson_object" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE ,
        bson_add_prop, mark_modified_remove, JS_PropertyStub, mark_modified,
        (JSEnumerateOp)bson_enumerate, (JSResolveOp)(&resolveBSONField) , JS_ConvertStub, bson_finalize ,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    static JSClass global_class = {
        "global", JSCLASS_GLOBAL_FLAGS,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    // --- global helpers ---

    JSBool native_print( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );
        for ( uintN i=0; i<argc; i++ ){
            if ( i > 0 )
                cout << " ";
            cout << c.toString( argv[i] );
        }
        cout << endl;
        return JS_TRUE;
    }

    JSBool native_helper( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        Convertor c(cx);
        uassert( "native_helper needs at least 1 arg" , argc >= 1 );
        
        NativeFunction func = (NativeFunction)((long long)c.toNumber( argv[0] ));
        assert( func );
        
        BSONObjBuilder args;
        for ( uintN i=1; i<argc; i++ ){
            c.append( args , args.numStr( i ) , argv[i] );
        }
        
        BSONObj a = args.obj();
        BSONObj out = func( a );
        
        if ( out.isEmpty() ){
            *rval = JSVAL_VOID;
        }
        else {
            *rval = c.toval( out.firstElement() );
        }

        return JS_TRUE;
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval );

    JSBool native_gc( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        JS_GC( cx );
        return JS_TRUE;
    }

    JSFunctionSpec globalHelpers[] = {
        { "print" , &native_print , 0 , 0 , 0 } ,
        { "nativeHelper" , &native_helper , 1 , 0 , 0 } ,
        { "load" , &native_load , 1 , 0 , 0 } ,
        { "gc" , &native_gc , 1 , 0 , 0 } ,

        { 0 , 0 , 0 , 0 , 0 }
    };

    // ----END global helpers ----


    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
        assert( JS_EnterLocalRootScope( cx ) );
        Convertor c( cx );

        BSONHolder * holder = GETHOLDER( cx , obj );
        assert( holder );
        holder->check();
        
        string s = c.toString( id );

        BSONElement e = holder->_obj[ s.c_str() ];

        if ( e.type() == EOO || holder->_removed.count( s ) ){
            *objp = 0;
            JS_LeaveLocalRootScope( cx );
            return JS_TRUE;
        }

        jsval val = c.toval( e );

        assert( ! holder->_inResolve );
        holder->_inResolve = true;
        assert( JS_SetProperty( cx , obj , s.c_str() , &val ) );
        holder->_inResolve = false;
        
        if ( val != JSVAL_NULL && val != JSVAL_VOID && JSVAL_IS_OBJECT( val ) ){
            // TODO: this is a hack to get around sub objects being modified
            JSObject * oo = JSVAL_TO_OBJECT( val );
            if ( JS_InstanceOf( cx , oo , &bson_class , 0 ) || 
                 JS_IsArrayObject( cx , oo ) ){
                holder->_modified = true;
            }
        }

        *objp = obj;
        JS_LeaveLocalRootScope( cx );
        return JS_TRUE;
    }


    class SMScope;

    class SMEngine : public ScriptEngine {
    public:

        SMEngine(){
#ifdef SM18
            JS_SetCStringsAreUTF8();
#endif

            _runtime = JS_NewRuntime(8L * 1024L * 1024L);
            uassert( "JS_NewRuntime failed" , _runtime );
            
            if ( ! utf8Ok() ){
                log() << "*** warning: spider monkey build without utf8 support.  consider rebuilding with utf8 support" << endl;
            }

            int x = 0;
            assert( x = 1 );
            if ( x != 1 )
                throw -1;
        }

        ~SMEngine(){
            JS_DestroyRuntime( _runtime );
            JS_ShutDown();
        }

        Scope * createScope();

        void runTest();

        virtual bool utf8Ok() const { return JS_CStringsAreUTF8(); }

#ifdef XULRUNNER
        JSClass * _dateClass;
        JSClass * _regexClass;
#endif


    private:
        JSRuntime * _runtime;
        friend class SMScope;
    };

    SMEngine * globalSMEngine;


    void ScriptEngine::setup(){
        globalSMEngine = new SMEngine();
        globalScriptEngine = globalSMEngine;
    }


    // ------ special helpers -------

    JSBool object_keyset(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){

        JSIdArray * properties = JS_Enumerate( cx , obj );
        assert( properties );

        JSObject * array = JS_NewArrayObject( cx , properties->length , 0 );
        assert( array );

        for ( jsint i=0; i<properties->length; i++ ){
            jsid id = properties->vector[i];
            jsval idval;
            assert( JS_IdToValue( cx , id , &idval ) );
            assert( JS_SetElement( cx , array , i ,  &idval ) );
        }

        JS_DestroyIdArray( cx , properties );

        *rval = OBJECT_TO_JSVAL( array );
        return JS_TRUE;
    }

    // ------ scope ------


    JSBool no_gc(JSContext *cx, JSGCStatus status){
        return JS_FALSE;
    }

    JSBool yes_gc(JSContext *cx, JSGCStatus status){
        return JS_TRUE;
    }

    class SMScope : public Scope {
    public:
        SMScope() : _this( 0 ) , _externalSetup( false ) , _localConnect( false ) {
            smlock;
            _context = JS_NewContext( globalSMEngine->_runtime , 8192 );
            _convertor = new Convertor( _context );
            massert( "JS_NewContext failed" , _context );

            JS_SetOptions( _context , JSOPTION_VAROBJFIX);
            //JS_SetVersion( _context , JSVERSION_LATEST); TODO
            JS_SetErrorReporter( _context , errorReporter );

            _global = JS_NewObject( _context , &global_class, NULL, NULL);
            massert( "JS_NewObject failed for global" , _global );
            JS_SetGlobalObject( _context , _global );
            massert( "js init failed" , JS_InitStandardClasses( _context , _global ) );

            JS_SetOptions( _context , JS_GetOptions( _context ) | JSOPTION_VAROBJFIX );

            JS_DefineFunctions( _context , _global , globalHelpers );

            // install my special helpers

            assert( JS_DefineFunction( _context , _convertor->getGlobalPrototype( "Object" ) ,
                                       "keySet" , object_keyset , 0 , JSPROP_READONLY ) );

            //JS_SetGCCallback( _context , no_gc ); // this is useful for seeing if something is a gc problem

            _postCreateHacks();
        }
        
        ~SMScope(){
            smlock;
            uassert( "deleted SMScope twice?" , _convertor );

            for ( list<void*>::iterator i=_roots.begin(); i != _roots.end(); i++ ){
                JS_RemoveRoot( _context , *i );
            }
            _roots.clear();
            
            if ( _this ){
                JS_RemoveRoot( _context , &_this );
                _this = 0;
            }

            if ( _convertor ){
                delete _convertor;
                _convertor = 0;
            }
            
            if ( _context ){
                JS_DestroyContext( _context );
                _context = 0;
            }

        }
        
        void reset(){
            smlock;
            assert( _convertor );
            return;
            if ( _this ){
                JS_RemoveRoot( _context , &_this );
                _this = 0;
            }
            currentScope.reset( this );
            _error = "";
        }
        
        void addRoot( void * root , const char * name ){
            JS_AddNamedRoot( _context , root , name );
            _roots.push_back( root );
        }

        void init( BSONObj * data ){
            smlock;
            if ( ! data )
                return;

            BSONObjIterator i( *data );
            while ( i.more() ){
                BSONElement e = i.next();
                _convertor->setProperty( _global , e.fieldName() , _convertor->toval( e ) );
                _initFieldNames.insert( e.fieldName() );
            }

        }

        void externalSetup(){
            smlock;
            uassert( "already local connected" , ! _localConnect );
            if ( _externalSetup )
                return;
            initMongoJS( this , _context , _global , false );
            _externalSetup = true;
        }

        void localConnect( const char * dbName ){
            smlock;
            uassert( "already setup for external db" , ! _externalSetup );
            if ( _localConnect ){
                uassert( "connected to different db" , _dbName == dbName );
                return;
            }
            
            initMongoJS( this , _context , _global , true );
            
            exec( "_mongo = new Mongo();" );
            exec( ((string)"db = _mongo.getDB( \"" + dbName + "\" ); ").c_str() );
            
            _localConnect = true;
            _dbName = dbName;
        }

        // ----- getters ------
        double getNumber( const char *field ){
            smlock;
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            return _convertor->toNumber( val );
        }

        string getString( const char *field ){
            smlock;
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            JSString * s = JS_ValueToString( _context , val );
            return _convertor->toString( s );
        }

        bool getBoolean( const char *field ){
            smlock;
            return _convertor->getBoolean( _global , field );
        }

        BSONObj getObject( const char *field ){
            smlock;
            return _convertor->toObject( _convertor->getProperty( _global , field ) );
        }

        JSObject * getJSObject( const char * field ){
            smlock;
            return _convertor->getJSObject( _global , field );
        }

        int type( const char *field ){
            smlock;
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );

            switch ( JS_TypeOfValue( _context , val ) ){
            case JSTYPE_VOID: return Undefined;
            case JSTYPE_NULL: return jstNULL;
            case JSTYPE_OBJECT: {
                if ( val == JSVAL_NULL )
                    return jstNULL;
                JSObject * o = JSVAL_TO_OBJECT( val );
                if ( JS_IsArrayObject( _context , o ) )
                    return Array;
                if ( isDate( _context , o ) )
                    return Date;
                return Object;
            }
            case JSTYPE_FUNCTION: return Code;
            case JSTYPE_STRING: return String;
            case JSTYPE_NUMBER: return NumberDouble;
            case JSTYPE_BOOLEAN: return Bool;
            default:
                uassert( "unknown type" , 0 );
            }
            return 0;
        }

        // ----- setters ------

        void setNumber( const char *field , double val ){
            smlock;
            jsval v = _convertor->toval( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );
        }

        void setString( const char *field , const char * val ){
            smlock;
            jsval v = _convertor->toval( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );
        }

        void setObject( const char *field , const BSONObj& obj , bool readOnly ){
            smlock;
            jsval v = _convertor->toval( &obj , readOnly );
            JS_SetProperty( _context , _global , field , &v );
        }

        void setBoolean( const char *field , bool val ){
            smlock;
            jsval v = BOOLEAN_TO_JSVAL( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );
        }

        void setThis( const BSONObj * obj ){
            smlock;
            if ( _this ){
                JS_RemoveRoot( _context , &_this );
                _this = 0;
            }
            
            if ( obj ){
                _this = _convertor->toJSObject( obj );
                JS_AddNamedRoot( _context , &_this , "scope this" );
            }
        }

        // ---- functions -----

        ScriptingFunction createFunction( const char * code ){
            smlock;
            precall();
            return (ScriptingFunction)_convertor->compileFunction( code );
        }

        struct TimeoutSpec {
            boost::posix_time::ptime start;
            boost::posix_time::time_duration timeout;
            int count;
        };

        static JSBool _checkTimeout( JSContext *cx ){
            TimeoutSpec &spec = *(TimeoutSpec *)( JS_GetContextPrivate( cx ) );
            if ( ++spec.count % 1000 != 0 )
                return JS_TRUE;
            boost::posix_time::time_duration elapsed = ( boost::posix_time::microsec_clock::local_time() - spec.start );
            if ( elapsed < spec.timeout ) {
                return JS_TRUE;
            }
            JS_ReportError( cx, "Timeout exceeded" );
            return JS_FALSE;

        }
        static JSBool checkTimeout( JSContext *cx, JSScript *script ){
            return _checkTimeout( cx );
        }


        void installCheckTimeout( int timeoutMs ) {
            if ( timeoutMs > 0 ) {
                TimeoutSpec *spec = new TimeoutSpec;
                spec->timeout = boost::posix_time::millisec( timeoutMs );
                spec->start = boost::posix_time::microsec_clock::local_time();
                spec->count = 0;
                JS_SetContextPrivate( _context, (void*)spec );
#ifdef SM181
                JS_SetOperationCallback( _context, _checkTimeout );
#else
                JS_SetBranchCallback( _context, checkTimeout );
#endif
            }
        }

        void uninstallCheckTimeout( int timeoutMs ) {
            if ( timeoutMs > 0 ) {
#ifdef SM181
                JS_SetOperationCallback( _context , 0 );
#else
                JS_SetBranchCallback( _context, 0 );
#endif
                delete (TimeoutSpec *)JS_GetContextPrivate( _context );
                JS_SetContextPrivate( _context, 0 );
            }
        }

        void precall(){
            _error = "";
            currentScope.reset( this );
        }

        bool exec( const string& code , const string& name = "(anon)" , bool printResult = false , bool reportError = true , bool assertOnError = true, int timeoutMs = 0 ){
            smlock;
            precall();

            jsval ret = JSVAL_VOID;

            installCheckTimeout( timeoutMs );
            JSBool worked = JS_EvaluateScript( _context , _global , code.c_str() , strlen( code.c_str() ) , name.c_str() , 0 , &ret );
            uninstallCheckTimeout( timeoutMs );

            if ( assertOnError )
                uassert( name + " exec failed" , worked );

            if ( reportError && ! _error.empty() ){
                // cout << "exec error: " << _error << endl;
                // already printed in reportError, so... TODO
            }

            if ( worked )
                _convertor->setProperty( _global , "__lastres__" , ret );

            if ( worked && printResult && ! JSVAL_IS_VOID( ret ) )
                cout << _convertor->toString( ret ) << endl;

            return worked;
        }
        
        int invoke( JSFunction * func , const BSONObj& args, int timeoutMs , bool ignoreReturn ){
            smlock;
            precall();

            assert( JS_EnterLocalRootScope( _context ) );
                
            int nargs = args.nFields();
            auto_ptr<jsval> smargsPtr( new jsval[nargs] );
            jsval* smargs = smargsPtr.get();
            if ( nargs ){
                BSONObjIterator it( args );
                for ( int i=0; i<nargs; i++ ){
                    smargs[i] = _convertor->toval( it.next() );
                }
            }

            if ( args.isEmpty() ){
                _convertor->setProperty( _global , "args" , JSVAL_NULL );
            }
            else {
                setObject( "args" , args , true ); // this is for backwards compatability
            }

            JS_LeaveLocalRootScope( _context );

            installCheckTimeout( timeoutMs );
            jsval rval;
            JSBool ret = JS_CallFunction( _context , _this ? _this : _global , func , nargs , smargs , &rval );
            uninstallCheckTimeout( timeoutMs );

            if ( !ret ) {
                return -3;
            }
            
            if ( ! ignoreReturn ){
                assert( JS_SetProperty( _context , _global , "return" , &rval ) );
            }

            return 0;
        }

        int invoke( ScriptingFunction funcAddr , const BSONObj& args, int timeoutMs = 0 , bool ignoreReturn = 0 ){
            return invoke( (JSFunction*)funcAddr , args , timeoutMs , ignoreReturn );
        }

        void gotError( string s ){
            _error = s;
        }

        string getError(){
            return _error;
        }

        void injectNative( const char *field, NativeFunction func ){
            smlock;
            string name = field;
            _convertor->setProperty( _global , (name + "_").c_str() , _convertor->toval( (double)(long long)func ) );

            stringstream code;
            code << field << " = function(){ var a = [ " << field << "_ ]; for ( var i=0; i<arguments.length; i++ ){ a.push( arguments[i] ); } return nativeHelper.apply( null , a ); }";
            exec( code.str().c_str() );

        }

        virtual void gc(){
            JS_GC( _context );
        }

        JSContext *context() const { return _context; }
        
    private:

        void _postCreateHacks(){
#ifdef XULRUNNER
            exec( "__x__ = new Date(1);" );
            globalSMEngine->_dateClass = _convertor->getClass( _global , "__x__" );
            exec( "__x__ = /abc/i" );
            globalSMEngine->_regexClass = _convertor->getClass( _global , "__x__" );
#endif
        }
        
        JSContext * _context;
        Convertor * _convertor;

        JSObject * _global;
        JSObject * _this;

        string _error;
        list<void*> _roots;

        bool _externalSetup;
        bool _localConnect;
        string _dbName;
        
        set<string> _initFieldNames;
        
    };

    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report ){
        stringstream ss;
        ss << "JS Error: " << message;

        if ( report ){
            ss << " " << report->filename << ":" << report->lineno;
        }

        log() << ss.str() << endl;

        if ( currentScope.get() ){
            currentScope->gotError( ss.str() );
        }
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        Convertor c(cx);

        Scope * s = currentScope.get();

        for ( uintN i=0; i<argc; i++ ){
            string filename = c.toString( argv[i] );
            cout << "should load [" << filename << "]" << endl;

            if ( ! s->execFile( filename , false , true , false ) ){
                JS_ReportError( cx , ((string)"error loading file: " + filename ).c_str() );
                return JS_FALSE;
            }
        }

        return JS_TRUE;
    }



    void SMEngine::runTest(){
        SMScope s;

        s.localConnect( "foo" );

        s.exec( "assert( db.getMongo() )" );
        s.exec( "assert( db.bar , 'collection getting does not work' ); " );
        s.exec( "assert.eq( db._name , 'foo' );" );
        s.exec( "assert( _mongo == db.getMongo() ); " );
        s.exec( "assert( _mongo == db._mongo ); " );
        s.exec( "assert( typeof DB.bar == 'undefined' ); " );
        s.exec( "assert( typeof DB.prototype.bar == 'undefined' , 'resolution is happening on prototype, not object' ); " );

        s.exec( "assert( db.bar ); " );
        s.exec( "assert( typeof db.addUser == 'function' )" );
        s.exec( "assert( db.addUser == DB.prototype.addUser )" );
        s.exec( "assert.eq( 'foo.bar' , db.bar._fullName ); " );
        s.exec( "db.bar.verify();" );

        s.exec( "db.bar.silly.verify();" );
        s.exec( "assert.eq( 'foo.bar.silly' , db.bar.silly._fullName )" );
        s.exec( "assert.eq( 'function' , typeof _mongo.find , 'mongo.find is not a function' )" );

        assert( (string)"abc" == trim( "abc" ) );
        assert( (string)"abc" == trim( " abc" ) );
        assert( (string)"abc" == trim( "abc " ) );
        assert( (string)"abc" == trim( " abc " ) );

    }

    Scope * SMEngine::createScope(){
        return new SMScope();
    }

    void Convertor::addRoot( JSFunction * f , const char * name ){
        if ( ! f )
            return;

        SMScope * scope = currentScope.get();
        uassert( "need a scope" , scope );
        
        JSObject * o = JS_GetFunctionObject( f );
        assert( o );
        scope->addRoot( &o , name );
    }

}

#include "sm_db.cpp"
