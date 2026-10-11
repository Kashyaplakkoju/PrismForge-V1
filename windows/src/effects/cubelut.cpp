#include "cubelut.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>

namespace PrismForge::Effects {
namespace {
std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
std::string_view Token(std::string_view& s) {
    s = Trim(s);
    const auto end = s.find_first_of(" \t\r\n");
    const auto token = s.substr(0, end);
    s = end == s.npos ? std::string_view{} : Trim(s.substr(end));
    return token;
}
bool Number(std::string_view token, float& value) {
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
        if (!token.empty() && (token.front() == '+' || token.front() == '-')) return false;
    }
    if (token.empty()) return false;
    double parsed = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        !std::isfinite(parsed) || std::abs(parsed) > std::numeric_limits<float>::max()) return false;
    value = static_cast<float>(parsed);
    return std::isfinite(value);
}
bool Triple(std::string_view s, RgbFloat& value) {
    return Number(Token(s), value.r) && Number(Token(s), value.g) &&
        Number(Token(s), value.b) && s.empty();
}
double Coordinate(float value, float minimum, float maximum, int size) noexcept {
    // Double subtraction/division also handles finite extreme or subnormal domains.
    if (std::isnan(value) || value <= minimum) return 0;
    if (value >= maximum) return size - 1;
    return std::clamp((static_cast<double>(value) - minimum) /
        (static_cast<double>(maximum) - minimum), 0.0, 1.0) * (size - 1);
}
}

CubeLut::CubeLut(int size, std::string title, RgbFloat minimum, RgbFloat maximum, std::vector<RgbFloat> table)
    : m_size(size), m_title(std::move(title)), m_domainMin(minimum), m_domainMax(maximum), m_table(std::move(table)) {
    for (const auto& value : m_table) {
        const float safe = std::numeric_limits<float>::max() / 16;
        if (std::abs(value.r) > safe || std::abs(value.g) > safe || std::abs(value.b) > safe) {
            m_extremeValues = true; break;
        }
    }
    auto fill = [size](PrecomputedCoord& c, double x) {
        c.i0 = static_cast<int>(x); c.i1 = std::min(c.i0 + 1, size - 1);
        c.fx = static_cast<float>(x - c.i0);
    };
    for (int i = 0; i != 256; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        fill(m_coordsR[i], Coordinate(v, minimum.r, maximum.r, size));
        fill(m_coordsG[i], Coordinate(v, minimum.g, maximum.g, size));
        fill(m_coordsB[i], Coordinate(v, minimum.b, maximum.b, size));
    }
}

std::unique_ptr<CubeLut> CubeLut::Parse(std::string_view text, std::string* error) {
    if (error) error->clear();
    size_t lineNumber = 0;
    auto fail = [&](const char* message) -> std::unique_ptr<CubeLut> {
        if (error) *error = std::string(message) + " (line " + std::to_string(lineNumber) + ")";
        return nullptr;
    };
    if (text.size() > MaxPayloadBytes) return fail("LUT exceeds 16 MiB");
    if (text.find('\0') != text.npos) return fail("Embedded NUL is unsupported");
    if (text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
    int size = 0;
    bool minSeen = false, maxSeen = false, titleSeen = false;
    RgbFloat minimum{0, 0, 0}, maximum{1, 1, 1};
    std::string title;
    std::vector<RgbFloat> table;
    while (!text.empty()) {
        ++lineNumber;
        const auto end = text.find('\n');
        auto line = Trim(text.substr(0, end));
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        // Comments may follow numbers or a title, but not terminate a quoted title.
        bool quoted = false;
        for (size_t i = 0; i != line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) { line = Trim(line.substr(0, i)); break; }
        }
        if (line.empty()) continue;
        auto rest = line;
        const auto first = Token(rest);
        std::string directive(first);
        for (auto& c : directive) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (directive == "LUT_1D_SIZE") return fail("Only 3D LUTs are supported");
        if (directive == "TITLE") {
            if (titleSeen || !table.empty() || rest.size() < 2 || rest.front() != '"' || rest.back() != '"' ||
                rest.substr(1, rest.size() - 2).find('"') != rest.npos || rest.size() > 257)
                return fail("Invalid or duplicate TITLE; use a quoted title up to 255 bytes");
            title = std::string(rest.substr(1, rest.size() - 2)); titleSeen = true;
        } else if (directive == "LUT_3D_SIZE") {
            if (size || !table.empty() || rest.empty()) return fail("Duplicate, missing or misplaced LUT_3D_SIZE");
            const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), size);
            if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + rest.size() || size < 2 || size > 65)
                return fail("LUT_3D_SIZE must be an integer from 2 to 65");
            table.reserve(static_cast<size_t>(size) * size * size);
        } else if (directive == "DOMAIN_MIN" || directive == "DOMAIN_MAX") {
            bool& seen = directive == "DOMAIN_MIN" ? minSeen : maxSeen;
            auto& domain = directive == "DOMAIN_MIN" ? minimum : maximum;
            if (seen || !table.empty() || !Triple(rest, domain)) return fail("Invalid, duplicate or misplaced domain");
            seen = true;
        } else {
            RgbFloat entry;
            if (!size || !Triple(line, entry)) return fail("Unsupported directive or invalid RGB row");
            if (table.size() >= static_cast<size_t>(size) * size * size) return fail("Too many RGB rows");
            table.push_back(entry);
        }
    }
    if (!size || table.size() != static_cast<size_t>(size) * size * size) return fail("Missing size or incomplete RGB table");
    if (!(maximum.r > minimum.r && maximum.g > minimum.g && maximum.b > minimum.b)) return fail("Invalid domain bounds");
    return std::unique_ptr<CubeLut>(new CubeLut(size, std::move(title), minimum, maximum, std::move(table)));
}

RgbFloat CubeLut::SampleCoords(const PrecomputedCoord& r, const PrecomputedCoord& g, const PrecomputedCoord& b) const noexcept {
    if (m_table.empty()) return {};
    if (!m_extremeValues) {
        // Three separable lerps reduce arithmetic on normal creative LUTs. The
        // constructor's magnitude bound keeps every float intermediate finite;
        // unusual finite extremes use the double convex-combination path below.
        auto at = [this](int x, int y, int z) -> const RgbFloat& { return m_table[x + m_size * (y + m_size * z)]; };
        auto lerp = [](const RgbFloat& a, const RgbFloat& b, float f) {
            return RgbFloat{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        };
        const auto low = lerp(lerp(at(r.i0, g.i0, b.i0), at(r.i1, g.i0, b.i0), r.fx),
            lerp(at(r.i0, g.i1, b.i0), at(r.i1, g.i1, b.i0), r.fx), g.fx);
        const auto high = lerp(lerp(at(r.i0, g.i0, b.i1), at(r.i1, g.i0, b.i1), r.fx),
            lerp(at(r.i0, g.i1, b.i1), at(r.i1, g.i1, b.i1), r.fx), g.fx);
        return lerp(low, high, b.fx);
    }
    double result[3]{};
    for (int z = 0; z != 2; ++z) for (int y = 0; y != 2; ++y) for (int x = 0; x != 2; ++x) {
        const auto& c = m_table[(x ? r.i1 : r.i0) + m_size * ((y ? g.i1 : g.i0) + m_size * (z ? b.i1 : b.i0))];
        const double weight = (x ? static_cast<double>(r.fx) : 1.0 - r.fx) *
            (y ? static_cast<double>(g.fx) : 1.0 - g.fx) * (z ? static_cast<double>(b.fx) : 1.0 - b.fx);
        result[0] += weight * c.r; result[1] += weight * c.g; result[2] += weight * c.b;
    }
    const double bound = std::numeric_limits<float>::max();
    return {static_cast<float>(std::clamp(result[0], -bound, bound)),
        static_cast<float>(std::clamp(result[1], -bound, bound)), static_cast<float>(std::clamp(result[2], -bound, bound))};
}
RgbFloat CubeLut::Sample(float r, float g, float b) const noexcept {
    auto coord = [this](float value, float low, float high) {
        const auto x = Coordinate(value, low, high, m_size);
        const auto i = static_cast<int>(x);
        return PrecomputedCoord{i, std::min(i + 1, m_size - 1), static_cast<float>(x - i)};
    };
    return SampleCoords(coord(r, m_domainMin.r, m_domainMax.r), coord(g, m_domainMin.g, m_domainMax.g),
        coord(b, m_domainMin.b, m_domainMax.b));
}
RgbFloat CubeLut::SampleBytes(uint8_t r, uint8_t g, uint8_t b) const noexcept {
    return SampleCoords(m_coordsR[r], m_coordsG[g], m_coordsB[b]);
}

struct CubeLutProcessor::Impl {
    AVFrame* output = nullptr;
    ~Impl() { av_frame_free(&output); }
    bool Prepare(int width, int height) {
        if (!output || output->width != width || output->height != height) {
            AVFrame* next = av_frame_alloc();
            if (!next) return false;
            next->format = AV_PIX_FMT_BGR24; next->width = width; next->height = height;
            if (av_frame_get_buffer(next, 64) < 0) { av_frame_free(&next); return false; }
            av_frame_free(&output); output = next;
        }
        return av_frame_make_writable(output) >= 0;
    }
};
CubeLutProcessor::CubeLutProcessor() : m_impl(std::make_unique<Impl>()) {}
CubeLutProcessor::~CubeLutProcessor() = default;
AVFrame* CubeLutProcessor::Process(AVFrame* input, const CubeLut* lut, float intensity) {
    if (!input || !lut || !std::isfinite(intensity) || intensity <= 0) return input;
    if (input->format != AV_PIX_FMT_BGR24 || !input->data[0] || input->width <= 0 || input->height <= 0 ||
        input->width > 16384 || input->height > 16384 || input->linesize[0] < input->width * 3 ||
        static_cast<int64_t>(input->linesize[0]) * input->height > (1LL << 30) ||
        input->color_trc == AVCOL_TRC_SMPTE2084 || input->color_trc == AVCOL_TRC_ARIB_STD_B67) return input;
    if (!m_impl->Prepare(input->width, input->height)) return input;
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    auto* out = m_impl->output;
    for (int y = 0; y != input->height; ++y) {
        const auto* src = input->data[0] + static_cast<ptrdiff_t>(y) * input->linesize[0];
        auto* dst = out->data[0] + static_cast<ptrdiff_t>(y) * out->linesize[0];
        for (int x = 0; x != input->width; ++x) {
            const auto c = lut->SampleBytes(src[2], src[1], src[0]);
            const float channels[]{c.b, c.g, c.r};
            for (int k = 0; k != 3; ++k) dst[k] = static_cast<uint8_t>(std::clamp(
                (1.0f - intensity) * src[k] + intensity * (std::clamp(channels[k], 0.0f, 1.0f) * 255.0f) + 0.5f, 0.0f, 255.0f));
            src += 3; dst += 3;
        }
    }
    out->pts = input->pts; out->pkt_dts = input->pkt_dts; out->duration = input->duration;
    out->time_base = input->time_base; out->best_effort_timestamp = input->best_effort_timestamp;
    out->sample_aspect_ratio = input->sample_aspect_ratio;
    out->color_range = AVCOL_RANGE_JPEG; out->colorspace = AVCOL_SPC_RGB;
    out->color_primaries = input->color_primaries; out->color_trc = input->color_trc;
    return out;
}
}
