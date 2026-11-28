/*
 * Hearth Engine: BC1, BC1a, BC4 and BC5 compression (and BC1-BC5 decompression) in Rust (hearth-rust/hearth-texture,
 * through hearth-ffi), in place of libsquish. The same algorithms (texpresso is a port of squish), so the same quality.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include <Compressors/Compressor.h>

namespace ImageProcessingAtom
{
    class HearthRustCompressor : public ICompressor
    {
    public:
        static bool IsCompressedPixelFormatSupported(EPixelFormat fmt);
        static bool IsUncompressedPixelFormatSupported(EPixelFormat fmt);
        static bool DoesSupportDecompress(EPixelFormat fmt);

        IImageObjectPtr CompressImage(IImageObjectPtr srcImage, EPixelFormat fmtDst, const CompressOption* compressOption) const override;
        IImageObjectPtr DecompressImage(IImageObjectPtr srcImage, EPixelFormat fmtDst) const override;
        EPixelFormat GetSuggestedUncompressedFormat(EPixelFormat compressedfmt, EPixelFormat uncompressedfmt) const override;
        ColorSpace GetSupportedColorSpace(EPixelFormat compressFormat) const final;
        const char* GetName() const final;
    };
} // namespace ImageProcessingAtom
