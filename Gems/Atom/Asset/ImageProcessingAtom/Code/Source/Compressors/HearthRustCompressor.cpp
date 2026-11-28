/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include <Atom/ImageProcessing/ImageObject.h>
#include <Compressors/HearthRustCompressor.h>
#include <Processing/PixelFormatInfo.h>

#include <hearth_ffi.h>

namespace ImageProcessingAtom
{
    namespace
    {
        uint32_t BcCode(EPixelFormat fmt)
        {
            switch (fmt)
            {
            case ePixelFormat_BC1:
                return 1;
            case ePixelFormat_BC1a:
                return 2;
            case ePixelFormat_BC3:
            case ePixelFormat_BC3t:
                return 3;
            case ePixelFormat_BC4:
                return 4;
            case ePixelFormat_BC5:
                return 5;
            default:
                return 0;
            }
        }

        uint32_t SourceCode(EPixelFormat fmt)
        {
            switch (fmt)
            {
            case ePixelFormat_R8G8B8A8:
                return 1;
            case ePixelFormat_R8G8B8X8:
                return 2;
            case ePixelFormat_R8:
                return 3;
            case ePixelFormat_R32G32B32A32F:
                return 4;
            default:
                return 0;
            }
        }
    } // namespace

    bool HearthRustCompressor::IsCompressedPixelFormatSupported(EPixelFormat fmt)
    {
        // BC3 stays with the ISPC compressor (found first) until Rust does BC3 at least as well; the signed formats
        // (BC4s, BC5s) stay with CTSquisher.
        return fmt == ePixelFormat_BC1 || fmt == ePixelFormat_BC1a || fmt == ePixelFormat_BC4 || fmt == ePixelFormat_BC5;
    }

    bool HearthRustCompressor::IsUncompressedPixelFormatSupported(EPixelFormat fmt)
    {
        return SourceCode(fmt) != 0;
    }

    bool HearthRustCompressor::DoesSupportDecompress(EPixelFormat fmt)
    {
        return BcCode(fmt) != 0;
    }

    ColorSpace HearthRustCompressor::GetSupportedColorSpace([[maybe_unused]] EPixelFormat compressFormat) const
    {
        return ColorSpace::autoSelect;
    }

    const char* HearthRustCompressor::GetName() const
    {
        return "HearthRust";
    }

    EPixelFormat HearthRustCompressor::GetSuggestedUncompressedFormat(EPixelFormat compressedfmt, EPixelFormat uncompressedfmt) const
    {
        // The same choices CTSquisher made, so the steps before compression don't change.
        if (compressedfmt == ePixelFormat_BC5)
        {
            return ePixelFormat_R32G32B32A32F;
        }
        if (IsUncompressedPixelFormatSupported(uncompressedfmt))
        {
            return uncompressedfmt;
        }
        switch (uncompressedfmt)
        {
        case ePixelFormat_R8G8:
        case ePixelFormat_R16G16:
        case ePixelFormat_R8G8B8:
        case ePixelFormat_B8G8R8:
            return ePixelFormat_R8G8B8X8;
        case ePixelFormat_R16:
        case ePixelFormat_A8:
            return ePixelFormat_R8;
        case ePixelFormat_R16G16B16A16:
        case ePixelFormat_B8G8R8A8:
            return ePixelFormat_R8G8B8A8;
        case ePixelFormat_R9G9B9E5:
        case ePixelFormat_R32G32F:
        case ePixelFormat_R16G16B16A16F:
        case ePixelFormat_R16G16F:
        case ePixelFormat_R16F:
        case ePixelFormat_R32F:
            return ePixelFormat_R32G32B32A32F;
        default:
            return CPixelFormats::GetInstance().IsPixelFormatWithoutAlpha(uncompressedfmt) ? ePixelFormat_R8G8B8X8 : ePixelFormat_R8G8B8A8;
        }
    }

    IImageObjectPtr HearthRustCompressor::CompressImage(IImageObjectPtr srcImage, EPixelFormat fmtDst, const CompressOption* compressOption) const
    {
        const EPixelFormat fmtSrc = srcImage->GetPixelFormat();
        if (!IsUncompressedPixelFormatSupported(fmtSrc) || !IsCompressedPixelFormatSupported(fmtDst))
        {
            return nullptr;
        }
        const uint32_t quality = compressOption ? static_cast<uint32_t>(compressOption->compressQuality) : static_cast<uint32_t>(eQuality_Normal);
        const AZ::Vector3 w = compressOption ? compressOption->rgbWeight : AZ::Vector3(0.3333f, 0.3334f, 0.3333f);
        const float weights[3] = { w.GetX(), w.GetY(), w.GetZ() };

        IImageObjectPtr dstImage(srcImage->AllocateImage(fmtDst));
        const AZ::u32 mipCount = dstImage->GetMipCount();
        for (AZ::u32 mip = 0; mip < mipCount; ++mip)
        {
            AZ::u8* src = nullptr;
            AZ::u32 srcPitch = 0;
            srcImage->GetImagePointer(mip, src, srcPitch);
            AZ::u8* dst = nullptr;
            AZ::u32 dstPitch = 0;
            dstImage->GetImagePointer(mip, dst, dstPitch);
            if (!ht_compress(
                    BcCode(fmtDst), SourceCode(fmtSrc), src, srcImage->GetWidth(mip), srcImage->GetHeight(mip), srcPitch, quality, weights,
                    dst, dstImage->GetMipBufSize(mip)))
            {
                AZ_Error("Image Processing", false, "Hearth's texture compressor couldn't compress mip %u", mip);
                return nullptr;
            }
        }
        return dstImage;
    }

    IImageObjectPtr HearthRustCompressor::DecompressImage(IImageObjectPtr srcImage, EPixelFormat fmtDst) const
    {
        const EPixelFormat fmtSrc = srcImage->GetPixelFormat();
        if (!DoesSupportDecompress(fmtSrc) || (fmtDst != ePixelFormat_R8G8B8A8 && fmtDst != ePixelFormat_R32G32B32A32F))
        {
            return nullptr;
        }
        IImageObjectPtr dstImage(srcImage->AllocateImage(fmtDst));
        const AZ::u32 mipCount = srcImage->GetMipCount();
        for (AZ::u32 mip = 0; mip < mipCount; ++mip)
        {
            AZ::u8* src = nullptr;
            AZ::u32 srcPitch = 0;
            srcImage->GetImagePointer(mip, src, srcPitch);
            AZ::u8* dst = nullptr;
            AZ::u32 dstPitch = 0;
            dstImage->GetImagePointer(mip, dst, dstPitch);
            if (!ht_decompress(
                    BcCode(fmtSrc), src, srcImage->GetMipBufSize(mip), srcImage->GetWidth(mip), srcImage->GetHeight(mip), dst, dstPitch,
                    fmtDst == ePixelFormat_R32G32B32A32F))
            {
                AZ_Error("Image Processing", false, "Hearth's texture compressor couldn't decompress mip %u", mip);
                return nullptr;
            }
        }
        return dstImage;
    }
} // namespace ImageProcessingAtom
