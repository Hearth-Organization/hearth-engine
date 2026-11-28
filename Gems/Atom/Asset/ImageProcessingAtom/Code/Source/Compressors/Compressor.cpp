/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */


#include <AzCore/PlatformIncl.h>
#include <Compressors/ASTCCompressor.h>
#include <Compressors/CTSquisher.h>
#include <Compressors/HearthRustCompressor.h>
#include <Compressors/ISPCTextureCompressor.h>

namespace ImageProcessingAtom
{
    ICompressorPtr ICompressor::FindCompressor(EPixelFormat fmt, ColorSpace colorSpace, bool isCompressing)
    {
        // Hearth Engine: BC1, BC1a, BC4 and BC5 (and decompressing BC1-BC5) are done in Rust.
        if (isCompressing ? HearthRustCompressor::IsCompressedPixelFormatSupported(fmt) : HearthRustCompressor::DoesSupportDecompress(fmt))
        {
            return ICompressorPtr(new HearthRustCompressor());
        }

        if (ISPCCompressor::IsCompressedPixelFormatSupported(fmt))
        {
            if ((isCompressing && ISPCCompressor::IsSourceColorSpaceSupported(colorSpace, fmt)) || (!isCompressing && ISPCCompressor::DoesSupportDecompress(fmt)))
            {
                return ICompressorPtr(new ISPCCompressor());
            }
        }

        if (CTSquisher::IsCompressedPixelFormatSupported(fmt))
        {
            if (isCompressing || (!isCompressing && CTSquisher::DoesSupportDecompress(fmt)))
            {
                return ICompressorPtr(new CTSquisher());
            }
        }
        
        if (ASTCCompressor::IsCompressedPixelFormatSupported(fmt))
        {
            if (isCompressing || (!isCompressing && ASTCCompressor::DoesSupportDecompress(fmt)))
            {
                return ICompressorPtr(new ASTCCompressor());
            }
        }

        return nullptr;
    }

    ICompressor::~ICompressor()
    {
    }
}; // namespace ImageProcessingAtom
