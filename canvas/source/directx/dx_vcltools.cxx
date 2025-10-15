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

#include <sal/config.h>

#include <basegfx/numeric/ftools.hxx>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <com/sun/star/rendering/XIntegerBitmap.hpp>
#include <comphelper/diagnose_ex.hxx>
#include <vcl/bitmap.hxx>
#include <vcl/BitmapReadAccess.hxx>
#include <vcl/canvastools.hxx>

#include "dx_impltools.hxx"
#include "dx_vcltools.hxx"

using namespace ::com::sun::star;

namespace dxcanvastools
{
        namespace
        {
            /// Calc number of colors in given BitmapInfoHeader
            sal_Int32 calcDIBColorCount( const BITMAPINFOHEADER& rBIH )
            {
                if( rBIH.biSize != sizeof( BITMAPCOREHEADER ) )
                {
                    if( rBIH.biBitCount <= 8 )
                    {
                        if( rBIH.biClrUsed )
                            return rBIH.biClrUsed;
                        else
                            return 1 << rBIH.biBitCount;
                    }
                }
                else
                {
                    BITMAPCOREHEADER const * pCoreHeader = reinterpret_cast<BITMAPCOREHEADER const *>(&rBIH);

                    if( pCoreHeader->bcBitCount <= 8 )
                        return 1 << pCoreHeader->bcBitCount;
                }

                return 0; // nothing known
            }

            /// Draw DI bits to given Graphics
            bool drawDIBits( const std::shared_ptr< Gdiplus::Graphics >& rGraphics,
                             void*                                       pDIB )
            {
                bool            bRet( false );

                const BITMAPINFO* pBI = static_cast<BITMAPINFO*>(pDIB);

                if( pBI )
                {
                    const BYTE*             pBits = reinterpret_cast<BYTE const *>(pBI) + pBI->bmiHeader.biSize +
                        calcDIBColorCount( pBI->bmiHeader ) * sizeof( RGBQUAD );

                    // forward to outsourced GDI+ rendering method
                    // (header clashes)
                    bRet = dxcanvastools::drawDIBits( rGraphics, *pBI, pBits );
                }

                return bRet;
            }

            /** Draw VCL bitmap to given Graphics

                @param rBmp
                Reference to bitmap. Might get modified, in such a way
                that it will hold a DIB after a successful function call.
             */
            bool drawVCLBitmap( const std::shared_ptr< Gdiplus::Graphics >& rGraphics,
                                ::Bitmap&                                       rBmp )
            {
                BitmapSystemData aBmpSysData;

                if( !rBmp.GetSystemData( aBmpSysData ) ||
                    !aBmpSysData.pDIB )
                {
                    // first of all, ensure that Bitmap contains a DIB, by
                    // acquiring a read access
                    BitmapScopedReadAccess pReadAcc(rBmp);

                    // TODO(P2): Acquiring a read access can actually
                    // force a read from VRAM, thus, avoiding this
                    // step somehow will increase performance
                    // here.
                    if( pReadAcc )
                    {
                        // try again: now, WinSalBitmap must have
                        // generated a DIB
                        if( rBmp.GetSystemData( aBmpSysData ) &&
                            aBmpSysData.pDIB )
                        {
                            return drawDIBits( rGraphics,
                                               aBmpSysData.pDIB );
                        }
                    }
                }
                else
                {
                    return drawDIBits( rGraphics,
                                       aBmpSysData.pDIB );
                }

                // failed to generate DIBits from vcl bitmap
                return false;
            }

            /** Create a chunk of raw RGBA data GDI+ Bitmap from VCL Bitmap
             */
            RawRGBABitmap bitmapFromVCLBitmap( const ::Bitmap& rBmp )
            {
                // TODO(P2): Avoid temporary bitmap generation, maybe
                // even ensure that created DIBs are copied back to
                // BmpEx (currently, every AcquireReadAccess() will
                // make the local bitmap copy unique, effectively
                // duplicating the memory used)

                // convert transparent bitmap to 32bit RGBA
                // ========================================

                const ::Size aBmpSize( rBmp.GetSizePixel() );

                RawRGBABitmap aBmpData;
                aBmpData.mnWidth     = aBmpSize.Width();
                aBmpData.mnHeight    = aBmpSize.Height();
                aBmpData.maBitmapData.resize(4*aBmpData.mnWidth*aBmpData.mnHeight);

                BitmapScopedReadAccess pReadAccess( rBmp );

                const sal_Int32 nWidth( aBmpSize.Width() );
                const sal_Int32 nHeight( aBmpSize.Height() );

                ENSURE_OR_THROW( pReadAccess.get() != nullptr,
                                  "::dxcanvastools::bitmapFromVCLBitmap(): "
                                  "Unable to acquire read access to bitmap" );

                sal_uInt8*      pCurrOutput(aBmpData.maBitmapData.data());

                for( int y=0; y<nHeight; ++y )
                {
                    switch( pReadAccess->GetScanlineFormat() )
                    {
                        case ScanlineFormat::N8BitPal:
                            {
                                Scanline pScan  = pReadAccess->GetScanline( y );

                                for( int x=0; x<nWidth; ++x )
                                {
                                    BitmapColor aCol = pReadAccess->GetPaletteColor( *pScan++ );

                                    *pCurrOutput++ = aCol.GetBlue();
                                    *pCurrOutput++ = aCol.GetGreen();
                                    *pCurrOutput++ = aCol.GetRed();
                                    *pCurrOutput++ = 0xff;
                                }
                            }
                            break;

                        case ScanlineFormat::N24BitTcBgr:
                            {
                                Scanline pScan  = pReadAccess->GetScanline( y );

                                for( int x=0; x<nWidth; ++x )
                                {
                                    // store as RGBA
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = 0xff;
                                }
                            }
                            break;

                        case ScanlineFormat::N32BitTcBgra:
                            {
                                Scanline pScan  = pReadAccess->GetScanline( y );

                                for( int x=0; x<nWidth; ++x )
                                {
                                    // store as RGBA
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = *pScan++;
                                    *pCurrOutput++ = *pScan++;
                                }
                            }
                            break;

                        case ScanlineFormat::N24BitTcRgb:
                        case ScanlineFormat::N32BitTcAbgr:
                        case ScanlineFormat::N32BitTcArgb:
                        case ScanlineFormat::N32BitTcRgba:
                        case ScanlineFormat::N32BitTcXbgr:
                        case ScanlineFormat::N32BitTcXrgb:
                        case ScanlineFormat::N32BitTcBgrx:
                        case ScanlineFormat::N32BitTcRgbx:
                        default:
                            ENSURE_OR_THROW( false,
                                            "::dxcanvastools::bitmapFromVCLBitmap(): "
                                            "Unexpected scanline format - has "
                                            "WinSalBitmap::AcquireBuffer() changed?" );
                    }
                }

                return aBmpData;
            }

            bool drawVCLBitmap( const std::shared_ptr< Gdiplus::Graphics >& rGraphics,
                                const ::Bitmap&                             rBmp )
            {
                return drawRGBABits( rGraphics,
                                     bitmapFromVCLBitmap( rBmp ) );
            }
        }

        bool drawVCLBitmapFromXBitmap( const std::shared_ptr< Gdiplus::Graphics >& rGraphics,
                                       const uno::Reference< rendering::XBitmap >&     xBitmap )
        {
            // TODO(F2): add support for floating point bitmap formats
            uno::Reference< rendering::XIntegerReadOnlyBitmap > xIntBmp(
                xBitmap, uno::UNO_QUERY );

            if( !xIntBmp.is() )
                return false;

            ::Bitmap aBmp = vcl::unotools::bitmapFromXBitmap( xIntBmp );
            if( aBmp.IsEmpty() )
                return false;

            return drawVCLBitmap( rGraphics, aBmp );
        }
}


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
