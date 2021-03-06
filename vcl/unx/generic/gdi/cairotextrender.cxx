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

#include "cairotextrender.hxx"

#include <basegfx/polygon/b2dpolypolygon.hxx>
#include <vcl/settings.hxx>
#include <vcl/sysdata.hxx>

#include "generic/printergfx.hxx"
#include "generic/genpspgraphics.h"
#include "generic/geninst.h"
#include "PhysicalFontFace.hxx"
#include "impfont.hxx"

#include <config_graphite.h>
#if ENABLE_GRAPHITE
#include <graphite_layout.hxx>
#include <graphite_serverfont.hxx>
#endif

#include <cairo.h>
#include <cairo-ft.h>
#include <cairo-xlib.h>
#include <cairo-xlib-xrender.h>

CairoTextRender::CairoTextRender(bool bPrinter):
    mbPrinter(bPrinter),
    nTextColor_(MAKE_SALCOLOR(0x00, 0x00, 0x00)) //black
{
    for( int i = 0; i < MAX_FALLBACK; ++i )
        mpServerFont[i] = NULL;

#if ENABLE_GRAPHITE
    // check if graphite fonts have been disabled
    static const char* pDisableGraphiteStr = getenv( "SAL_DISABLE_GRAPHITE" );
    bDisableGraphite_ = pDisableGraphiteStr && (pDisableGraphiteStr[0]!='0');
#endif
}

bool CairoTextRender::setFont( const FontSelectPattern *pEntry, int nFallbackLevel )
{
    // release all no longer needed font resources
    for( int i = nFallbackLevel; i < MAX_FALLBACK; ++i )
    {
        if( mpServerFont[i] != NULL )
        {
            // old server side font is no longer referenced
            GlyphCache::GetInstance().UncacheFont( *mpServerFont[i] );
            mpServerFont[i] = NULL;
        }
    }

    // return early if there is no new font
    if( !pEntry )
        return false;

    // return early if this is not a valid font for this graphics
    if( !pEntry->mpFontData )
        return false;

    // handle the request for a non-native X11-font => use the GlyphCache
    ServerFont* pServerFont = GlyphCache::GetInstance().CacheFont( *pEntry );
    if( pServerFont != NULL )
    {
        // ignore fonts with e.g. corrupted font files
        if( !pServerFont->TestFont() )
        {
            GlyphCache::GetInstance().UncacheFont( *pServerFont );
            return false;
        }

        // register to use the font
        mpServerFont[ nFallbackLevel ] = pServerFont;

        // apply font specific-hint settings if needed
        // TODO: also disable it for reference devices
        if( !mbPrinter )
        {
            ImplServerFontEntry* pSFE = static_cast<ImplServerFontEntry*>( pEntry->mpFontEntry );
            pSFE->HandleFontOptions();
        }

        return true;
    }

    return false;
}

ImplFontOptions* GetFCFontOptions( const ImplFontAttributes& rFontAttributes, int nSize);

void ImplServerFontEntry::HandleFontOptions( void )
{
    if( !mpServerFont )
        return;
    if( !mbGotFontOptions )
    {
        // get and cache the font options
        mbGotFontOptions = true;
        mpFontOptions.reset(GetFCFontOptions( *maFontSelData.mpFontData,
            maFontSelData.mnHeight ));
    }
    // apply the font options
    mpServerFont->SetFontOptions( mpFontOptions );
}

CairoFontsCache::LRUFonts CairoFontsCache::maLRUFonts;
int CairoFontsCache::mnRefCount = 0;

CairoFontsCache::CairoFontsCache()
{
    ++mnRefCount;
}

CairoFontsCache::~CairoFontsCache()
{
    --mnRefCount;
    if (!mnRefCount && !maLRUFonts.empty())
    {
        LRUFonts::iterator aEnd = maLRUFonts.end();
        for (LRUFonts::iterator aI = maLRUFonts.begin(); aI != aEnd; ++aI)
            cairo_font_face_destroy((cairo_font_face_t*)aI->first);
    }
}

void CairoFontsCache::CacheFont(void *pFont, const CairoFontsCache::CacheId &rId)
{
    maLRUFonts.push_front( std::pair<void*, CairoFontsCache::CacheId>(pFont, rId) );
    if (maLRUFonts.size() > 8)
    {
        cairo_font_face_destroy((cairo_font_face_t*)maLRUFonts.back().first);
        maLRUFonts.pop_back();
    }
}

void* CairoFontsCache::FindCachedFont(const CairoFontsCache::CacheId &rId)
{
    LRUFonts::iterator aEnd = maLRUFonts.end();
    for (LRUFonts::iterator aI = maLRUFonts.begin(); aI != aEnd; ++aI)
        if (aI->second == rId)
            return aI->first;
    return NULL;
}

namespace
{
    bool hasRotation(int nRotation)
    {
      return nRotation != 0;
    }

    double toRadian(int nDegree10th)
    {
        return (3600 - (nDegree10th)) * M_PI / 1800.0;
    }
}

void CairoTextRender::DrawServerFontLayout( const ServerFontLayout& rLayout )
{
    std::vector<cairo_glyph_t> cairo_glyphs;
    std::vector<int> glyph_extrarotation;
    cairo_glyphs.reserve( 256 );

    Point aPos;
    sal_GlyphId aGlyphId;
    for( int nStart = 0; rLayout.GetNextGlyphs( 1, &aGlyphId, aPos, nStart ); )
    {
        cairo_glyph_t aGlyph;
        aGlyph.index = aGlyphId & GF_IDXMASK;
        aGlyph.x = aPos.X();
        aGlyph.y = aPos.Y();
        cairo_glyphs.push_back(aGlyph);

        switch (aGlyphId & GF_ROTMASK)
        {
            case GF_ROTL:    // left
                glyph_extrarotation.push_back(1);
                break;
            case GF_ROTR:    // right
                glyph_extrarotation.push_back(-1);
                break;
            default:
                glyph_extrarotation.push_back(0);
                break;
        }
    }

    if (cairo_glyphs.empty())
        return;

    cairo_surface_t *surface = getCairoSurface();

    DBG_ASSERT( surface!=NULL, "no cairo surface for text" );
    if( !surface )
        return;

    /*
     * It might be ideal to cache surface and cairo context between calls and
     * only destroy it when the drawable changes, but to do that we need to at
     * least change the SalFrame etc impls to dtor the SalGraphics *before* the
     * destruction of the windows they reference
    */
    cairo_t *cr = cairo_create(surface);
    cairo_surface_destroy(surface);

    if (const void *pOptions = Application::GetSettings().GetStyleSettings().GetCairoFontOptions())
        cairo_set_font_options(cr, static_cast<const cairo_font_options_t*>(pOptions));

    double nDX, nDY;
    getSurfaceOffset(nDX, nDY);
    cairo_translate(cr, nDX, nDY);

    clipRegion(cr);

    cairo_set_source_rgb(cr,
        SALCOLOR_RED(nTextColor_)/255.0,
        SALCOLOR_GREEN(nTextColor_)/255.0,
        SALCOLOR_BLUE(nTextColor_)/255.0);

    ServerFont& rFont = rLayout.GetServerFont();

    FT_Face aFace = rFont.GetFtFace();
    CairoFontsCache::CacheId aId;
    aId.maFace = aFace;
    aId.mpOptions = rFont.GetFontOptions().get();
    aId.mbEmbolden = rFont.NeedsArtificialBold();

    cairo_matrix_t m;
    const FontSelectPattern& rFSD = rFont.GetFontSelData();
    int nHeight = rFSD.mnHeight;
    int nWidth = rFSD.mnWidth ? rFSD.mnWidth : nHeight;

    std::vector<int>::const_iterator aEnd = glyph_extrarotation.end();
    std::vector<int>::const_iterator aStart = glyph_extrarotation.begin();
    std::vector<int>::const_iterator aI = aStart;
    while (aI != aEnd)
    {
        int nGlyphRotation = *aI;

        std::vector<int>::const_iterator aNext = std::find_if(aI+1, aEnd, hasRotation);

        size_t nStartIndex = std::distance(aStart, aI);
        size_t nLen = std::distance(aI, aNext);

        aId.mbVerticalMetrics = nGlyphRotation != 0.0;
        cairo_font_face_t* font_face = (cairo_font_face_t*)CairoFontsCache::FindCachedFont(aId);
        if (!font_face)
        {
            const ImplFontOptions *pOptions = rFont.GetFontOptions().get();
            void *pPattern = pOptions ? pOptions->GetPattern(aFace, aId.mbEmbolden, aId.mbVerticalMetrics) : NULL;
            if (pPattern)
                font_face = cairo_ft_font_face_create_for_pattern(reinterpret_cast<FcPattern*>(pPattern));
            if (!font_face)
                font_face = cairo_ft_font_face_create_for_ft_face(reinterpret_cast<FT_Face>(aFace), rFont.GetLoadFlags());
            CairoFontsCache::CacheFont(font_face, aId);
        }
        cairo_set_font_face(cr, font_face);

        cairo_set_font_size(cr, nHeight);

        cairo_matrix_init_identity(&m);

        if (rLayout.GetOrientation())
            cairo_matrix_rotate(&m, toRadian(rLayout.GetOrientation()));

        cairo_matrix_scale(&m, nWidth, nHeight);

        if (nGlyphRotation)
        {
            cairo_matrix_rotate(&m, toRadian(nGlyphRotation*900));

            cairo_matrix_t em_square;
            cairo_matrix_init_identity(&em_square);
            cairo_get_matrix(cr, &em_square);

            cairo_matrix_scale(&em_square, aFace->units_per_EM,
                aFace->units_per_EM);
            cairo_set_matrix(cr, &em_square);

            cairo_font_extents_t font_extents;
            cairo_font_extents(cr, &font_extents);

            cairo_matrix_init_identity(&em_square);
            cairo_set_matrix(cr, &em_square);

            //gives the same positions as pre-cairo conversion, but I don't
            //like them
            double xdiff = 0.0;
            double ydiff = 0.0;
            if (nGlyphRotation == 1)
            {
                ydiff = font_extents.ascent/nHeight;
                xdiff = -font_extents.descent/nHeight;
            }
            else if (nGlyphRotation == -1)
            {
                cairo_text_extents_t text_extents;
                cairo_glyph_extents(cr, &cairo_glyphs[nStartIndex], nLen,
                    &text_extents);

                xdiff = -text_extents.x_advance/nHeight;
                //to restore an apparent bug in the original X11 impl, replace
                //nHeight with nWidth below
                xdiff += font_extents.descent/nHeight;
            }
            cairo_matrix_translate(&m, xdiff, ydiff);
        }

        if (rFont.NeedsArtificialItalic())
        {
            cairo_matrix_t shear;
            cairo_matrix_init_identity(&shear);
            shear.xy = -shear.xx * 0x6000L / 0x10000L;
            cairo_matrix_multiply(&m, &shear, &m);
        }

        cairo_set_font_matrix(cr, &m);
        cairo_show_glyphs(cr, &cairo_glyphs[nStartIndex], nLen);

#if OSL_DEBUG_LEVEL > 2
        //draw origin
        cairo_save (cr);
        cairo_rectangle (cr, cairo_glyphs[nStartIndex].x, cairo_glyphs[nStartIndex].y, 5, 5);
        cairo_set_source_rgba (cr, 1, 0, 0, 0.80);
        cairo_fill (cr);
        cairo_restore (cr);
#endif

        aI = aNext;
    }

    drawSurface(cr);
    cairo_destroy(cr);
}

const FontCharMapPtr CairoTextRender::GetFontCharMap() const
{
    if( !mpServerFont[0] )
        return NULL;

    const FontCharMapPtr pFCMap = mpServerFont[0]->GetFontCharMap();
    return pFCMap;
}

bool CairoTextRender::GetFontCapabilities(vcl::FontCapabilities &rGetImplFontCapabilities) const
{
    if (!mpServerFont[0])
        return false;
    return mpServerFont[0]->GetFontCapabilities(rGetImplFontCapabilities);
}

// SalGraphics

sal_uInt16 CairoTextRender::SetFont( FontSelectPattern *pEntry, int nFallbackLevel )
{
    sal_uInt16 nRetVal = 0;
    if( !setFont( pEntry, nFallbackLevel ) )
        nRetVal |= SAL_SETFONT_BADFONT;
    if( mbPrinter || (mpServerFont[ nFallbackLevel ] != NULL) )
        nRetVal |= SAL_SETFONT_USEDRAWTEXTARRAY;
    return nRetVal;
}

void
CairoTextRender::SetTextColor( SalColor nSalColor )
{
    if( nTextColor_ != nSalColor )
    {
        nTextColor_ = nSalColor;
    }
}

bool CairoTextRender::AddTempDevFont( PhysicalFontCollection* pFontCollection,
                                     const OUString& rFileURL,
                                     const OUString& rFontName )
{
    // inform PSP font manager
    OUString aUSystemPath;
    OSL_VERIFY( !osl::FileBase::getSystemPathFromFileURL( rFileURL, aUSystemPath ) );
    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    OString aOFileName( OUStringToOString( aUSystemPath, aEncoding ) );
    psp::PrintFontManager& rMgr = psp::PrintFontManager::get();
    std::vector<psp::fontID> aFontIds = rMgr.addFontFile( aOFileName );
    if( aFontIds.empty() )
        return false;

    GlyphCache& rGC = getPlatformGlyphCache();

    for (std::vector<psp::fontID>::iterator aI = aFontIds.begin(), aEnd = aFontIds.end(); aI != aEnd; ++aI)
    {
        // prepare font data
        psp::FastPrintFontInfo aInfo;
        rMgr.getFontFastInfo( *aI, aInfo );
        aInfo.m_aFamilyName = rFontName;

        // inform glyph cache of new font
        ImplDevFontAttributes aDFA = GenPspGraphics::Info2DevFontAttributes( aInfo );
        aDFA.mnQuality += 5800;

        int nFaceNum = rMgr.getFontFaceNumber( aInfo.m_nID );

        const OString& rFileName = rMgr.getFontFileSysPath( aInfo.m_nID );
        rGC.AddFontFile( rFileName, nFaceNum, aInfo.m_nID, aDFA );
    }

    // announce new font to device's font list
    rGC.AnnounceFonts( pFontCollection );
    return true;
}

void CairoTextRender::ClearDevFontCache()
{
    GlyphCache& rGC = getPlatformGlyphCache();
    rGC.ClearFontCache();
}

void CairoTextRender::GetDevFontList( PhysicalFontCollection* pFontCollection )
{
    // prepare the GlyphCache using psprint's font infos
    GlyphCache& rGC = getPlatformGlyphCache();

    psp::PrintFontManager& rMgr = psp::PrintFontManager::get();
    ::std::list< psp::fontID > aList;
    ::std::list< psp::fontID >::iterator it;
    psp::FastPrintFontInfo aInfo;
    rMgr.getFontList( aList );
    for( it = aList.begin(); it != aList.end(); ++it )
    {
        if( !rMgr.getFontFastInfo( *it, aInfo ) )
            continue;

        // normalize face number to the GlyphCache
        int nFaceNum = rMgr.getFontFaceNumber( aInfo.m_nID );

        // inform GlyphCache about this font provided by the PsPrint subsystem
        ImplDevFontAttributes aDFA = GenPspGraphics::Info2DevFontAttributes( aInfo );
        aDFA.mnQuality += 4096;
        const OString& rFileName = rMgr.getFontFileSysPath( aInfo.m_nID );
        rGC.AddFontFile( rFileName, nFaceNum, aInfo.m_nID, aDFA );
   }

    // announce glyphcache fonts
    rGC.AnnounceFonts( pFontCollection );

    // register platform specific font substitutions if available
    SalGenericInstance::RegisterFontSubstitutors( pFontCollection );

    ImplGetSVData()->maGDIData.mbNativeFontConfig = true;
}

void cairosubcallback(void* pPattern)
{
    const StyleSettings& rStyleSettings = Application::GetSettings().GetStyleSettings();
    const void* pFontOptions = rStyleSettings.GetCairoFontOptions();
    if( !pFontOptions )
        return;
    cairo_ft_font_options_substitute(static_cast<const cairo_font_options_t*>(pFontOptions),
        static_cast<FcPattern*>(pPattern));
}

ImplFontOptions* GetFCFontOptions( const ImplFontAttributes& rFontAttributes, int nSize)
{
    psp::FastPrintFontInfo aInfo;

    aInfo.m_aFamilyName = rFontAttributes.GetFamilyName();
    aInfo.m_eItalic = rFontAttributes.GetSlant();
    aInfo.m_eWeight = rFontAttributes.GetWeight();
    aInfo.m_eWidth = rFontAttributes.GetWidthType();

    const psp::PrintFontManager& rPFM = psp::PrintFontManager::get();
    return rPFM.getFontOptions(aInfo, nSize, cairosubcallback);
}

void
CairoTextRender::GetFontMetric( ImplFontMetricData *pMetric, int nFallbackLevel )
{
    if( nFallbackLevel >= MAX_FALLBACK )
        return;

    if( mpServerFont[nFallbackLevel] != NULL )
    {
        long rDummyFactor;
        mpServerFont[nFallbackLevel]->FetchFontMetric( *pMetric, rDummyFactor );
    }
}

bool CairoTextRender::GetGlyphBoundRect( sal_GlyphId aGlyphId, Rectangle& rRect )
{
    const int nLevel = aGlyphId >> GF_FONTSHIFT;
    if( nLevel >= MAX_FALLBACK )
        return false;

    ServerFont* pSF = mpServerFont[ nLevel ];
    if( !pSF )
        return false;

    aGlyphId &= GF_IDXMASK;
    const GlyphMetric& rGM = pSF->GetGlyphMetric(aGlyphId);
    Rectangle aRect( rGM.GetOffset(), rGM.GetSize() );

    if ( pSF->mnCos != 0x10000 && pSF->mnSin != 0 )
    {
        double nCos = pSF->mnCos / 65536.0;
        double nSin = pSF->mnSin / 65536.0;
        rRect.Left() =  nCos*aRect.Left() + nSin*aRect.Top();
        rRect.Top()  = -nSin*aRect.Left() - nCos*aRect.Top();

        rRect.Right()  =  nCos*aRect.Right() + nSin*aRect.Bottom();
        rRect.Bottom() = -nSin*aRect.Right() - nCos*aRect.Bottom();
    }
    else
        rRect = aRect;

    return true;
}

bool CairoTextRender::GetGlyphOutline( sal_GlyphId aGlyphId,
    ::basegfx::B2DPolyPolygon& rPolyPoly )
{
    const int nLevel = aGlyphId >> GF_FONTSHIFT;
    if( nLevel >= MAX_FALLBACK )
        return false;

    ServerFont* pSF = mpServerFont[ nLevel ];
    if( !pSF )
        return false;

    aGlyphId &= GF_IDXMASK;
    if( pSF->GetGlyphOutline( aGlyphId, rPolyPoly ) )
        return true;

    return false;
}

SalLayout* CairoTextRender::GetTextLayout( ImplLayoutArgs& rArgs, int nFallbackLevel )
{
    SalLayout* pLayout = NULL;

    if( mpServerFont[ nFallbackLevel ]
    && !(rArgs.mnFlags & SAL_LAYOUT_DISABLE_GLYPH_PROCESSING) )
    {
#if ENABLE_GRAPHITE
        // Is this a Graphite font?
        if (!bDisableGraphite_ &&
            GraphiteServerFontLayout::IsGraphiteEnabledFont(*mpServerFont[nFallbackLevel]))
        {
            pLayout = new GraphiteServerFontLayout(*mpServerFont[nFallbackLevel]);
        }
        else
#endif
            pLayout = new ServerFontLayout( *mpServerFont[ nFallbackLevel ] );
    }

    return pLayout;
}

SystemFontData CairoTextRender::GetSysFontData( int nFallbackLevel ) const
{
    SystemFontData aSysFontData;
    aSysFontData.nSize = sizeof( SystemFontData );
    aSysFontData.nFontId = 0;

    if (nFallbackLevel >= MAX_FALLBACK) nFallbackLevel = MAX_FALLBACK - 1;
    if (nFallbackLevel < 0 ) nFallbackLevel = 0;

    if (mpServerFont[nFallbackLevel] != NULL)
    {
        ServerFont* rFont = mpServerFont[nFallbackLevel];
        aSysFontData.nFontId = rFont->GetFtFace();
        aSysFontData.nFontFlags = rFont->GetLoadFlags();
        aSysFontData.bFakeBold = rFont->NeedsArtificialBold();
        aSysFontData.bFakeItalic = rFont->NeedsArtificialItalic();
        aSysFontData.bAntialias = rFont->GetAntialiasAdvice();
        aSysFontData.bVerticalCharacterType = rFont->GetFontSelData().mbVertical;
    }

    return aSysFontData;
}

bool CairoTextRender::CreateFontSubset(
                                   const OUString& rToFile,
                                   const PhysicalFontFace* pFont,
                                   sal_GlyphId* pGlyphIds,
                                   sal_uInt8* pEncoding,
                                   sal_Int32* pWidths,
                                   int nGlyphCount,
                                   FontSubsetInfo& rInfo
                                   )
{
    // in this context the pFont->GetFontId() is a valid PSP
    // font since they are the only ones left after the PDF
    // export has filtered its list of subsettable fonts (for
    // which this method was created). The correct way would
    // be to have the GlyphCache search for the PhysicalFontFace pFont
    psp::fontID aFont = pFont->GetFontId();

    psp::PrintFontManager& rMgr = psp::PrintFontManager::get();
    bool bSuccess = rMgr.createFontSubset( rInfo,
                                 aFont,
                                 rToFile,
                                 pGlyphIds,
                                 pEncoding,
                                 pWidths,
                                 nGlyphCount );
    return bSuccess;
}

const void* CairoTextRender::GetEmbedFontData( const PhysicalFontFace* pFont, const sal_Ucs* pUnicodes, sal_Int32* pWidths, FontSubsetInfo& rInfo, long* pDataLen )
{
    // in this context the pFont->GetFontId() is a valid PSP
    // font since they are the only ones left after the PDF
    // export has filtered its list of subsettable fonts (for
    // which this method was created). The correct way would
    // be to have the GlyphCache search for the PhysicalFontFace pFont
    psp::fontID aFont = pFont->GetFontId();
    return GenPspGraphics::DoGetEmbedFontData( aFont, pUnicodes, pWidths, rInfo, pDataLen );
}

void CairoTextRender::FreeEmbedFontData( const void* pData, long nLen )
{
    GenPspGraphics::DoFreeEmbedFontData( pData, nLen );
}

const Ucs2SIntMap* CairoTextRender::GetFontEncodingVector( const PhysicalFontFace* pFont, const Ucs2OStrMap** pNonEncoded )
{
    // in this context the pFont->GetFontId() is a valid PSP
    // font since they are the only ones left after the PDF
    // export has filtered its list of subsettable fonts (for
    // which this method was created). The correct way would
    // be to have the GlyphCache search for the PhysicalFontFace pFont
    psp::fontID aFont = pFont->GetFontId();
    return GenPspGraphics::DoGetFontEncodingVector( aFont, pNonEncoded );
}

void CairoTextRender::GetGlyphWidths( const PhysicalFontFace* pFont,
                                   bool bVertical,
                                   Int32Vector& rWidths,
                                   Ucs2UIntMap& rUnicodeEnc )
{
    // in this context the pFont->GetFontId() is a valid PSP
    // font since they are the only ones left after the PDF
    // export has filtered its list of subsettable fonts (for
    // which this method was created). The correct way would
    // be to have the GlyphCache search for the PhysicalFontFace pFont
    psp::fontID aFont = pFont->GetFontId();
    GenPspGraphics::DoGetGlyphWidths( aFont, bVertical, rWidths, rUnicodeEnc );
}
/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
