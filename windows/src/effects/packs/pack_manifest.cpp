#include "pack_manifest.h"
#include "pack_validator.h"

#include <fstream>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace PrismForge::Effects::Packs {

namespace fs = std::filesystem;

namespace {

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string AssetTypeToString(PackAssetType type) {
    switch (type) {
        case PackAssetType::CubeLut: return "CubeLut";
        case PackAssetType::TrackedAccessory: return "TrackedAccessory";
        case PackAssetType::ScreenOverlay: return "ScreenOverlay";
        case PackAssetType::PresetConfig: return "PresetConfig";
        case PackAssetType::PreviewImage: return "PreviewImage";
        default: return "Unknown";
    }
}

bool StringToAssetType(const std::string& s, PackAssetType& outType) {
    if (s == "CubeLut" || s == "cubelut" || s == "lut") {
        outType = PackAssetType::CubeLut;
        return true;
    }
    if (s == "TrackedAccessory" || s == "trackedaccessory" || s == "accessory") {
        outType = PackAssetType::TrackedAccessory;
        return true;
    }
    if (s == "ScreenOverlay" || s == "screenoverlay" || s == "overlay") {
        outType = PackAssetType::ScreenOverlay;
        return true;
    }
    if (s == "PresetConfig" || s == "presetconfig" || s == "preset") {
        outType = PackAssetType::PresetConfig;
        return true;
    }
    if (s == "PreviewImage" || s == "previewimage" || s == "preview") {
        outType = PackAssetType::PreviewImage;
        return true;
    }
    return false;
}

bool ValidateUtf8String(const std::string& str, std::string& outError) {
    size_t i = 0;
    while (i < str.size()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c <= 0x7F) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2) { outError = "Overlong UTF-8 encoding"; return false; }
            if (i + 1 >= str.size()) { outError = "Truncated UTF-8 sequence"; return false; }
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);
            if ((c2 & 0xC0) != 0x80) { outError = "Invalid UTF-8 continuation byte"; return false; }
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= str.size()) { outError = "Truncated UTF-8 sequence"; return false; }
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(str[i + 2]);
            if (c == 0xE0 && c2 < 0xA0) { outError = "Overlong UTF-8 sequence"; return false; }
            if (c == 0xED && c2 >= 0xA0) { outError = "UTF-8 surrogate codepoint prohibited"; return false; }
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) { outError = "Invalid UTF-8 continuation byte"; return false; }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (c > 0xF4) { outError = "UTF-8 codepoint exceeds U+10FFFF"; return false; }
            if (i + 3 >= str.size()) { outError = "Truncated UTF-8 sequence"; return false; }
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(str[i + 2]);
            unsigned char c4 = static_cast<unsigned char>(str[i + 3]);
            if (c == 0xF0 && c2 < 0x90) { outError = "Overlong UTF-8 sequence"; return false; }
            if (c == 0xF4 && c2 > 0x8F) { outError = "UTF-8 codepoint exceeds U+10FFFF"; return false; }
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) { outError = "Invalid UTF-8 continuation byte"; return false; }
            i += 4;
        } else {
            outError = "Invalid UTF-8 leading byte";
            return false;
        }
    }
    return true;
}

enum class TokenType {
    None,
    OpenBrace,
    CloseBrace,
    OpenBracket,
    CloseBracket,
    Colon,
    Comma,
    String,
    Number,
    Boolean,
    Null,
    EndOfFile,
    Error
};

struct Token {
    TokenType type = TokenType::None;
    std::string value;
    int line = 1;
    std::string errorMsg;
};

class StrictJsonTokenizer {
public:
    explicit StrictJsonTokenizer(const std::string& src) : m_src(src) {}

    Token NextToken() {
        SkipWhitespace();
        if (m_hasError) {
            return Token{ TokenType::Error, "", m_line, m_lastError };
        }
        if (m_pos >= m_src.size()) {
            return Token{ TokenType::EndOfFile, "", m_line };
        }

        char c = m_src[m_pos];
        if (c == '{') { m_pos++; return Token{ TokenType::OpenBrace, "{", m_line }; }
        if (c == '}') { m_pos++; return Token{ TokenType::CloseBrace, "}", m_line }; }
        if (c == '[') { m_pos++; return Token{ TokenType::OpenBracket, "[", m_line }; }
        if (c == ']') { m_pos++; return Token{ TokenType::CloseBracket, "]", m_line }; }
        if (c == ':') { m_pos++; return Token{ TokenType::Colon, ":", m_line }; }
        if (c == ',') { m_pos++; return Token{ TokenType::Comma, ",", m_line }; }

        if (c == '"') {
            return ParseString();
        }

        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return ParseNumber();
        }

        if (m_src.compare(m_pos, 4, "true") == 0) {
            m_pos += 4;
            return Token{ TokenType::Boolean, "true", m_line };
        }
        if (m_src.compare(m_pos, 5, "false") == 0) {
            m_pos += 5;
            return Token{ TokenType::Boolean, "false", m_line };
        }
        if (m_src.compare(m_pos, 4, "null") == 0) {
            m_pos += 4;
            return Token{ TokenType::Null, "null", m_line };
        }

        m_hasError = true;
        m_lastError = "Unexpected character in JSON: '" + std::string(1, c) + "'";
        m_pos++;
        return Token{ TokenType::Error, "", m_line, m_lastError };
    }

private:
    void SkipWhitespace() {
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos];
            if (c == ' ' || c == '\t' || c == '\r') {
                m_pos++;
            } else if (c == '\n') {
                m_line++;
                m_pos++;
            } else if (c == '/') {
                // Strict RFC 8259: comments are strictly forbidden
                m_hasError = true;
                m_lastError = "Comments are strictly prohibited in JSON grammar";
                return;
            } else {
                break;
            }
        }
    }

    Token ParseString() {
        int startLine = m_line;
        m_pos++; // skip initial '"'
        std::string s;
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos++];
            if (c == '"') {
                std::string utfErr;
                if (!ValidateUtf8String(s, utfErr)) {
                    m_hasError = true;
                    m_lastError = "Invalid UTF-8 in string literal: " + utfErr;
                    return Token{ TokenType::Error, "", startLine, m_lastError };
                }
                if (s.size() > 4096) {
                    m_hasError = true;
                    m_lastError = "String literal exceeds maximum bound of 4096 characters";
                    return Token{ TokenType::Error, "", startLine, m_lastError };
                }
                return Token{ TokenType::String, s, startLine };
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                m_hasError = true;
                m_lastError = "Unescaped control character in string literal";
                return Token{ TokenType::Error, "", startLine, m_lastError };
            }
            if (c == '\\') {
                if (m_pos >= m_src.size()) {
                    m_hasError = true;
                    m_lastError = "Unfinished escape sequence in string literal";
                    return Token{ TokenType::Error, "", startLine, m_lastError };
                }
                char esc = m_src[m_pos++];
                if (esc == '"') s += '"';
                else if (esc == '\\') s += '\\';
                else if (esc == '/') s += '/';
                else if (esc == 'b') s += '\b';
                else if (esc == 'f') s += '\f';
                else if (esc == 'n') s += '\n';
                else if (esc == 'r') s += '\r';
                else if (esc == 't') s += '\t';
                else if (esc == 'u') {
                    if (m_pos + 4 > m_src.size()) {
                        m_hasError = true;
                        m_lastError = "Incomplete unicode escape sequence";
                        return Token{ TokenType::Error, "", startLine, m_lastError };
                    }
                    std::string hexStr = m_src.substr(m_pos, 4);
                    m_pos += 4;
                    for (char hc : hexStr) {
                        if (!std::isxdigit(static_cast<unsigned char>(hc))) {
                            m_hasError = true;
                            m_lastError = "Invalid hex digit in unicode escape sequence: " + hexStr;
                            return Token{ TokenType::Error, "", startLine, m_lastError };
                        }
                    }
                    unsigned long cp = std::strtoul(hexStr.c_str(), nullptr, 16);

                    // Check for surrogate pairs
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: must be followed by \uDC00..\uDFFF
                        if (m_pos + 6 > m_src.size() || m_src[m_pos] != '\\' || m_src[m_pos + 1] != 'u') {
                            m_hasError = true;
                            m_lastError = "Lone high surrogate in unicode escape without low surrogate";
                            return Token{ TokenType::Error, "", startLine, m_lastError };
                        }
                        m_pos += 2;
                        std::string lowHex = m_src.substr(m_pos, 4);
                        m_pos += 4;
                        for (char hc : lowHex) {
                            if (!std::isxdigit(static_cast<unsigned char>(hc))) {
                                m_hasError = true;
                                m_lastError = "Invalid hex digit in low surrogate: " + lowHex;
                                return Token{ TokenType::Error, "", startLine, m_lastError };
                            }
                        }
                        unsigned long lowCp = std::strtoul(lowHex.c_str(), nullptr, 16);
                        if (lowCp < 0xDC00 || lowCp > 0xDFFF) {
                            m_hasError = true;
                            m_lastError = "Invalid low surrogate in unicode escape sequence";
                            return Token{ TokenType::Error, "", startLine, m_lastError };
                        }
                        uint32_t fullCp = 0x10000 + ((cp - 0xD800) << 10) + (lowCp - 0xDC00);
                        s += static_cast<char>(0xF0 | (fullCp >> 18));
                        s += static_cast<char>(0x80 | ((fullCp >> 12) & 0x3F));
                        s += static_cast<char>(0x80 | ((fullCp >> 6) & 0x3F));
                        s += static_cast<char>(0x80 | (fullCp & 0x3F));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        m_hasError = true;
                        m_lastError = "Lone low surrogate in unicode escape sequence";
                        return Token{ TokenType::Error, "", startLine, m_lastError };
                    } else if (cp < 0x80) {
                        s += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        s += static_cast<char>(0xC0 | (cp >> 6));
                        s += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        s += static_cast<char>(0xE0 | (cp >> 12));
                        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        s += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                } else {
                    m_hasError = true;
                    m_lastError = "Invalid escape sequence: \\" + std::string(1, esc);
                    return Token{ TokenType::Error, "", startLine, m_lastError };
                }
            } else {
                s += c;
            }
        }
        m_hasError = true;
        m_lastError = "Unterminated string literal in JSON";
        return Token{ TokenType::Error, "", startLine, m_lastError };
    }

    Token ParseNumber() {
        size_t start = m_pos;
        int startLine = m_line;
        if (m_src[m_pos] == '-') m_pos++;

        if (m_pos >= m_src.size() || !std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
            m_hasError = true;
            m_lastError = "Invalid number format in JSON";
            return Token{ TokenType::Error, "", startLine, m_lastError };
        }

        // Strict leading zero check: if starts with '0' and followed by another digit, reject!
        if (m_src[m_pos] == '0' && m_pos + 1 < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos + 1]))) {
            m_hasError = true;
            m_lastError = "Leading-zero integers are strictly prohibited in JSON";
            return Token{ TokenType::Error, "", startLine, m_lastError };
        }

        while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
            m_pos++;
        }

        if (m_pos < m_src.size() && m_src[m_pos] == '.') {
            m_pos++;
            if (m_pos >= m_src.size() || !std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                m_hasError = true;
                m_lastError = "Number with trailing decimal dot prohibited";
                return Token{ TokenType::Error, "", startLine, m_lastError };
            }
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                m_pos++;
            }
        }

        if (m_pos < m_src.size() && (m_src[m_pos] == 'e' || m_src[m_pos] == 'E')) {
            m_pos++;
            if (m_pos < m_src.size() && (m_src[m_pos] == '+' || m_src[m_pos] == '-')) {
                m_pos++;
            }
            if (m_pos >= m_src.size() || !std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                m_hasError = true;
                m_lastError = "Invalid exponent in number";
                return Token{ TokenType::Error, "", startLine, m_lastError };
            }
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                m_pos++;
            }
        }

        return Token{ TokenType::Number, m_src.substr(start, m_pos - start), startLine };
    }

private:
    const std::string& m_src;
    size_t m_pos = 0;
    int m_line = 1;
    bool m_hasError = false;
    std::string m_lastError;
};

enum class JsonValueType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

struct JsonValue {
    JsonValueType type = JsonValueType::Null;
    std::string strVal;
    double numVal = 0.0;
    uint64_t uintVal = 0;
    bool isUint = false;
    bool boolVal = false;
    std::vector<JsonValue> arrayVal;
    std::vector<std::pair<std::string, JsonValue>> objVal;

    const JsonValue* Find(const std::string& key) const {
        if (type != JsonValueType::Object) return nullptr;
        for (const auto& kv : objVal) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

bool ParseJsonValue(
    StrictJsonTokenizer& tokenizer,
    Token& currentToken,
    int depth,
    int& nodeCount,
    JsonValue& outVal,
    std::string& outError)
{
    if (depth > 16) {
        outError = "JSON nesting depth exceeds maximum limit of 16";
        return false;
    }
    if (++nodeCount > 1024) {
        outError = "JSON node count exceeds maximum limit of 1024";
        return false;
    }

    if (currentToken.type == TokenType::Error) {
        outError = currentToken.errorMsg.empty() ? "JSON syntax error" : currentToken.errorMsg;
        return false;
    }

    if (currentToken.type == TokenType::Null) {
        outVal.type = JsonValueType::Null;
        currentToken = tokenizer.NextToken();
        return true;
    }

    if (currentToken.type == TokenType::Boolean) {
        outVal.type = JsonValueType::Boolean;
        outVal.boolVal = (currentToken.value == "true");
        currentToken = tokenizer.NextToken();
        return true;
    }

    if (currentToken.type == TokenType::String) {
        outVal.type = JsonValueType::String;
        outVal.strVal = currentToken.value;
        currentToken = tokenizer.NextToken();
        return true;
    }

    if (currentToken.type == TokenType::Number) {
        outVal.type = JsonValueType::Number;
        outVal.strVal = currentToken.value;
        char* endPtr = nullptr;
        errno = 0;
        outVal.numVal = std::strtod(currentToken.value.c_str(), &endPtr);
        if (errno == ERANGE) {
            outError = "Numeric overflow in JSON: " + currentToken.value;
            return false;
        }
        if (currentToken.value.find('.') == std::string::npos &&
            currentToken.value.find('e') == std::string::npos &&
            currentToken.value.find('E') == std::string::npos &&
            !currentToken.value.empty() && currentToken.value[0] != '-') {
            errno = 0;
            outVal.uintVal = std::strtoull(currentToken.value.c_str(), &endPtr, 10);
            if (errno == ERANGE) {
                outError = "Integer overflow in JSON: " + currentToken.value;
                return false;
            }
            outVal.isUint = true;
        }
        currentToken = tokenizer.NextToken();
        return true;
    }

    if (currentToken.type == TokenType::OpenBracket) {
        outVal.type = JsonValueType::Array;
        currentToken = tokenizer.NextToken();
        if (currentToken.type == TokenType::CloseBracket) {
            currentToken = tokenizer.NextToken();
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!ParseJsonValue(tokenizer, currentToken, depth + 1, nodeCount, elem, outError)) {
                return false;
            }
            outVal.arrayVal.push_back(std::move(elem));
            if (currentToken.type == TokenType::Comma) {
                currentToken = tokenizer.NextToken();
                if (currentToken.type == TokenType::CloseBracket) {
                    outError = "Trailing comma prohibited in array";
                    return false;
                }
            } else if (currentToken.type == TokenType::CloseBracket) {
                currentToken = tokenizer.NextToken();
                break;
            } else {
                outError = "Expected ',' or ']' in JSON array";
                return false;
            }
        }
        return true;
    }

    if (currentToken.type == TokenType::OpenBrace) {
        outVal.type = JsonValueType::Object;
        currentToken = tokenizer.NextToken();
        if (currentToken.type == TokenType::CloseBrace) {
            currentToken = tokenizer.NextToken();
            return true;
        }
        std::unordered_set<std::string> seenKeys;
        while (true) {
            if (currentToken.type != TokenType::String) {
                outError = "Expected string key in JSON object";
                return false;
            }
            std::string key = currentToken.value;
            if (seenKeys.find(key) != seenKeys.end()) {
                outError = "Duplicate key prohibited in JSON object: " + key;
                return false;
            }
            seenKeys.insert(key);

            currentToken = tokenizer.NextToken();
            if (currentToken.type != TokenType::Colon) {
                outError = "Expected ':' after key in JSON object";
                return false;
            }
            currentToken = tokenizer.NextToken();

            JsonValue val;
            if (!ParseJsonValue(tokenizer, currentToken, depth + 1, nodeCount, val, outError)) {
                return false;
            }
            outVal.objVal.emplace_back(std::move(key), std::move(val));

            if (currentToken.type == TokenType::Comma) {
                currentToken = tokenizer.NextToken();
                if (currentToken.type == TokenType::CloseBrace) {
                    outError = "Trailing comma prohibited in object";
                    return false;
                }
            } else if (currentToken.type == TokenType::CloseBrace) {
                currentToken = tokenizer.NextToken();
                break;
            } else {
                outError = "Expected ',' or '}' in JSON object";
                return false;
            }
        }
        return true;
    }

    outError = "Unexpected token in JSON value";
    return false;
}

} // anonymous namespace

std::string ManifestJson::Serialize(const PackManifest& manifest) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << manifest.schemaVersion << ",\n";
    ss << "  \"packId\": \"" << EscapeJsonString(manifest.packId) << "\",\n";
    ss << "  \"version\": \"" << EscapeJsonString(manifest.version) << "\",\n";
    ss << "  \"displayName\": \"" << EscapeJsonString(manifest.displayName) << "\",\n";
    ss << "  \"description\": \"" << EscapeJsonString(manifest.description) << "\",\n";
    ss << "  \"author\": \"" << EscapeJsonString(manifest.author) << "\",\n";
    ss << "  \"previewImagePath\": \"" << EscapeJsonString(manifest.previewImagePath) << "\",\n";
    ss << "  \"assets\": [\n";
    for (size_t i = 0; i < manifest.assets.size(); ++i) {
        const auto& a = manifest.assets[i];
        ss << "    {\n";
        ss << "      \"type\": \"" << AssetTypeToString(a.assetType) << "\",\n";
        ss << "      \"id\": \"" << EscapeJsonString(a.assetId) << "\",\n";
        ss << "      \"path\": \"" << EscapeJsonString(a.relativePath) << "\",\n";
        ss << "      \"sha256\": \"" << a.sha256Hex << "\",\n";
        ss << "      \"size\": " << a.sizeBytes << "\n";
        ss << "    }" << (i + 1 < manifest.assets.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

bool ManifestJson::Deserialize(
    const std::string& jsonText,
    PackManifest& outManifest,
    std::string& outError)
{
    outManifest = PackManifest{};
    if (jsonText.size() > 1024 * 1024) {
        outError = "Manifest JSON exceeds maximum limit of 1 MB";
        return false;
    }
    if (jsonText.empty()) {
        outError = "Empty manifest JSON text";
        return false;
    }

    StrictJsonTokenizer tokenizer(jsonText);
    Token cur = tokenizer.NextToken();
    int nodeCount = 0;
    JsonValue root;
    if (!ParseJsonValue(tokenizer, cur, 0, nodeCount, root, outError)) {
        return false;
    }

    // Must not have junk suffix
    if (cur.type != TokenType::EndOfFile) {
        outError = "Junk trailing content after JSON root element";
        return false;
    }

    if (root.type != JsonValueType::Object) {
        outError = "Manifest root must be a JSON object";
        return false;
    }

    // Required field: schemaVersion
    const JsonValue* schemaVal = root.Find("schemaVersion");
    if (!schemaVal) {
        outError = "Missing required manifest field: 'schemaVersion'";
        return false;
    }
    if (schemaVal->type != JsonValueType::Number || !schemaVal->isUint) {
        outError = "Field 'schemaVersion' must be an integer";
        return false;
    }
    if (schemaVal->uintVal != 1) {
        outError = "Unsupported schemaVersion: " + std::to_string(schemaVal->uintVal) + " (must be 1)";
        return false;
    }
    outManifest.schemaVersion = static_cast<int>(schemaVal->uintVal);

    // Required field: packId
    const JsonValue* packIdVal = root.Find("packId");
    if (!packIdVal) {
        outError = "Missing required manifest field: 'packId'";
        return false;
    }
    if (packIdVal->type != JsonValueType::String || packIdVal->strVal.empty()) {
        outError = "Field 'packId' must be a non-empty string";
        return false;
    }
    outManifest.packId = packIdVal->strVal;

    // Required field: version
    const JsonValue* verVal = root.Find("version");
    if (!verVal) {
        outError = "Missing required manifest field: 'version'";
        return false;
    }
    if (verVal->type != JsonValueType::String || verVal->strVal.empty()) {
        outError = "Field 'version' must be a non-empty string";
        return false;
    }
    std::string verErr;
    if (!PackValidator::ValidateIdentifier(verVal->strVal, true, verErr)) {
        outError = "Invalid manifest version (SemVer violation): " + verErr;
        return false;
    }
    outManifest.version = verVal->strVal;

    // Required field: displayName
    const JsonValue* nameVal = root.Find("displayName");
    if (!nameVal) {
        outError = "Missing required manifest field: 'displayName'";
        return false;
    }
    if (nameVal->type != JsonValueType::String || nameVal->strVal.empty()) {
        outError = "Field 'displayName' must be a non-empty string";
        return false;
    }
    outManifest.displayName = nameVal->strVal;

    // Optional fields remain optional, but a present value must have its declared type.
    for (const auto* key : {"description", "author", "previewImagePath"}) {
        const auto* field = root.Find(key);
        if (field && field->type != JsonValueType::String) {
            outError = std::string("Field '") + key + "' must be a string";
            return false;
        }
    }
    // Optional fields
    const JsonValue* descVal = root.Find("description");
    if (descVal && descVal->type == JsonValueType::String) {
        outManifest.description = descVal->strVal;
    }
    const JsonValue* authVal = root.Find("author");
    if (authVal && authVal->type == JsonValueType::String) {
        outManifest.author = authVal->strVal;
    }
    const JsonValue* prevVal = root.Find("previewImagePath");
    if (prevVal && prevVal->type == JsonValueType::String) {
        outManifest.previewImagePath = prevVal->strVal;
    }

    // Required field: assets
    const JsonValue* assetsVal = root.Find("assets");
    if (!assetsVal) {
        outError = "Missing required manifest field: 'assets'";
        return false;
    }
    if (assetsVal->type != JsonValueType::Array) {
        outError = "Field 'assets' must be a JSON array";
        return false;
    }
    if (assetsVal->arrayVal.empty()) {
        outError = "Manifest contains zero assets";
        return false;
    }
    if (assetsVal->arrayVal.size() > 50) {
        outError = "Manifest exceeds maximum asset count of 50";
        return false;
    }

    for (size_t i = 0; i < assetsVal->arrayVal.size(); ++i) {
        const auto& item = assetsVal->arrayVal[i];
        if (item.type != JsonValueType::Object) {
            outError = "Asset entry at index " + std::to_string(i) + " must be a JSON object";
            return false;
        }

        const JsonValue* tVal = item.Find("type");
        if (!tVal || tVal->type != JsonValueType::String) {
            outError = "Asset at index " + std::to_string(i) + " missing required 'type' string";
            return false;
        }
        PackAssetType aType;
        if (!StringToAssetType(tVal->strVal, aType)) {
            outError = "Unsupported asset type: " + tVal->strVal;
            return false;
        }

        const JsonValue* idVal = item.Find("id");
        if (!idVal || idVal->type != JsonValueType::String || idVal->strVal.empty()) {
            outError = "Asset at index " + std::to_string(i) + " missing required 'id' string";
            return false;
        }

        const JsonValue* pVal = item.Find("path");
        if (!pVal || pVal->type != JsonValueType::String || pVal->strVal.empty()) {
            outError = "Asset '" + idVal->strVal + "' missing required 'path' string";
            return false;
        }

        const JsonValue* hVal = item.Find("sha256");
        if (!hVal || hVal->type != JsonValueType::String || hVal->strVal.size() != 64) {
            outError = "Asset '" + idVal->strVal + "' missing or invalid 64-char hex 'sha256'";
            return false;
        }

        const JsonValue* sVal = item.Find("size");
        if (!sVal || sVal->type != JsonValueType::Number || !sVal->isUint || sVal->uintVal == 0) {
            outError = "Asset '" + idVal->strVal + "' missing or invalid positive integer 'size'";
            return false;
        }

        PackAssetEntry entry;
        entry.assetType = aType;
        entry.assetId = idVal->strVal;
        entry.relativePath = pVal->strVal;
        entry.sha256Hex = hVal->strVal;
        entry.sizeBytes = sVal->uintVal;
        outManifest.assets.push_back(std::move(entry));
    }

    return true;
}

bool ManifestJson::ReadFromFile(
    const std::string& filePath,
    PackManifest& outManifest,
    std::string& outError)
{
    fs::path p = fs::u8path(filePath);
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
        outError = "Manifest file missing or not regular file: " + filePath;
        return false;
    }
    uintmax_t size = fs::file_size(p, ec);
    if (ec || size > 1024 * 1024) { // 1 MB manifest limit
        outError = "Manifest file size exceeds maximum limit of 1 MB: " + filePath;
        return false;
    }
    std::ifstream file(p, std::ios::binary);
    if (!file.is_open()) {
        outError = "Failed to open manifest file: " + filePath;
        return false;
    }
    // Enforce the read cap on the opened stream too: file_size alone races growth.
    constexpr size_t maxManifestBytes = 1024 * 1024;
    std::string text(maxManifestBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = file.gcount();
    if (file.bad() || count < 0 || static_cast<size_t>(count) > maxManifestBytes) {
        outError = "Manifest read failed or exceeds maximum limit of 1 MB: " + filePath;
        return false;
    }
    text.resize(static_cast<size_t>(count));
    return Deserialize(text, outManifest, outError);
}

bool ManifestJson::WriteToFile(
    const std::string& filePath,
    const PackManifest& manifest,
    std::string& outError)
{
    std::string json = Serialize(manifest);
    fs::path p = fs::u8path(filePath);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream file(p, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        outError = "Failed to open manifest file for writing: " + filePath;
        return false;
    }
    file << json;
    file.close();
    if (file.fail()) {
        outError = "Failed to flush and close manifest file: " + filePath;
        return false;
    }
    return true;
}

} // namespace PrismForge::Effects::Packs
