#include "../../windows/src/effects/packs/pack_preview_decoder.h"
#include "../../windows/src/effects/packs/pack_validator.h"
#include "../../windows/src/effects/packs/sha256.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;
using namespace PrismForge::Effects::Packs;

static int g_assertCount = 0;
static int g_failCount = 0;
static int g_skipCount = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_assertCount++; \
    if (!(cond)) { \
        std::cerr << "  FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        g_failCount++; \
    } \
} while(0)

#define TEST_ASSERT_MSG(cond, expected, actual, msg) do { \
    g_assertCount++; \
    if (!(cond)) { \
        std::cerr << "  FAILED: " << msg << " [Expected: " << (expected) << ", Actual: " << (actual) << "] (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        g_failCount++; \
    } \
} while(0)

static std::vector<uint8_t> ReadFile(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    size_t sz = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    if (sz > 0) {
        file.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}

static fs::path FindFixturesDir(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        if (fs::exists(argv[1])) return fs::path(argv[1]);
    }
    std::vector<fs::path> candidates = {
        fs::path("tests/packs/preview_decoder_fixtures"),
        fs::path("../tests/packs/preview_decoder_fixtures"),
        fs::path("../../tests/packs/preview_decoder_fixtures"),
        fs::path("fixtures"),
        fs::path("../fixtures")
    };
    for (const auto& c : candidates) {
        if (fs::exists(c) && fs::is_directory(c)) {
            return fs::absolute(c);
        }
    }
    return fs::path("tests/packs/preview_decoder_fixtures");
}

int main(int argc, char** argv) {
    // 1. Handle command-line proofs
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--exit-proof") {
            std::cout << "Exiting with proof code 42\n";
            return 42;
        }
        if (arg1 == "--hang-proof") {
            std::cout << "Hanging child process for watchdog proof...\n";
            Sleep(30000);
            return 0;
        }
    }

    std::cout << "====================================================\n";
    std::cout << " PrismForge Preview Decoder Test Suite\n";
    std::cout << "====================================================\n";

    fs::path fixturesDir = FindFixturesDir(argc, argv);
    std::cout << "Using fixtures directory: " << fixturesDir.string() << "\n\n";

    int groupIndex = 0;

    // -------------------------------------------------------------
    // GROUP 1: Valid PNG Decoding (Lossless RGBA & Straight Alpha)
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Valid PNG Decoding (Lossless RGBA & Known Pixels)\n";
    {
        fs::path pngPath = fixturesDir / "valid_2x2_rgba.png";
        auto bytes = ReadFile(pngPath);
        TEST_ASSERT(!bytes.empty(), "Read valid_2x2_rgba.png");

        auto res = PackPreviewDecoder::DecodePreviewBytes(bytes.data(), bytes.size(), pngPath.string());
        TEST_ASSERT(res.success, "Decode valid_2x2_rgba.png succeeded: " + res.errorReason);
        if (res.success) {
            const auto& img = res.image;
            TEST_ASSERT(img.IsValid(), "Image is valid");
            TEST_ASSERT_MSG(img.Width() == 2, 2, img.Width(), "Width is 2");
            TEST_ASSERT_MSG(img.Height() == 2, 2, img.Height(), "Height is 2");
            TEST_ASSERT_MSG(img.StrideBytes() == 8, 8, img.StrideBytes(), "Stride is 8");
            TEST_ASSERT(img.Format() == PreviewFormat::Png, "Format is Png");
            TEST_ASSERT(img.ColorSpace() == PreviewColorSpace::sRgb, "ColorSpace is sRgb");
            TEST_ASSERT_MSG(img.Orientation() == 1, 1, img.Orientation(), "Orientation is 1");
            TEST_ASSERT_MSG(img.PixelBytesCount() == 16, 16, img.PixelBytesCount(), "Byte count is 16");

            // Check (0,0): Red (255, 0, 0, 255)
            uint8_t r = 0, g = 0, b = 0, a = 0;
            TEST_ASSERT(img.GetPixelRgba(0, 0, r, g, b, a), "GetPixel (0,0)");
            TEST_ASSERT(r == 255 && g == 0 && b == 0 && a == 255, "Pixel (0,0) is Red RGBA(255,0,0,255)");

            // Check (1,0): Green (0, 255, 0, 255)
            TEST_ASSERT(img.GetPixelRgba(1, 0, r, g, b, a), "GetPixel (1,0)");
            TEST_ASSERT(r == 0 && g == 255 && b == 0 && a == 255, "Pixel (1,0) is Green RGBA(0,255,0,255)");

            // Check (0,1): Blue (0, 0, 255, 255)
            TEST_ASSERT(img.GetPixelRgba(0, 1, r, g, b, a), "GetPixel (0,1)");
            TEST_ASSERT(r == 0 && g == 0 && b == 255 && a == 255, "Pixel (0,1) is Blue RGBA(0,0,255,255)");

            // Check (1,1): Straight Alpha (255, 255, 255, 128) - tests straight alpha preservation
            TEST_ASSERT(img.GetPixelRgba(1, 1, r, g, b, a), "GetPixel (1,1)");
            TEST_ASSERT(r == 255 && g == 255 && b == 0 && a == 128, "Pixel (1,1) is Semi-transparent Yellow RGBA(255,255,0,128)");
        }
    }

    // -------------------------------------------------------------
    // GROUP 2: Valid JPEG Decoding (Opaque Alpha & Known Colors with Tolerance)
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Valid JPEG Decoding (Opaque Alpha & Color Tolerance)\n";
    {
        fs::path jpgPath = fixturesDir / "valid_2x2.jpg";
        auto bytes = ReadFile(jpgPath);
        TEST_ASSERT(!bytes.empty(), "Read valid_2x2.jpg");

        auto res = PackPreviewDecoder::DecodePreviewBytes(bytes.data(), bytes.size(), jpgPath.string());
        TEST_ASSERT(res.success, "Decode valid_2x2.jpg succeeded: " + res.errorReason);
        if (res.success) {
            const auto& img = res.image;
            TEST_ASSERT(img.IsValid(), "Image is valid");
            TEST_ASSERT_MSG(img.Width() == 2, 2, img.Width(), "Width is 2");
            TEST_ASSERT_MSG(img.Height() == 2, 2, img.Height(), "Height is 2");
            TEST_ASSERT(img.Format() == PreviewFormat::Jpeg, "Format is Jpeg");

            // Milestone 4: BMP and JPEG opaque alpha is 255
            for (uint32_t y = 0; y < img.Height(); ++y) {
                for (uint32_t x = 0; x < img.Width(); ++x) {
                    uint8_t r = 0, g = 0, b = 0, a = 0;
                    TEST_ASSERT(img.GetPixelRgba(x, y, r, g, b, a), "GetPixel JPEG");
                    TEST_ASSERT_MSG(a == 255, 255, a, "JPEG pixel alpha must be 255");
                }
            }

            uint8_t r00 = 0, g00 = 0, b00 = 0, a00 = 0;
            img.GetPixelRgba(0, 0, r00, g00, b00, a00);
            uint8_t r10 = 0, g10 = 0, b10 = 0, a10 = 0;
            img.GetPixelRgba(1, 0, r10, g10, b10, a10);
            uint8_t r01 = 0, g01 = 0, b01 = 0, a01 = 0;
            img.GetPixelRgba(0, 1, r01, g01, b01, a01);
            uint8_t r11 = 0, g11 = 0, b11 = 0, a11 = 0;
            img.GetPixelRgba(1, 1, r11, g11, b11, a11);

            // Verify known colors with justified JPEG tolerance (<= 15 delta from lossless)
            TEST_ASSERT(r00 >= 240 && g00 <= 15 && b00 <= 15, "JPEG (0,0) is Red within tolerance");
            TEST_ASSERT(r10 <= 15 && g10 >= 240 && b10 <= 15, "JPEG (1,0) is Green within tolerance");
            TEST_ASSERT(r01 <= 15 && g01 <= 15 && b01 >= 240, "JPEG (0,1) is Blue within tolerance");
            TEST_ASSERT(r11 >= 240 && g11 >= 240 && b11 <= 15, "JPEG (1,1) is Yellow within tolerance");
        }
    }

    // -------------------------------------------------------------
    // GROUP 3: Valid BMP Decoding (Lossless Colors & Opaque Alpha)
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Valid BMP Decoding (Lossless Colors & Opaque Alpha)\n";
    {
        fs::path bmpPath = fixturesDir / "valid_2x2.bmp";
        auto bytes = ReadFile(bmpPath);
        TEST_ASSERT(!bytes.empty(), "Read valid_2x2.bmp");

        auto res = PackPreviewDecoder::DecodePreviewBytes(bytes.data(), bytes.size(), bmpPath.string());
        TEST_ASSERT(res.success, "Decode valid_2x2.bmp succeeded: " + res.errorReason);
        if (res.success) {
            const auto& img = res.image;
            TEST_ASSERT(img.IsValid(), "Image is valid");
            TEST_ASSERT_MSG(img.Width() == 2, 2, img.Width(), "Width is 2");
            TEST_ASSERT_MSG(img.Height() == 2, 2, img.Height(), "Height is 2");
            TEST_ASSERT(img.Format() == PreviewFormat::Bmp, "Format is Bmp");

            // Milestone 4: BMP opaque alpha is 255
            for (uint32_t y = 0; y < img.Height(); ++y) {
                for (uint32_t x = 0; x < img.Width(); ++x) {
                    uint8_t r = 0, g = 0, b = 0, a = 0;
                    TEST_ASSERT(img.GetPixelRgba(x, y, r, g, b, a), "GetPixel BMP");
                    TEST_ASSERT_MSG(a == 255, 255, a, "BMP pixel alpha must be 255");
                }
            }

            // Verify exact pixel values in BMP
            uint8_t r = 0, g = 0, b = 0, a = 0;
            img.GetPixelRgba(0, 0, r, g, b, a);
            TEST_ASSERT(r == 255 && g == 0 && b == 0, "BMP (0,0) is Red");
            img.GetPixelRgba(1, 0, r, g, b, a);
            TEST_ASSERT(r == 0 && g == 255 && b == 0, "BMP (1,0) is Green");
            img.GetPixelRgba(0, 1, r, g, b, a);
            TEST_ASSERT(r == 0 && g == 0 && b == 255, "BMP (0,1) is Blue");
            img.GetPixelRgba(1, 1, r, g, b, a);
            TEST_ASSERT(r == 255 && g == 255 && b == 0, "BMP (1,1) is Yellow");
        }
    }

    // -------------------------------------------------------------
    // GROUP 4: Strict Square RAW Compatibility & Sample Pack Decoding
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Strict Square RAW Compatibility & Sample Packs\n";
    {
        // 64x64 fixture
        fs::path rawPath = fixturesDir / "valid_64x64.raw";
        auto bytes = ReadFile(rawPath);
        TEST_ASSERT(!bytes.empty(), "Read valid_64x64.raw");
        auto res = PackPreviewDecoder::DecodePreviewBytes(bytes.data(), bytes.size(), rawPath.string());
        TEST_ASSERT(res.success, "Decode valid_64x64.raw succeeded: " + res.errorReason);
        if (res.success) {
            TEST_ASSERT_MSG(res.image.Width() == 64, 64, res.image.Width(), "Raw width is 64");
            TEST_ASSERT_MSG(res.image.Height() == 64, 64, res.image.Height(), "Raw height is 64");
            TEST_ASSERT_MSG(res.image.StrideBytes() == 256, 256, res.image.StrideBytes(), "Raw stride is 256");
            TEST_ASSERT(res.image.Format() == PreviewFormat::RawRgba, "Raw format is RawRgba");
        }

        // Check genuine sample pack preview
        fs::path sampleRaw = fs::path("windows/src/effects/packs/samples/pack_cool_broadcast/preview.raw");
        if (fs::exists(sampleRaw)) {
            auto sBytes = ReadFile(sampleRaw);
            TEST_ASSERT(sBytes.size() == 16384, "Sample preview.raw is exactly 16384 bytes");
            auto sRes = PackPreviewDecoder::DecodePreviewBytes(sBytes.data(), sBytes.size(), sampleRaw.string());
            TEST_ASSERT(sRes.success, "Decode genuine sample preview.raw succeeded");
            if (sRes.success) {
                TEST_ASSERT(sRes.image.Width() == 64 && sRes.image.Height() == 64, "Genuine sample is 64x64");
            }
        } else {
            std::cout << "  [INFO] sample raw path relative to cwd not found; tested fixture 64x64\n";
        }

        // Small 2x2 RAW (16 bytes)
        std::vector<uint8_t> smallRaw(16, 200);
        auto smallRes = PackPreviewDecoder::DecodePreviewBytes(smallRaw.data(), smallRaw.size(), "test.raw");
        TEST_ASSERT(smallRes.success, "Small 2x2 RAW decoded successfully");
        if (smallRes.success) {
            TEST_ASSERT(smallRes.image.Width() == 2 && smallRes.image.Height() == 2, "2x2 dimensions");
        }
    }

    // -------------------------------------------------------------
    // GROUP 5: PNG Structural Rejection (32b Fragment, Corrupt CRC, Missing IEND, APNG)
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": PNG Structural Rejection (Header-Only, Corrupt CRC, APNG)\n";
    {
        // 1. 32-byte header fragment (must be rejected!)
        fs::path fragPath = fixturesDir / "header_fragment_32b.png";
        auto fragBytes = ReadFile(fragPath);
        TEST_ASSERT(fragBytes.size() == 32, "Header fragment is 32 bytes");
        auto fragRes = PackPreviewDecoder::DecodePreviewBytes(fragBytes.data(), fragBytes.size(), fragPath.string());
        TEST_ASSERT(!fragRes.success, "32-byte PNG header fragment must be rejected");
        TEST_ASSERT(!fragRes.image.IsValid(), "Output image is cleared/invalid");

        // 2. Corrupt CRC in PNG chunk
        fs::path crcPath = fixturesDir / "corrupt_crc.png";
        auto crcBytes = ReadFile(crcPath);
        auto crcRes = PackPreviewDecoder::DecodePreviewBytes(crcBytes.data(), crcBytes.size(), crcPath.string());
        TEST_ASSERT(!crcRes.success, "Corrupt PNG CRC must be rejected");
        TEST_ASSERT(crcRes.errorReason.find("CRC") != std::string::npos, "Error mentions CRC mismatch");

        // 3. Missing IEND marker
        fs::path iendPath = fixturesDir / "missing_iend.png";
        auto iendBytes = ReadFile(iendPath);
        auto iendRes = PackPreviewDecoder::DecodePreviewBytes(iendBytes.data(), iendBytes.size(), iendPath.string());
        TEST_ASSERT(!iendRes.success, "Missing PNG IEND marker must be rejected");

        // 4. APNG / animated PNG rejection
        fs::path apngPath = fixturesDir / "apng_animated.png";
        auto apngBytes = ReadFile(apngPath);
        auto apngRes = PackPreviewDecoder::DecodePreviewBytes(apngBytes.data(), apngBytes.size(), apngPath.string());
        TEST_ASSERT(!apngRes.success, "APNG animation must be rejected");
        TEST_ASSERT(apngRes.errorReason.find("APNG") != std::string::npos ||
                    apngRes.errorReason.find("animated") != std::string::npos, "Error mentions APNG/animated");
    }

    // -------------------------------------------------------------
    // GROUP 6: JPEG & BMP Structural Rejection (Missing EOI, Truncated BMP)
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": JPEG & BMP Structural Rejection (Missing EOI, Truncated BMP)\n";
    {
        // Missing JPEG EOI marker
        fs::path noEoiPath = fixturesDir / "missing_eoi.jpg";
        auto noEoiBytes = ReadFile(noEoiPath);
        auto noEoiRes = PackPreviewDecoder::DecodePreviewBytes(noEoiBytes.data(), noEoiBytes.size(), noEoiPath.string());
        TEST_ASSERT(!noEoiRes.success, "Missing JPEG EOI must be rejected");
        TEST_ASSERT(noEoiRes.errorReason.find("EOI") != std::string::npos ||
                    noEoiRes.errorReason.find("truncated") != std::string::npos, "Error mentions EOI or truncated");

        // Truncated BMP pixel data
        fs::path truncBmpPath = fixturesDir / "truncated.bmp";
        auto truncBmpBytes = ReadFile(truncBmpPath);
        auto truncBmpRes = PackPreviewDecoder::DecodePreviewBytes(truncBmpBytes.data(), truncBmpBytes.size(), truncBmpPath.string());
        TEST_ASSERT(!truncBmpRes.success, "Truncated BMP pixel data must be rejected");

        // Corrupt BMP magic
        std::vector<uint8_t> badMagicBmp = truncBmpBytes;
        badMagicBmp[0] = 'X'; badMagicBmp[1] = 'Y';
        auto badMagicRes = PackPreviewDecoder::DecodePreviewBytes(badMagicBmp.data(), badMagicBmp.size(), "bad.bmp");
        TEST_ASSERT(!badMagicRes.success, "BMP with bad magic must be rejected");
    }

    // -------------------------------------------------------------
    // GROUP 7: Arbitrary Text & Malformed RAW Rejection
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Arbitrary Text & Malformed RAW Rejection\n";
    {
        // Arbitrary text disguised as raw
        fs::path rawTextPath = fixturesDir / "arbitrary_text.raw";
        auto rawTextBytes = ReadFile(rawTextPath);
        auto rawTextRes = PackPreviewDecoder::DecodePreviewBytes(rawTextBytes.data(), rawTextBytes.size(), rawTextPath.string());
        TEST_ASSERT(!rawTextRes.success, "Arbitrary text .raw must be rejected");
        TEST_ASSERT(rawTextRes.errorReason.find("square") != std::string::npos, "Error mentions square contract");

        // Arbitrary text disguised as png
        fs::path pngTextPath = fixturesDir / "arbitrary_text.png";
        auto pngTextBytes = ReadFile(pngTextPath);
        auto pngTextRes = PackPreviewDecoder::DecodePreviewBytes(pngTextBytes.data(), pngTextBytes.size(), pngTextPath.string());
        TEST_ASSERT(!pngTextRes.success, "Arbitrary text .png must be rejected");

        // Non-multiple-of-4 raw
        std::vector<uint8_t> oddRaw(15, 'A');
        auto oddRes = PackPreviewDecoder::DecodePreviewBytes(oddRaw.data(), oddRaw.size(), "odd.raw");
        TEST_ASSERT(!oddRes.success, "Non-multiple-of-4 raw must be rejected");

        // Multiple of 4 but non-square raw (e.g. 24 bytes = 6 pixels, not square)
        std::vector<uint8_t> nonSquareRaw(24, 0);
        auto nsRes = PackPreviewDecoder::DecodePreviewBytes(nonSquareRaw.data(), nonSquareRaw.size(), "nonsquare.raw");
        TEST_ASSERT(!nsRes.success, "Non-square raw must be rejected");
    }

    // -------------------------------------------------------------
    // GROUP 8: Dimension Bombs, Oversized Input & Bounds Limits
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Dimension Bombs, Oversized Input & Bounds Limits\n";
    {
        // Dimension bomb PNG (65535x65535) - must reject BEFORE allocation!
        fs::path bombPath = fixturesDir / "huge_dim_bomb.png";
        auto bombBytes = ReadFile(bombPath);
        auto bombRes = PackPreviewDecoder::DecodePreviewBytes(bombBytes.data(), bombBytes.size(), bombPath.string());
        TEST_ASSERT(!bombRes.success, "Dimension bomb PNG must be rejected");
        TEST_ASSERT(bombRes.errorReason.find("bounds") != std::string::npos, "Error mentions bounds (1..4096)");

        // Encoded input exceeding 10 MiB limit
        std::vector<uint8_t> hugeInput(10 * 1024 * 1024 + 1, 0);
        auto hugeRes = PackPreviewDecoder::DecodePreviewBytes(hugeInput.data(), hugeInput.size(), "huge.png");
        TEST_ASSERT(!hugeRes.success, "Input > 10 MiB must be rejected");
        TEST_ASSERT(hugeRes.errorReason.find("10 MB") != std::string::npos, "Error mentions 10 MB limit");

        // Null / empty buffer
        auto nullRes = PackPreviewDecoder::DecodePreviewBytes(nullptr, 0, "empty.png");
        TEST_ASSERT(!nullRes.success, "Empty buffer must be rejected");
    }

    // -------------------------------------------------------------
    // GROUP 9: Format Mismatch & Extension Spoofing Rejection
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Format Mismatch & Extension Spoofing Rejection\n";
    {
        // PNG bytes passed with .jpg extension
        fs::path pngPath = fixturesDir / "valid_2x2_rgba.png";
        auto pngBytes = ReadFile(pngPath);
        auto spoof1 = PackPreviewDecoder::DecodePreviewBytes(pngBytes.data(), pngBytes.size(), "spoof.jpg");
        TEST_ASSERT(!spoof1.success, "PNG data with .jpg extension must be rejected");

        // JPEG bytes passed with .png extension
        fs::path jpgPath = fixturesDir / "valid_2x2.jpg";
        auto jpgBytes = ReadFile(jpgPath);
        auto spoof2 = PackPreviewDecoder::DecodePreviewBytes(jpgBytes.data(), jpgBytes.size(), "spoof.png");
        TEST_ASSERT(!spoof2.success, "JPEG data with .png extension must be rejected");

        // BMP bytes passed with .raw extension
        fs::path bmpPath = fixturesDir / "valid_2x2.bmp";
        auto bmpBytes = ReadFile(bmpPath);
        auto spoof3 = PackPreviewDecoder::DecodePreviewBytes(bmpBytes.data(), bmpBytes.size(), "spoof.raw");
        TEST_ASSERT(!spoof3.success, "BMP data with .raw extension must be rejected");

        // Unsupported extension (.exe)
        auto unsupp = PackPreviewDecoder::DecodePreviewBytes(pngBytes.data(), pngBytes.size(), "image.exe");
        TEST_ASSERT(!unsupp.success, "Prohibited extension (.exe) must be rejected");
    }

    // -------------------------------------------------------------
    // GROUP 10: Asset Descriptor Verification, EXIF Orientation & Immutable Lifetime
    // -------------------------------------------------------------
    groupIndex++;
    std::cout << "Group " << groupIndex << ": Asset Verification, EXIF Orientation & Immutable Lifetime\n";
    {
        // 1. PackAssetEntry validation with correct SHA-256 and size
        fs::path pngPath = fixturesDir / "valid_2x2_rgba.png";
        auto pngBytes = ReadFile(pngPath);
        std::string actualHash = Sha256::ComputeHex(pngBytes.data(), pngBytes.size());

        PackAssetEntry validEntry;
        validEntry.assetType = PackAssetType::PreviewImage;
        validEntry.assetId = "test_preview";
        validEntry.relativePath = "previews/preview.png";
        validEntry.sizeBytes = pngBytes.size();
        validEntry.sha256Hex = actualHash;

        auto assetRes = PackPreviewDecoder::DecodePreviewAsset(validEntry, pngBytes.data(), pngBytes.size());
        TEST_ASSERT(assetRes.success, "DecodePreviewAsset with valid entry succeeded");

        // 2. Asset entry with SHA-256 mismatch
        PackAssetEntry badHashEntry = validEntry;
        badHashEntry.sha256Hex = "0000000000000000000000000000000000000000000000000000000000000000";
        auto badHashRes = PackPreviewDecoder::DecodePreviewAsset(badHashEntry, pngBytes.data(), pngBytes.size());
        TEST_ASSERT(!badHashRes.success, "DecodePreviewAsset with bad hash must be rejected");
        TEST_ASSERT(badHashRes.errorReason.find("integrity") != std::string::npos ||
                    badHashRes.errorReason.find("SHA-256") != std::string::npos, "Error mentions SHA-256");

        // 3. Asset entry with size mismatch
        PackAssetEntry badSizeEntry = validEntry;
        badSizeEntry.sizeBytes = pngBytes.size() + 10;
        auto badSizeRes = PackPreviewDecoder::DecodePreviewAsset(badSizeEntry, pngBytes.data(), pngBytes.size());
        TEST_ASSERT(!badSizeRes.success, "DecodePreviewAsset with bad size must be rejected");

        // 4. Asset entry with wrong asset type
        PackAssetEntry wrongTypeEntry = validEntry;
        wrongTypeEntry.assetType = PackAssetType::CubeLut;
        auto wrongTypeRes = PackPreviewDecoder::DecodePreviewAsset(wrongTypeEntry, pngBytes.data(), pngBytes.size());
        TEST_ASSERT(!wrongTypeRes.success, "DecodePreviewAsset with wrong asset type must be rejected");

        // 5. EXIF Orientation Test (4x2 image with Orientation 6 -> transformed to 2x4)
        fs::path exifPath = fixturesDir / "exif_rot90.jpg";
        if (fs::exists(exifPath)) {
            auto exifBytes = ReadFile(exifPath);
            auto exifRes = PackPreviewDecoder::DecodePreviewBytes(exifBytes.data(), exifBytes.size(), exifPath.string());
            TEST_ASSERT(exifRes.success, "Decode EXIF rotated image succeeded");
            if (exifRes.success) {
                // EXIF 6 rotates 90 deg clockwise: dimensions 4x2 become 2x4!
                TEST_ASSERT_MSG(exifRes.image.Width() == 2, 2, exifRes.image.Width(), "EXIF rotated width is 2");
                TEST_ASSERT_MSG(exifRes.image.Height() == 4, 4, exifRes.image.Height(), "EXIF rotated height is 4");
                TEST_ASSERT_MSG(exifRes.image.Orientation() == 6, 6, exifRes.image.Orientation(), "EXIF orientation tag recorded as 6");
            }
        } else {
            g_skipCount++;
            std::cout << "  [SKIP] exif_rot90.jpg not found\n";
        }

        // 6. Immutable Lifetime & Independence Proof
        // Verify that mutating/clearing caller buffer does NOT affect DecodedPreviewImage
        {
            std::vector<uint8_t> callerBuf = pngBytes;
            auto decodeLifetimeRes = PackPreviewDecoder::DecodePreviewBytes(callerBuf.data(), callerBuf.size(), "test.png");
            TEST_ASSERT(decodeLifetimeRes.success, "Decode for lifetime test succeeded");

            DecodedPreviewImage img = std::move(decodeLifetimeRes.image);
            TEST_ASSERT(img.IsValid(), "Image is valid before caller mutation");

            // Zero out caller buffer
            std::fill(callerBuf.begin(), callerBuf.end(), 0);
            callerBuf.clear();

            // Assert that pixel data in img is completely unchanged!
            uint8_t r = 0, g = 0, b = 0, a = 0;
            TEST_ASSERT(img.GetPixelRgba(0, 0, r, g, b, a), "GetPixel after caller buffer destruction");
            TEST_ASSERT(r == 255 && g == 0 && b == 0 && a == 255, "Pixel (0,0) remains Red (255,0,0,255) after caller destroyed");

            // Test move constructor and assignment
            DecodedPreviewImage movedImg = std::move(img);
            TEST_ASSERT(movedImg.IsValid(), "Moved image is valid");
            TEST_ASSERT(movedImg.GetPixelRgba(0, 0, r, g, b, a), "GetPixel on moved image");
            TEST_ASSERT(r == 255 && g == 0 && b == 0 && a == 255, "Pixel preserved after move");
        }

        // 7. ValidateAndGetDimensions delegation check
        {
            int outW = -1, outH = -1;
            std::string outErr;
            bool vRes = PackValidator::ValidatePreviewImageBytes(pngBytes.data(), pngBytes.size(), "test.png", outW, outH, outErr);
            TEST_ASSERT(vRes, "PackValidator::ValidatePreviewImageBytes succeeded for valid PNG");
            TEST_ASSERT_MSG(outW == 2, 2, outW, "ValidatePreviewImageBytes width 2");
            TEST_ASSERT_MSG(outH == 2, 2, outH, "ValidatePreviewImageBytes height 2");

            // Reject 32b fragment through PackValidator::ValidatePreviewImageBytes as well
            fs::path fragPath = fixturesDir / "header_fragment_32b.png";
            auto fragBytes = ReadFile(fragPath);
            int badW = -1, badH = -1;
            std::string badErr;
            bool vFragRes = PackValidator::ValidatePreviewImageBytes(fragBytes.data(), fragBytes.size(), "test.png", badW, badH, badErr);
            TEST_ASSERT(!vFragRes, "PackValidator::ValidatePreviewImageBytes rejects 32b fragment");
            TEST_ASSERT(badW == 0 && badH == 0, "Out dimensions cleared to 0 on rejection");
            TEST_ASSERT(!badErr.empty(), "Error reason returned on rejection");
        }
    }

    std::cout << "\n====================================================\n";
    std::cout << " TEST SUMMARY\n";
    std::cout << " Total Assertions: " << g_assertCount << "\n";
    std::cout << " Failed:           " << g_failCount << "\n";
    std::cout << " Skipped:          " << g_skipCount << "\n";
    std::cout << " Result:           " << (g_failCount == 0 ? "PASSED" : "FAILED") << "\n";
    std::cout << "====================================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
