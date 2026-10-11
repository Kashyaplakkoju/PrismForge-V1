#pragma once
#include "pack_manifest.h"
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace PrismForge::Effects::Packs {
enum class PreviewFormat { Unknown, Png, Jpeg, Bmp, RawRgba };
enum class PreviewColorSpace { Unknown, sRgb };
// Privately owned, straight (never premultiplied) RGBA8 in sRGB. Orientation is
// the source EXIF orientation; Pixels() are already transformed into display order.
class DecodedPreviewImage {
public:
    DecodedPreviewImage() = default;
    DecodedPreviewImage(uint32_t w, uint32_t h, PreviewFormat f,
        PreviewColorSpace c, uint32_t o, std::vector<uint8_t> pixels)
        : width_(w), height_(h), strideBytes_(w * 4), format_(f), colorSpace_(c),
          orientation_(o), pixels_(std::move(pixels)) {}
    bool IsValid() const { return width_ && height_ && width_ <= 4096 && height_ <= 4096 &&
        strideBytes_ == width_ * 4 && pixels_.size() == size_t(strideBytes_) * height_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    uint32_t StrideBytes() const { return strideBytes_; }
    PreviewFormat Format() const { return format_; }
    PreviewColorSpace ColorSpace() const { return colorSpace_; }
    uint32_t Orientation() const { return orientation_; }
    size_t PixelBytesCount() const { return pixels_.size(); }
    const std::vector<uint8_t>& Pixels() const { return pixels_; }
    const uint8_t* Data() const { return pixels_.data(); }
    const uint8_t* GetPixelAddress(uint32_t x, uint32_t y) const {
        return x < width_ && y < height_ && IsValid() ? pixels_.data() + size_t(y) * strideBytes_ + size_t(x) * 4 : nullptr;
    }
    bool GetPixelRgba(uint32_t x, uint32_t y, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
        const auto p = GetPixelAddress(x,y); if (!p) return false;
        r=p[0];g=p[1];b=p[2];a=p[3];return true;
    }
private:
    uint32_t width_=0, height_=0, strideBytes_=0;
    PreviewFormat format_=PreviewFormat::Unknown;
    PreviewColorSpace colorSpace_=PreviewColorSpace::Unknown;
    uint32_t orientation_=1;
    std::vector<uint8_t> pixels_;
};
struct PackPreviewDecodeResult {
    bool success=false;
    std::string errorReason;
    DecodedPreviewImage image;
    static PackPreviewDecodeResult Success(DecodedPreviewImage img) {
        PackPreviewDecodeResult r; r.success=true;r.image=std::move(img);return r;
    }
    static PackPreviewDecodeResult Failure(const std::string& e) {
        PackPreviewDecodeResult r;r.errorReason=e;return r;
    }
};
enum class PreviewDecodeFault { None, PixelAllocation, PixelConversion };
class PackPreviewDecoder {
public:
    static constexpr size_t kMaxEncodedSizeBytes=10*1024*1024;
    static constexpr uint32_t kMaxDimension=4096;
    static constexpr uint64_t kMaxPixelCount=16777216ULL;
    static constexpr size_t kMaxDecodedSizeBytes=64*1024*1024;
    // Asset form checks type, safe relative path, extension, exact size and SHA.
    static PackPreviewDecodeResult DecodePreviewAsset(const PackAssetEntry&, const uint8_t*, size_t);
    // Byte form performs the identical full decoding, without claiming asset identity.
    // .raw means exactly64x64 straight RGBA8 sRGB (16384 bytes), no other shape.
    // PNG supports non-interlaced legal PNG colour/depth combinations.
    // BMP supports 40-byte BI_RGB 24/32bpp. JPEG is explicitly unsupported in v1:
    // measured WIC behavior silently recovers damaged/trailing entropy.
    // ICC/non-sRGB profiles, animations, unsupported layouts and trailing bytes fail.
    static PackPreviewDecodeResult DecodePreviewBytes(const uint8_t*, size_t, const std::string&);
    // Explicit opt-in failure seams; normal production entry points always use None.
    static PackPreviewDecodeResult DecodePreviewBytesForTesting(const uint8_t*,size_t,const std::string&,PreviewDecodeFault);
    static bool ValidateAndGetDimensions(const uint8_t*,size_t,const std::string&,int&,int&,std::string&);
};
}
