/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include "sax/fastparser.hxx"
#include "sax/fastattribs.hxx"
#include "xml2utf.hxx"

#include <com/sun/star/lang/DisposedException.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/xml/sax/FastToken.hpp>
#include <com/sun/star/xml/sax/SAXParseException.hpp>
#include <com/sun/star/xml/sax/XFastContextHandler.hpp>
#include <com/sun/star/xml/sax/XFastDocumentHandler.hpp>
#include <com/sun/star/xml/sax/XFastTokenHandler.hpp>
#include <cppuhelper/supportsservice.hxx>
#include <osl/conditn.hxx>
#include <osl/diagnose.h>
#include <rtl/ref.hxx>
#include <rtl/ustrbuf.hxx>
#include <salhelper/thread.hxx>

#include <boost/optional.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <stack>
#include <vector>
#include <queue>
#include <cassert>
#include <cstring>
#include <libxml/parser.h>

// Inverse of libxml's BAD_CAST.
#define XML_CAST( str ) reinterpret_cast< const sal_Char* >( str )

using namespace ::std;
using namespace ::osl;
using namespace ::cppu;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::lang;
using namespace ::com::sun::star::xml::sax;
using namespace ::com::sun::star::io;
using namespace com::sun::star;
using namespace sax_fastparser;

namespace {

struct Event;
class FastLocatorImpl;
struct NamespaceDefine;
struct Entity;

typedef ::boost::shared_ptr< NamespaceDefine > NamespaceDefineRef;

typedef ::boost::unordered_map< OUString, sal_Int32,
        OUStringHash, ::std::equal_to< OUString > > NamespaceMap;

typedef std::vector<Event> EventList;

enum CallbackType { INVALID, START_ELEMENT, END_ELEMENT, CHARACTERS, DONE, EXCEPTION };

struct Event
{
    CallbackType maType;
    sal_Int32 mnElementToken;
    OUString msNamespace;
    OUString msElementName;
    rtl::Reference< FastAttributeList > mxAttributes;
    OUString msChars;
};

struct NameWithToken
{
    OUString msName;
    sal_Int32 mnToken;

    NameWithToken(const OUString& sName, const sal_Int32& nToken) :
        msName(sName), mnToken(nToken) {}
};

struct SaxContext
{
    Reference< XFastContextHandler > mxContext;
    sal_Int32 mnElementToken;
    OUString  maNamespace;
    OUString  maElementName;

    SaxContext( sal_Int32 nElementToken, const OUString& aNamespace, const OUString& aElementName ):
            mnElementToken(nElementToken)
    {
        if (nElementToken == FastToken::DONTKNOW)
        {
            maNamespace = aNamespace;
            maElementName = aElementName;
        }
    }
};


struct ParserData
{
    ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XFastDocumentHandler > mxDocumentHandler;
    ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XFastTokenHandler >    mxTokenHandler;
    FastTokenHandlerBase *mpTokenHandler;
    ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XErrorHandler >        mxErrorHandler;
    ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XEntityResolver >      mxEntityResolver;
    ::com::sun::star::lang::Locale          maLocale;

    ParserData();
    ~ParserData();
};

struct NamespaceDefine
{
    OString     maPrefix;
    sal_Int32   mnToken;
    OUString    maNamespaceURL;

    NamespaceDefine( const OString& rPrefix, sal_Int32 nToken, const OUString& rNamespaceURL ) : maPrefix( rPrefix ), mnToken( nToken ), maNamespaceURL( rNamespaceURL ) {}
};

// Entity binds all information needed for a single file | single call of parseStream
struct Entity : public ParserData
{
    // Amount of work producer sends to consumer in one iteration:
    static const size_t mnEventListSize = 1000;

    // unique for each Entity instance:

    // Number of valid events in mpProducedEvents:
    size_t mnProducedEventsSize;
    EventList *mpProducedEvents;
    std::queue< EventList * > maPendingEvents;
    std::queue< EventList * > maUsedEvents;
    osl::Mutex maEventProtector;

    static const size_t mnEventLowWater = 4;
    static const size_t mnEventHighWater = 8;
    osl::Condition maConsumeResume;
    osl::Condition maProduceResume;
    // Event we use to store data if threading is disabled:
    Event maSharedEvent;

    // copied in copy constructor:

    // Allow to disable threading for small documents:
    bool                                    mbEnableThreads;
    ::com::sun::star::xml::sax::InputSource maStructSource;
    xmlParserCtxtPtr                        mpParser;
    ::sax_expatwrap::XMLFile2UTFConverter   maConverter;

    // Exceptions cannot be thrown through the C-XmlParser (possible
    // resource leaks), therefore any exception thrown by a UNO callback
    // must be saved somewhere until the C-XmlParser is stopped.
    ::com::sun::star::uno::Any maSavedException;
    void saveException( const Exception &e );
    void throwException( const ::rtl::Reference< FastLocatorImpl > &xDocumentLocator,
                         bool mbDuringParse );

    ::std::stack< NameWithToken >           maNamespaceStack;
    /* Context for main thread consuming events.
     * startElement() stores the data, which characters() and endElement() uses
     */
    ::std::stack< SaxContext>               maContextStack;
    // Determines which elements of maNamespaceDefines are valid in current context
    ::std::stack< sal_uInt32 >              maNamespaceCount;
    ::std::vector< NamespaceDefineRef >     maNamespaceDefines;

    explicit Entity( const ParserData& rData );
    Entity( const Entity& rEntity );
    ~Entity();
    void startElement( Event *pEvent );
    void characters( const OUString& sChars );
    void endElement();
    EventList* getEventList();
    Event& getEvent( CallbackType aType );
};

} // namespace

namespace sax_fastparser {

class FastSaxParserImpl
{
public:
    FastSaxParserImpl( FastSaxParser* pFront );
    ~FastSaxParserImpl();

    // XFastParser
    void parseStream( const ::com::sun::star::xml::sax::InputSource& aInputSource ) throw (::com::sun::star::xml::sax::SAXException, ::com::sun::star::io::IOException, ::com::sun::star::uno::RuntimeException, std::exception);
    void setFastDocumentHandler( const ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XFastDocumentHandler >& Handler ) throw (::com::sun::star::uno::RuntimeException);
    void setTokenHandler( const ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XFastTokenHandler >& Handler ) throw (::com::sun::star::uno::RuntimeException);
    void registerNamespace( const OUString& NamespaceURL, sal_Int32 NamespaceToken ) throw (::com::sun::star::lang::IllegalArgumentException, ::com::sun::star::uno::RuntimeException);
    OUString getNamespaceURL( const OUString& rPrefix ) throw(::com::sun::star::lang::IllegalArgumentException, ::com::sun::star::uno::RuntimeException);
    void setErrorHandler( const ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XErrorHandler >& Handler ) throw (::com::sun::star::uno::RuntimeException);
    void setEntityResolver( const ::com::sun::star::uno::Reference< ::com::sun::star::xml::sax::XEntityResolver >& Resolver ) throw (::com::sun::star::uno::RuntimeException);
    void setLocale( const ::com::sun::star::lang::Locale& rLocale ) throw (::com::sun::star::uno::RuntimeException);

    // called by the C callbacks of the expat parser
    void callbackStartElement( const xmlChar *localName , const xmlChar* prefix, const xmlChar* URI,
        int numNamespaces, const xmlChar** namespaces, int numAttributes, int defaultedAttributes, const xmlChar **attributes );
    void callbackEndElement( const xmlChar* localName, const xmlChar* prefix, const xmlChar* URI );
    void callbackCharacters( const xmlChar* s, int nLen );
#if 0
    bool callbackExternalEntityRef( XML_Parser parser, const xmlChar *openEntityNames, const xmlChar *base, const xmlChar *systemId, const xmlChar *publicId);
    void callbackEntityDecl(const xmlChar *entityName, int is_parameter_entity,
            const xmlChar *value, int value_length, const xmlChar *base,
            const xmlChar *systemId, const xmlChar *publicId,
            const xmlChar *notationName);
#endif

    void pushEntity( const Entity& rEntity );
    void popEntity();
    Entity& getEntity()             { return *mpTop; }
    const Entity& getEntity() const { return *mpTop; }
    void parse();
    void produce( bool bForceFlush = false );

    bool hasNamespaceURL( const OUString& rPrefix ) const;

private:
    bool consume(EventList *);
    void deleteUsedEvents();
    void sendPendingCharacters();

    sal_Int32 GetToken( const xmlChar* pName, sal_Int32 nameLen );
    sal_Int32 GetTokenWithPrefix( const xmlChar* pPrefix, int prefixLen, const xmlChar* pName, int nameLen ) throw (::com::sun::star::xml::sax::SAXException);
    OUString GetNamespaceURL( const OString& rPrefix ) throw (::com::sun::star::xml::sax::SAXException);
    sal_Int32 GetNamespaceToken( const OUString& rNamespaceURL );
    sal_Int32 GetTokenWithContextNamespace( sal_Int32 nNamespaceToken, const xmlChar* pName, int nNameLen );
    void DefineNamespace( const OString& rPrefix, const OUString& namespaceURL );

private:
#if 0
    FastSaxParser* mpFront;
#endif
    osl::Mutex maMutex; ///< Protecting whole parseStream() execution
    ::rtl::Reference< FastLocatorImpl >     mxDocumentLocator;
    NamespaceMap                            maNamespaceMap;

    ParserData maData;                      /// Cached parser configuration for next call of parseStream().

    Entity *mpTop;                          /// std::stack::top() is amazingly slow => cache this.
    ::std::stack< Entity > maEntities;      /// Entity stack for each call of parseStream().
    OUString pendingCharacters;             /// Data from characters() callback that needs to be sent.
};

} // namespace sax_fastparser

namespace {

class ParserThread: public salhelper::Thread
{
    FastSaxParserImpl *mpParser;
public:
    ParserThread(FastSaxParserImpl *pParser): Thread("Parser"), mpParser(pParser) {}
private:
    virtual void execute() SAL_OVERRIDE
    {
        try
        {
            mpParser->parse();
        }
        catch (const Exception &)
        {
            Entity &rEntity = mpParser->getEntity();
            rEntity.getEvent( EXCEPTION );
            mpParser->produce( true );
        }
    }
};

extern "C" {

static void call_callbackStartElement(void *userData, const xmlChar *localName , const xmlChar* prefix, const xmlChar* URI,
    int numNamespaces, const xmlChar** namespaces, int numAttributes, int defaultedAttributes, const xmlChar **attributes)
{
    FastSaxParserImpl* pFastParser = reinterpret_cast<FastSaxParserImpl*>( userData );
    pFastParser->callbackStartElement( localName, prefix, URI, numNamespaces, namespaces, numAttributes, defaultedAttributes, attributes );
}

static void call_callbackEndElement(void *userData, const xmlChar* localName, const xmlChar* prefix, const xmlChar* URI)
{
    FastSaxParserImpl* pFastParser = reinterpret_cast<FastSaxParserImpl*>( userData );
    pFastParser->callbackEndElement( localName, prefix, URI );
}

static void call_callbackCharacters( void *userData , const xmlChar *s , int nLen )
{
    FastSaxParserImpl* pFastParser = reinterpret_cast<FastSaxParserImpl*>( userData );
    pFastParser->callbackCharacters( s, nLen );
}

#if 0
static void call_callbackEntityDecl(void *userData, const xmlChar *entityName,
        int is_parameter_entity, const xmlChar *value, int value_length,
        const xmlChar *base, const xmlChar *systemId,
        const xmlChar *publicId, const xmlChar *notationName)
{
    FastSaxParserImpl* pFastParser = reinterpret_cast<FastSaxParserImpl*>(userData);
    pFastParser->callbackEntityDecl(entityName, is_parameter_entity, value,
            value_length, base, systemId, publicId, notationName);
}

static int call_callbackExternalEntityRef( XML_Parser parser,
        const xmlChar *openEntityNames, const xmlChar *base, const xmlChar *systemId, const xmlChar *publicId )
{
    FastSaxParserImpl* pFastParser = reinterpret_cast<FastSaxParserImpl*>( XML_GetUserData( parser ) );
    return pFastParser->callbackExternalEntityRef( parser, openEntityNames, base, systemId, publicId );
}
#endif
}

class FastLocatorImpl : public WeakImplHelper1< XLocator >
{
public:
    FastLocatorImpl( FastSaxParserImpl *p ) : mpParser(p) {}

    void dispose() { mpParser = 0; }
    void checkDispose() throw (RuntimeException) { if( !mpParser ) throw DisposedException(); }

    //XLocator
    virtual sal_Int32 SAL_CALL getColumnNumber(void) throw (RuntimeException, std::exception) SAL_OVERRIDE;
    virtual sal_Int32 SAL_CALL getLineNumber(void) throw (RuntimeException, std::exception) SAL_OVERRIDE;
    virtual OUString SAL_CALL getPublicId(void) throw (RuntimeException, std::exception) SAL_OVERRIDE;
    virtual OUString SAL_CALL getSystemId(void) throw (RuntimeException, std::exception) SAL_OVERRIDE;

private:
    FastSaxParserImpl *mpParser;
};

sal_Int32 SAL_CALL FastLocatorImpl::getColumnNumber(void) throw (RuntimeException, std::exception)
{
    checkDispose();
    return xmlSAX2GetColumnNumber( mpParser->getEntity().mpParser );
}

sal_Int32 SAL_CALL FastLocatorImpl::getLineNumber(void) throw (RuntimeException, std::exception)
{
    checkDispose();
    return xmlSAX2GetLineNumber( mpParser->getEntity().mpParser );
}

OUString SAL_CALL FastLocatorImpl::getPublicId(void) throw (RuntimeException, std::exception)
{
    checkDispose();
    return mpParser->getEntity().maStructSource.sPublicId;
}

OUString SAL_CALL FastLocatorImpl::getSystemId(void) throw (RuntimeException, std::exception)
{
    checkDispose();
    return mpParser->getEntity().maStructSource.sSystemId;
}

ParserData::ParserData()
    : mpTokenHandler( NULL )
{}

ParserData::~ParserData()
{}

Entity::Entity(const ParserData& rData)
    : ParserData(rData)
    , mnProducedEventsSize(0)
    , mpProducedEvents(NULL)
    , mbEnableThreads(false)
    , mpParser(NULL)
{
}

Entity::Entity(const Entity& e)
    : ParserData(e)
    , mnProducedEventsSize(0)
    , mpProducedEvents(NULL)
    , mbEnableThreads(e.mbEnableThreads)
    , maStructSource(e.maStructSource)
    , mpParser(e.mpParser)
    , maConverter(e.maConverter)
    , maSavedException(e.maSavedException)
    , maNamespaceStack(e.maNamespaceStack)
    , maContextStack(e.maContextStack)
    , maNamespaceCount(e.maNamespaceCount)
    , maNamespaceDefines(e.maNamespaceDefines)
{
}

Entity::~Entity()
{
}

void Entity::startElement( Event *pEvent )
{
    const sal_Int32& nElementToken = pEvent->mnElementToken;
    const OUString& aNamespace = pEvent->msNamespace;
    const OUString& aElementName = pEvent->msElementName;

    // Use un-wrapped pointers to avoid significant acquire/release overhead
    XFastContextHandler *pParentContext = NULL;
    if( !maContextStack.empty() )
    {
        pParentContext = maContextStack.top().mxContext.get();
        if( !pParentContext )
        {
            maContextStack.push( SaxContext(nElementToken, aNamespace, aElementName) );
            return;
        }
    }

    maContextStack.push( SaxContext( nElementToken, aNamespace, aElementName ) );

    try
    {
        Reference< XFastAttributeList > xAttr( pEvent->mxAttributes.get() );
        Reference< XFastContextHandler > xContext;
        if( nElementToken == FastToken::DONTKNOW )
        {
            if( pParentContext )
                xContext = pParentContext->createUnknownChildContext( aNamespace, aElementName, xAttr );
            else if( mxDocumentHandler.is() )
                xContext = mxDocumentHandler->createUnknownChildContext( aNamespace, aElementName, xAttr );

            if( xContext.is() )
            {
                xContext->startUnknownElement( aNamespace, aElementName, xAttr );
            }
        }
        else
        {
            if( pParentContext )
                xContext = pParentContext->createFastChildContext( nElementToken, xAttr );
            else if( mxDocumentHandler.is() )
                xContext = mxDocumentHandler->createFastChildContext( nElementToken, xAttr );

            if( xContext.is() )
                xContext->startFastElement( nElementToken, xAttr );
        }
        // swap the reference we own in to avoid referencing thrash.
        maContextStack.top().mxContext.set( static_cast<XFastContextHandler *>( xContext.get() ) );
        xContext.set( NULL, UNO_REF_NO_ACQUIRE );
    }
    catch (const Exception& e)
    {
        saveException( e );
    }
}

void Entity::characters( const OUString& sChars )
{
    if (maContextStack.empty())
    {
        // Malformed XML stream !?
        return;
    }

    const Reference< XFastContextHandler >& xContext( maContextStack.top().mxContext );
    if( xContext.is() ) try
    {
        xContext->characters( sChars );
    }
    catch (const Exception& e)
    {
        saveException( e );
    }
}

void Entity::endElement()
{
    if (maContextStack.empty())
    {
        // Malformed XML stream !?
        return;
    }

    const SaxContext& aContext = maContextStack.top();
    const Reference< XFastContextHandler >& xContext( aContext.mxContext );
    if( xContext.is() ) try
    {
        sal_Int32 nElementToken = aContext.mnElementToken;
        if( nElementToken != FastToken::DONTKNOW )
            xContext->endFastElement( nElementToken );
        else
            xContext->endUnknownElement( aContext.maNamespace, aContext.maElementName );
    }
    catch (const Exception& e)
    {
        saveException( e );
    }
    maContextStack.pop();
}

EventList* Entity::getEventList()
{
    if (!mpProducedEvents)
    {
        osl::ResettableMutexGuard aGuard(maEventProtector);
        if (!maUsedEvents.empty())
        {
            mpProducedEvents = maUsedEvents.front();
            maUsedEvents.pop();
            aGuard.clear(); // unlock
            mnProducedEventsSize = 0;
        }
        if (!mpProducedEvents)
        {
            mpProducedEvents = new EventList();
            mpProducedEvents->resize(mnEventListSize);
            mnProducedEventsSize = 0;
        }
    }
    return mpProducedEvents;
}

Event& Entity::getEvent( CallbackType aType )
{
    if (!mbEnableThreads)
        return maSharedEvent;

    EventList* pEventList = getEventList();
    Event& rEvent = (*pEventList)[mnProducedEventsSize++];
    rEvent.maType = aType;
    return rEvent;
}

OUString lclGetErrorMessage( xmlParserCtxtPtr ctxt, const OUString& sSystemId, sal_Int32 nLine )
{
    const sal_Char* pMessage;
    xmlErrorPtr error = xmlCtxtGetLastError( ctxt );
    if( error && error->message )
        pMessage = error->message;
    else
        pMessage = "unknown error";
    OUStringBuffer aBuffer( '[' );
    aBuffer.append( sSystemId );
    aBuffer.append( " line " );
    aBuffer.append( nLine );
    aBuffer.append( "]: " );
    aBuffer.appendAscii( pMessage );
    return aBuffer.makeStringAndClear();
}

// throw an exception, but avoid callback if
// during a threaded produce
void Entity::throwException( const ::rtl::Reference< FastLocatorImpl > &xDocumentLocator,
                             bool mbDuringParse )
{
    // Error during parsing !
    SAXParseException aExcept(
        lclGetErrorMessage( mpParser,
                            xDocumentLocator->getSystemId(),
                            xDocumentLocator->getLineNumber() ),
        Reference< XInterface >(),
        Any( &maSavedException, getCppuType( &maSavedException ) ),
        xDocumentLocator->getPublicId(),
        xDocumentLocator->getSystemId(),
        xDocumentLocator->getLineNumber(),
        xDocumentLocator->getColumnNumber()
    );

    // error handler is set, it may throw the exception
    if( !mbDuringParse || !mbEnableThreads )
    {
        if (mxErrorHandler.is() )
            mxErrorHandler->fatalError( Any( aExcept ) );
    }

    // error handler has not thrown, but parsing must stop => throw ourselves
    throw aExcept;
}

// In the single threaded case we emit events via our C
// callbacks, so any exception caught must be queued up until
// we can safely re-throw it from our C++ parent of parse()

// If multi-threaded, we need to push an EXCEPTION event, at
// which point we transfer ownership of maSavedException to
// the consuming thread.
void Entity::saveException( const Exception &e )
{
    // fdo#81214 - allow the parser to run on after an exception,
    // unexpectedly some 'startElements' produce an UNO_QUERY_THROW
    // for XComponent; and yet expect to continue parsing.
    SAL_WARN("sax", "Unexpected exception from XML parser " << e.Message);
    maSavedException <<= e;
}

} // namespace

namespace sax_fastparser {

FastSaxParserImpl::FastSaxParserImpl( FastSaxParser* ) :
#if 0
    mpFront(pFront),
#endif
    mpTop(NULL)
{
    mxDocumentLocator.set( new FastLocatorImpl( this ) );
}

FastSaxParserImpl::~FastSaxParserImpl()
{
    if( mxDocumentLocator.is() )
        mxDocumentLocator->dispose();
}

void FastSaxParserImpl::DefineNamespace( const OString& rPrefix, const OUString& namespaceURL )
{
    Entity& rEntity = getEntity();
    assert(!rEntity.maNamespaceCount.empty()); // need a context!

    sal_uInt32 nOffset = rEntity.maNamespaceCount.top()++;
    if( rEntity.maNamespaceDefines.size() <= nOffset )
        rEntity.maNamespaceDefines.resize( rEntity.maNamespaceDefines.size() + 64 );

    rEntity.maNamespaceDefines[nOffset].reset( new NamespaceDefine( rPrefix, GetNamespaceToken( namespaceURL ), namespaceURL ) );
}

sal_Int32 FastSaxParserImpl::GetToken( const xmlChar* pName, sal_Int32 nameLen /* = 0 */ )
{
    return FastTokenHandlerBase::getTokenFromChars( getEntity().mxTokenHandler,
                                                    getEntity().mpTokenHandler,
                                                    XML_CAST( pName ), nameLen ); // uses utf-8
}

sal_Int32 FastSaxParserImpl::GetTokenWithPrefix( const xmlChar* pPrefix, int nPrefixLen, const xmlChar* pName, int nNameLen ) throw (SAXException)
{
    sal_Int32 nNamespaceToken = FastToken::DONTKNOW;

    Entity& rEntity = getEntity();
    if (rEntity.maNamespaceCount.empty())
        return nNamespaceToken;

    sal_uInt32 nNamespace = rEntity.maNamespaceCount.top();
    while( nNamespace-- )
    {
        const OString& rPrefix( rEntity.maNamespaceDefines[nNamespace]->maPrefix );
        if( (rPrefix.getLength() == nPrefixLen) &&
            (strncmp( rPrefix.getStr(), XML_CAST( pPrefix ), nPrefixLen ) == 0 ) )
        {
            nNamespaceToken = rEntity.maNamespaceDefines[nNamespace]->mnToken;
            break;
        }

        if( !nNamespace )
            throw SAXException("No namespace defined for " + OUString(XML_CAST(pPrefix),
                    nPrefixLen, RTL_TEXTENCODING_UTF8), Reference< XInterface >(), Any());
    }

    if( nNamespaceToken != FastToken::DONTKNOW )
    {
        sal_Int32 nNameToken = GetToken( pName, nNameLen );
        if( nNameToken != FastToken::DONTKNOW )
            return nNamespaceToken | nNameToken;
    }

    return FastToken::DONTKNOW;
}

sal_Int32 FastSaxParserImpl::GetNamespaceToken( const OUString& rNamespaceURL )
{
    NamespaceMap::iterator aIter( maNamespaceMap.find( rNamespaceURL ) );
    if( aIter != maNamespaceMap.end() )
        return (*aIter).second;
    else
        return FastToken::DONTKNOW;
}

OUString FastSaxParserImpl::GetNamespaceURL( const OString& rPrefix ) throw (SAXException)
{
    Entity& rEntity = getEntity();
    if( !rEntity.maNamespaceCount.empty() )
    {
        sal_uInt32 nNamespace = rEntity.maNamespaceCount.top();
        while( nNamespace-- )
            if( rEntity.maNamespaceDefines[nNamespace]->maPrefix == rPrefix )
                return rEntity.maNamespaceDefines[nNamespace]->maNamespaceURL;
    }

    throw SAXException("No namespace defined for " + OUString::fromUtf8(rPrefix),
            Reference< XInterface >(), Any());
}

sal_Int32 FastSaxParserImpl::GetTokenWithContextNamespace( sal_Int32 nNamespaceToken, const xmlChar* pName, int nNameLen )
{
    if( nNamespaceToken != FastToken::DONTKNOW )
    {
        sal_Int32 nNameToken = GetToken( pName, nNameLen );
        if( nNameToken != FastToken::DONTKNOW )
            return nNamespaceToken | nNameToken;
    }

    return FastToken::DONTKNOW;
}

/***************
*
* parseStream does Parser-startup initializations. The FastSaxParser::parse() method does
* the file-specific initialization work. (During a parser run, external files may be opened)
*
****************/
void FastSaxParserImpl::parseStream(const InputSource& maStructSource)
    throw (SAXException, IOException, RuntimeException, std::exception)
{
    xmlInitParser();

    // Only one text at one time
    MutexGuard guard( maMutex );

    Entity entity( maData );
    entity.maStructSource = maStructSource;

    if( !entity.mxTokenHandler.is() )
        throw SAXException("No token handler, use setTokenHandler()", Reference< XInterface >(), Any() );

    if( !entity.maStructSource.aInputStream.is() )
        throw SAXException("No input source", Reference< XInterface >(), Any() );

    entity.maConverter.setInputStream( entity.maStructSource.aInputStream );
    if( !entity.maStructSource.sEncoding.isEmpty() )
        entity.maConverter.setEncoding( OUStringToOString( entity.maStructSource.sEncoding, RTL_TEXTENCODING_ASCII_US ) );

    pushEntity( entity );
    Entity& rEntity = getEntity();
    try
    {
        // start the document
        if( rEntity.mxDocumentHandler.is() )
        {
            Reference< XLocator > xLoc( mxDocumentLocator.get() );
            rEntity.mxDocumentHandler->setDocumentLocator( xLoc );
            rEntity.mxDocumentHandler->startDocument();
        }

        rEntity.mbEnableThreads = (rEntity.maStructSource.aInputStream->available() > 10000);

        if (rEntity.mbEnableThreads)
        {
            rtl::Reference<ParserThread> xParser;
            xParser = new ParserThread(this);
            xParser->launch();
            bool done = false;
            do {
                rEntity.maConsumeResume.wait();
                rEntity.maConsumeResume.reset();

                osl::ResettableMutexGuard aGuard(rEntity.maEventProtector);
                while (!rEntity.maPendingEvents.empty())
                {
                    if (rEntity.maPendingEvents.size() <= rEntity.mnEventLowWater)
                        rEntity.maProduceResume.set(); // start producer again

                    EventList *pEventList = rEntity.maPendingEvents.front();
                    rEntity.maPendingEvents.pop();
                    aGuard.clear(); // unlock

                    if (!consume(pEventList))
                        done = true;

                    aGuard.reset(); // lock
                    rEntity.maUsedEvents.push(pEventList);
                }
            } while (!done);
            xParser->join();
            deleteUsedEvents();
        }
        else
        {
            parse();
        }

        // finish document
        if( rEntity.mxDocumentHandler.is() )
        {
            rEntity.mxDocumentHandler->endDocument();
        }
    }
    catch (const SAXException&)
    {
        // TODO free mpParser.myDoc ?
        xmlFreeParserCtxt( rEntity.mpParser );
        popEntity();
        throw;
    }
    catch (const IOException&)
    {
        xmlFreeParserCtxt( rEntity.mpParser );
        popEntity();
        throw;
    }
    catch (const RuntimeException&)
    {
        xmlFreeParserCtxt( rEntity.mpParser );
        popEntity();
        throw;
    }

    xmlFreeParserCtxt( rEntity.mpParser );
    popEntity();
}

void FastSaxParserImpl::setFastDocumentHandler( const Reference< XFastDocumentHandler >& Handler ) throw (RuntimeException)
{
    maData.mxDocumentHandler = Handler;
}

void FastSaxParserImpl::setTokenHandler( const Reference< XFastTokenHandler >& xHandler ) throw (RuntimeException)
{
    maData.mxTokenHandler = xHandler;
    maData.mpTokenHandler = dynamic_cast< FastTokenHandlerBase *>( xHandler.get() );
}

void FastSaxParserImpl::registerNamespace( const OUString& NamespaceURL, sal_Int32 NamespaceToken ) throw (IllegalArgumentException, RuntimeException)
{
    if( NamespaceToken >= FastToken::NAMESPACE )
    {
        if( GetNamespaceToken( NamespaceURL ) == FastToken::DONTKNOW )
        {
            maNamespaceMap[ NamespaceURL ] = NamespaceToken;
            return;
        }
    }
    throw IllegalArgumentException();
}

OUString FastSaxParserImpl::getNamespaceURL( const OUString& rPrefix ) throw(IllegalArgumentException, RuntimeException)
{
    try
    {
        return GetNamespaceURL( OUStringToOString( rPrefix, RTL_TEXTENCODING_UTF8 ) );
    }
    catch (const Exception&)
    {
    }
    throw IllegalArgumentException();
}

void FastSaxParserImpl::setErrorHandler(const Reference< XErrorHandler > & Handler) throw (RuntimeException)
{
    maData.mxErrorHandler = Handler;
}

void FastSaxParserImpl::setEntityResolver(const Reference < XEntityResolver > & Resolver) throw (RuntimeException)
{
    maData.mxEntityResolver = Resolver;
}

void FastSaxParserImpl::setLocale( const lang::Locale & Locale ) throw (RuntimeException)
{
    maData.maLocale = Locale;
}

void FastSaxParserImpl::deleteUsedEvents()
{
    Entity& rEntity = getEntity();
    osl::ResettableMutexGuard aGuard(rEntity.maEventProtector);

    while (!rEntity.maUsedEvents.empty())
    {
        EventList *pEventList = rEntity.maUsedEvents.front();
        rEntity.maUsedEvents.pop();

        aGuard.clear(); // unlock

        delete pEventList;

        aGuard.reset(); // lock
    }
}

void FastSaxParserImpl::produce( bool bForceFlush )
{
    Entity& rEntity = getEntity();
    if (bForceFlush ||
        rEntity.mnProducedEventsSize == rEntity.mnEventListSize)
    {
        osl::ResettableMutexGuard aGuard(rEntity.maEventProtector);

        while (rEntity.maPendingEvents.size() >= rEntity.mnEventHighWater)
        { // pause parsing for a bit
            aGuard.clear(); // unlock
            rEntity.maProduceResume.wait();
            rEntity.maProduceResume.reset();
            aGuard.reset(); // lock
        }

        rEntity.maPendingEvents.push(rEntity.mpProducedEvents);
        rEntity.mpProducedEvents = 0;

        aGuard.clear(); // unlock

        rEntity.maConsumeResume.set();
    }
}

bool FastSaxParserImpl::hasNamespaceURL( const OUString& rPrefix ) const
{
    if (maEntities.empty())
        return false;

    const Entity& rEntity = getEntity();

    if (rEntity.maNamespaceCount.empty())
        return false;

    OString aPrefix = OUStringToOString(rPrefix, RTL_TEXTENCODING_UTF8);
    sal_uInt32 nNamespace = rEntity.maNamespaceCount.top();
    while (nNamespace--)
    {
        if (rEntity.maNamespaceDefines[nNamespace]->maPrefix == aPrefix)
            return true;
    }

    return false;
}

bool FastSaxParserImpl::consume(EventList *pEventList)
{
    Entity& rEntity = getEntity();
    for (EventList::iterator aEventIt = pEventList->begin();
         aEventIt != pEventList->end(); ++aEventIt)
    {
        switch ((*aEventIt).maType)
        {
            case START_ELEMENT:
                rEntity.startElement( &(*aEventIt) );
                break;
            case END_ELEMENT:
                rEntity.endElement();
                break;
            case CHARACTERS:
                rEntity.characters( (*aEventIt).msChars );
                break;
            case DONE:
                return false;
            case EXCEPTION:
                rEntity.throwException( mxDocumentLocator, false );
                return false;
            default:
                assert(false);
                return false;
        }
    }
    return true;
}

void FastSaxParserImpl::pushEntity( const Entity& rEntity )
{
    maEntities.push( rEntity );
    mpTop = &maEntities.top();
}

void FastSaxParserImpl::popEntity()
{
    maEntities.pop();
    mpTop = !maEntities.empty() ? &maEntities.top() : NULL;
}

// starts parsing with actual parser !
void FastSaxParserImpl::parse()
{
    const int BUFFER_SIZE = 16 * 1024;
    Sequence< sal_Int8 > seqOut( BUFFER_SIZE );

    Entity& rEntity = getEntity();

    // set all necessary C-Callbacks
    static xmlSAXHandler callbacks;
    callbacks.startElementNs = call_callbackStartElement;
    callbacks.endElementNs = call_callbackEndElement;
    callbacks.characters = call_callbackCharacters;
    callbacks.initialized = XML_SAX2_MAGIC;
#if 0
    XML_SetEntityDeclHandler(entity.mpParser, call_callbackEntityDecl);
    XML_SetExternalEntityRefHandler( entity.mpParser, call_callbackExternalEntityRef );
#endif
    int nRead = 0;
    do
    {
        nRead = rEntity.maConverter.readAndConvert( seqOut, BUFFER_SIZE );
        if( nRead <= 0 )
        {
            if( rEntity.mpParser != NULL )
            {
                if( xmlParseChunk( rEntity.mpParser, (const char*) seqOut.getConstArray(), 0, 1 ) != XML_ERR_OK )
                    rEntity.throwException( mxDocumentLocator, true );
            }
            break;
        }

        bool bContinue = true;
        if( rEntity.mpParser == NULL )
        {
            // create parser with proper encoding (needs the first chunk of data)
            rEntity.mpParser = xmlCreatePushParserCtxt( &callbacks, this,
                reinterpret_cast<const char*>(seqOut.getConstArray()), nRead, NULL );
            if( !rEntity.mpParser )
                throw SAXException("Couldn't create parser", Reference< XInterface >(), Any() );

            // Tell libxml2 parser to decode entities in attribute values.
            xmlCtxtUseOptions(rEntity.mpParser, XML_PARSE_NOENT);
        }
        else
        {
            bContinue = xmlParseChunk( rEntity.mpParser, reinterpret_cast<const char*>(seqOut.getConstArray()), nRead, 0 )
                            == XML_ERR_OK;
        }

        // callbacks used inside XML_Parse may have caught an exception
        if( !bContinue || rEntity.maSavedException.hasValue() )
            rEntity.throwException( mxDocumentLocator, true );
    } while( nRead > 0 );
    rEntity.getEvent( DONE );
    if( rEntity.mbEnableThreads )
        produce( true );
}

// The C-Callbacks
void FastSaxParserImpl::callbackStartElement(const xmlChar *localName , const xmlChar* prefix, const xmlChar* URI,
    int numNamespaces, const xmlChar** namespaces, int numAttributes, int /*defaultedAttributes*/, const xmlChar **attributes)
{
    if( !pendingCharacters.isEmpty())
        sendPendingCharacters();
    Entity& rEntity = getEntity();
    if( rEntity.maNamespaceCount.empty() )
    {
        rEntity.maNamespaceCount.push(0);
        DefineNamespace( OString("xml"), OUString( "http://www.w3.org/XML/1998/namespace" ));
    }
    else
    {
        rEntity.maNamespaceCount.push( rEntity.maNamespaceCount.top() );
    }

    // create attribute map and process namespace instructions
    Event& rEvent = getEntity().getEvent( START_ELEMENT );
    if (rEvent.mxAttributes.is())
        rEvent.mxAttributes->clear();
    else
        rEvent.mxAttributes.set(
                new FastAttributeList( rEntity.mxTokenHandler,
                                       rEntity.mpTokenHandler ) );

    sal_Int32 nNamespaceToken = FastToken::DONTKNOW;
    if (!rEntity.maNamespaceStack.empty())
    {
        rEvent.msNamespace = rEntity.maNamespaceStack.top().msName;
        nNamespaceToken = rEntity.maNamespaceStack.top().mnToken;
    }

    try
    {
        /*  #158414# Each element may define new namespaces, also for attribues.
            First, process all namespaces, second, process the attributes after namespaces
            have been initialized. */

        // #158414# first: get namespaces
        for (int i = 0; i < numNamespaces * 2; i += 2)
        {
            // namespaces[] is (prefix/URI)
            if( namespaces[ i ] != NULL )
            {
                    DefineNamespace( OString( XML_CAST( namespaces[ i ] )),
                        OUString( XML_CAST( namespaces[ i + 1 ] ), strlen( XML_CAST( namespaces[ i + 1 ] )), RTL_TEXTENCODING_UTF8 ));
            }
            else
            {
                // default namespace
                rEvent.msNamespace = OUString( XML_CAST( namespaces[ i + 1 ] ), strlen( XML_CAST( namespaces[ i + 1 ] )), RTL_TEXTENCODING_UTF8 );
                nNamespaceToken = GetNamespaceToken( rEvent.msNamespace );
            }
        }

        // #158414# second: fill attribute list with other attributes
        for (int i = 0; i < numAttributes * 5; i += 5)
        {
            if( attributes[ i + 1 ] != NULL )
            {
                sal_Int32 nAttributeToken = GetTokenWithPrefix( attributes[ i + 1 ], strlen( XML_CAST( attributes[ i + 1 ] )), attributes[ i ], strlen( XML_CAST( attributes[ i ] )));
                if( nAttributeToken != FastToken::DONTKNOW )
                    rEvent.mxAttributes->add( nAttributeToken, XML_CAST( attributes[ i + 3 ] ), attributes[ i + 4 ] - attributes[ i + 3 ] );
                else
                    rEvent.mxAttributes->addUnknown( OUString( XML_CAST( attributes[ i + 2 ] ), strlen( XML_CAST( attributes[ i + 2 ] )), RTL_TEXTENCODING_UTF8 ),
                            OString( XML_CAST( attributes[ i ] )), OString( XML_CAST( attributes[ i + 3 ] ), attributes[ i + 4 ] - attributes[ i + 3 ] ));
            }
            else
            {
                sal_Int32 nAttributeToken = GetToken( attributes[ i ], strlen( XML_CAST( attributes[ i ] )));
                if( nAttributeToken != FastToken::DONTKNOW )
                    rEvent.mxAttributes->add( nAttributeToken, XML_CAST( attributes[ i + 3 ] ), attributes[ i + 4 ] - attributes[ i + 3 ] );
                else
                    rEvent.mxAttributes->addUnknown( XML_CAST( attributes[ i ] ),
                        OString( XML_CAST( attributes[ i + 3 ] ), attributes[ i + 4 ] - attributes[ i + 3 ] ));
            }
        }

        if( prefix != NULL )
            rEvent.mnElementToken = GetTokenWithPrefix( prefix, strlen( XML_CAST( prefix )), localName, strlen( XML_CAST( localName )));
        else if( !rEvent.msNamespace.isEmpty() )
            rEvent.mnElementToken = GetTokenWithContextNamespace( nNamespaceToken, localName, strlen( XML_CAST( localName )));
        else
            rEvent.mnElementToken = GetToken( localName, strlen( XML_CAST( localName )));

        if( rEvent.mnElementToken == FastToken::DONTKNOW )
        {
            if( prefix != NULL )
            {
                rEvent.msNamespace = OUString( XML_CAST( URI ), strlen( XML_CAST( URI )), RTL_TEXTENCODING_UTF8 );
                nNamespaceToken = GetNamespaceToken( rEvent.msNamespace );
            }
            rEvent.msElementName = OUString( XML_CAST( localName ), strlen( XML_CAST( localName )), RTL_TEXTENCODING_UTF8 );
        }
        else // token is always preferred.
            rEvent.msElementName.clear();

        rEntity.maNamespaceStack.push( NameWithToken(rEvent.msNamespace, nNamespaceToken) );
        if (rEntity.mbEnableThreads)
            produce();
        else
            rEntity.startElement( &rEvent );
    }
    catch (const Exception& e)
    {
        rEntity.saveException( e );
    }
}

void FastSaxParserImpl::callbackEndElement( const xmlChar*, const xmlChar*, const xmlChar* )
{
    if( !pendingCharacters.isEmpty())
        sendPendingCharacters();
    Entity& rEntity = getEntity();
    SAL_WARN_IF(rEntity.maNamespaceCount.empty(), "sax", "Empty NamespaceCount");
    if( !rEntity.maNamespaceCount.empty() )
        rEntity.maNamespaceCount.pop();

    SAL_WARN_IF(rEntity.maNamespaceStack.empty(), "sax", "Empty NamespaceStack");
    if( !rEntity.maNamespaceStack.empty() )
        rEntity.maNamespaceStack.pop();

    rEntity.getEvent( END_ELEMENT );
    if (rEntity.mbEnableThreads)
        produce();
    else
        rEntity.endElement();
}

void FastSaxParserImpl::callbackCharacters( const xmlChar* s, int nLen )
{
    // SAX interface allows that the characters callback splits content of one XML node
    // (e.g. because there's an entity that needs decoding), however for consumers it's
    // simpler FastSaxParser's character callback provides the whole string at once,
    // so merge data from possible multiple calls and send them at once (before the element
    // ends or another one starts).
    pendingCharacters += OUString( XML_CAST( s ), nLen, RTL_TEXTENCODING_UTF8 );
}

void FastSaxParserImpl::sendPendingCharacters()
{
    Entity& rEntity = getEntity();
    Event& rEvent = rEntity.getEvent( CHARACTERS );
    rEvent.msChars = pendingCharacters;
    pendingCharacters.clear();
    if (rEntity.mbEnableThreads)
        produce();
    else
        rEntity.characters( rEvent.msChars );
}

#if 0
void FastSaxParserImpl::callbackEntityDecl(
    SAL_UNUSED_PARAMETER const xmlChar * /*entityName*/,
    SAL_UNUSED_PARAMETER int /*is_parameter_entity*/,
    const xmlChar *value, SAL_UNUSED_PARAMETER int /*value_length*/,
    SAL_UNUSED_PARAMETER const xmlChar * /*base*/,
    SAL_UNUSED_PARAMETER const xmlChar * /*systemId*/,
    SAL_UNUSED_PARAMETER const xmlChar * /*publicId*/,
    SAL_UNUSED_PARAMETER const xmlChar * /*notationName*/)
{
    if (value) { // value != 0 means internal entity
        SAL_INFO("sax", "FastSaxParser: internal entity declaration, stopping");
        XML_StopParser(getEntity().mpParser, XML_FALSE);
        getEntity().saveException( SAXParseException(
            "FastSaxParser: internal entity declaration, stopping",
            static_cast<OWeakObject*>(mpFront), Any(),
            mxDocumentLocator->getPublicId(),
            mxDocumentLocator->getSystemId(),
            mxDocumentLocator->getLineNumber(),
            mxDocumentLocator->getColumnNumber() ) );
    } else {
        SAL_INFO("sax", "FastSaxParser: ignoring external entity declaration");
    }
}

bool FastSaxParserImpl::callbackExternalEntityRef(
    XML_Parser parser, const xmlChar *context,
    SAL_UNUSED_PARAMETER const xmlChar * /*base*/, const xmlChar *systemId,
    const xmlChar *publicId )
{
    bool bOK = true;
    InputSource source;

    Entity& rCurrEntity = getEntity();
    Entity aNewEntity( rCurrEntity );

    if( rCurrEntity.mxEntityResolver.is() ) try
    {
        aNewEntity.maStructSource = rCurrEntity.mxEntityResolver->resolveEntity(
            OUString( publicId, strlen( publicId ), RTL_TEXTENCODING_UTF8 ) ,
            OUString( systemId, strlen( systemId ), RTL_TEXTENCODING_UTF8 ) );
    }
    catch (const SAXParseException & e)
    {
        rCurrEntity.saveException( e );
        bOK = false;
    }
    catch (const SAXException& e)
    {
        rCurrEntity.saveException( SAXParseException(
            e.Message, e.Context, e.WrappedException,
            mxDocumentLocator->getPublicId(),
            mxDocumentLocator->getSystemId(),
            mxDocumentLocator->getLineNumber(),
            mxDocumentLocator->getColumnNumber() ) );
        bOK = false;
    }

    if( aNewEntity.maStructSource.aInputStream.is() )
    {
        aNewEntity.mpParser = XML_ExternalEntityParserCreate( parser, context, 0 );
        if( !aNewEntity.mpParser )
        {
            return false;
        }

        aNewEntity.maConverter.setInputStream( aNewEntity.maStructSource.aInputStream );
        pushEntity( aNewEntity );
        try
        {
            parse();
        }
        catch (const SAXParseException& e)
        {
            rCurrEntity.saveException( e );
            bOK = false;
        }
        catch (const IOException& e)
        {
            SAXException aEx;
            aEx.WrappedException <<= e;
            rCurrEntity.saveException( aEx );
            bOK = false;
        }
        catch (const RuntimeException& e)
        {
            SAXException aEx;
            aEx.WrappedException <<= e;
            rCurrEntity.saveException( aEx );
            bOK = false;
        }

        popEntity();
        XML_ParserFree( aNewEntity.mpParser );
    }

    return bOK;
}
#endif

FastSaxParser::FastSaxParser() : mpImpl(new FastSaxParserImpl(this)) {}

FastSaxParser::~FastSaxParser()
{
    delete mpImpl;
}

void FastSaxParser::parseStream( const xml::sax::InputSource& aInputSource )
    throw (xml::sax::SAXException, io::IOException,
           uno::RuntimeException, std::exception)
{
    mpImpl->parseStream(aInputSource);
}

void FastSaxParser::setFastDocumentHandler( const uno::Reference<xml::sax::XFastDocumentHandler>& Handler )
    throw (uno::RuntimeException, std::exception)
{
    mpImpl->setFastDocumentHandler(Handler);
}

void FastSaxParser::setTokenHandler( const uno::Reference<xml::sax::XFastTokenHandler>& Handler )
    throw (uno::RuntimeException, std::exception)
{
    mpImpl->setTokenHandler(Handler);
}

void FastSaxParser::registerNamespace( const OUString& NamespaceURL, sal_Int32 NamespaceToken )
    throw (lang::IllegalArgumentException, uno::RuntimeException, std::exception)
{
    mpImpl->registerNamespace(NamespaceURL, NamespaceToken);
}

OUString FastSaxParser::getNamespaceURL( const OUString& rPrefix )
    throw(lang::IllegalArgumentException, uno::RuntimeException, std::exception)
{
    return mpImpl->getNamespaceURL(rPrefix);
}

void FastSaxParser::setErrorHandler( const uno::Reference< xml::sax::XErrorHandler >& Handler )
    throw (uno::RuntimeException, std::exception)
{
    mpImpl->setErrorHandler(Handler);
}

void FastSaxParser::setEntityResolver( const uno::Reference< xml::sax::XEntityResolver >& Resolver )
    throw (uno::RuntimeException, std::exception)
{
    mpImpl->setEntityResolver(Resolver);
}

void FastSaxParser::setLocale( const lang::Locale& rLocale )
    throw (uno::RuntimeException, std::exception)
{
    mpImpl->setLocale(rLocale);
}

OUString FastSaxParser::getImplementationName()
    throw (uno::RuntimeException, std::exception)
{
    return OUString("com.sun.star.comp.extensions.xml.sax.FastParser");
}

sal_Bool FastSaxParser::supportsService( const OUString& ServiceName )
    throw (uno::RuntimeException, std::exception)
{
    return cppu::supportsService(this, ServiceName);
}

uno::Sequence<OUString> FastSaxParser::getSupportedServiceNames()
    throw (uno::RuntimeException, std::exception)
{
    Sequence<OUString> seq(1);
    seq[0] = "com.sun.star.xml.sax.FastParser";
    return seq;
}

bool FastSaxParser::hasNamespaceURL( const OUString& rPrefix ) const
{
    return mpImpl->hasNamespaceURL(rPrefix);
}

} // namespace sax_fastparser

extern "C" SAL_DLLPUBLIC_EXPORT css::uno::XInterface * SAL_CALL
com_sun_star_comp_extensions_xml_sax_FastParser_get_implementation(
    css::uno::XComponentContext *,
    css::uno::Sequence<css::uno::Any> const &)
{
    return cppu::acquire(new FastSaxParser);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
