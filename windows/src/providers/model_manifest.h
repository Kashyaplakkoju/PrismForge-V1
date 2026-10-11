#pragma once

#include "frame_preprocessor.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <limits>
#include <cstdlib>

namespace PrismForge::Providers {

enum class ModelOutputInterpretation {
    RawProbability = 0, LogitsSigmoid = 1, SoftmaxTwoClass = 2, ArgMax = 3
};

// A deliberately small checked JSON reader for this flat manifest schema. No substring
// searches, implicit coercions, nested structures, duplicate keys, trailing data or NaN.
namespace ManifestJson {
struct Value {
    enum class Type { String, Number, Boolean, Array } type;
    std::string text;
    double number = 0;
    bool boolean = false;
    std::vector<double> array;
};
class Reader {
public:
    explicit Reader(const std::string& text) : text_(text) {}
    bool Parse(std::map<std::string, Value>& values) {
        Skip(); if (!Take('{')) return false; Skip();
        if (Take('}')) { Skip(); return pos_ == text_.size(); }
        for (;;) {
            std::string key; if (!String(key) || !Take(':')) return false;
            Value v; if (!ReadValue(v) || !values.emplace(key, std::move(v)).second) return false;
            Skip(); if (Take('}')) break;
            if (!Take(',')) return false;
        }
        Skip(); return pos_ == text_.size();
    }
private:
    void Skip() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_; }
    bool Take(char c) { Skip(); if (pos_ == text_.size() || text_[pos_] != c) return false; ++pos_; return true; }
    bool String(std::string& out) {
        if (!Take('"')) return false;
        while (pos_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ == text_.size()) return false;
            char e = text_[pos_++];
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break; case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break; case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
            case 'u': {
                // JSON escapes are decoded to UTF-8. Reject isolated surrogate halves.
                unsigned code = 0;
                if (!Hex4(code) || (code >= 0xD800 && code <= 0xDFFF)) return false;
                if (code < 0x80) out.push_back(static_cast<char>(code));
                else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 63))); }
                else { out.push_back(static_cast<char>(0xE0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 63))); out.push_back(static_cast<char>(0x80 | (code & 63))); }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Hex4(unsigned& code) {
        if (text_.size() - pos_ < 4) return false;
        for (int i = 0; i < 4; ++i) { char c = text_[pos_++]; unsigned v;
            if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else return false;
            code = code * 16 + v;
        } return true;
    }
    bool Number(double& out) {
        Skip(); size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ == text_.size()) return false;
        if (text_[pos_] == '0') ++pos_;
        else { if (text_[pos_] < '1' || text_[pos_] > '9') return false; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_; }
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; size_t digits = pos_; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_; if (digits == pos_) return false; }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) { ++pos_; if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_; size_t digits = pos_; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_; if (digits == pos_) return false; }
        std::string value = text_.substr(begin, pos_ - begin); char* end = nullptr;
        out = std::strtod(value.c_str(), &end);
        return end == value.c_str() + value.size() && std::isfinite(out);
    }
    bool ReadValue(Value& v) {
        Skip(); if (pos_ == text_.size()) return false;
        if (text_[pos_] == '"') { v.type = Value::Type::String; return String(v.text); }
        if (text_[pos_] == '[') {
            v.type = Value::Type::Array; ++pos_; if (Take(']')) return true;
            for (;;) { double d; if (!Number(d) || v.array.size() >= 16) return false; v.array.push_back(d); if (Take(']')) return true; if (!Take(',')) return false; }
        }
        if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; v.type = Value::Type::Boolean; v.boolean = true; return true; }
        if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.type = Value::Type::Boolean; v.boolean = false; return true; }
        v.type = Value::Type::Number; return Number(v.number);
    }
    const std::string& text_; size_t pos_ = 0;
};
} // namespace ManifestJson

struct ModelManifest {
    static constexpr size_t kMaxTensorElements = FramePreprocessor::kMaxTensorElements;
    static constexpr int kMaxClasses = 256;
    static constexpr int kMaxDimension = 8192;
    std::string modelName = "DeterministicGrayscaleFixture";
    std::string modelVersion = "1.0.0";
    std::string inputTensorName = "input";
    std::vector<int64_t> inputDims = { 1, 3, 16, 16 };
    TensorLayout inputLayout = TensorLayout::NCHW;
    NormalizationConfig normConfig;
    AspectMode aspectMode = AspectMode::LetterboxPad;
    std::string outputTensorName = "output";
    std::vector<int64_t> outputDims = { 1, 1, 16, 16 };
    TensorLayout outputLayout = TensorLayout::NCHW;
    ModelOutputInterpretation outputInterpretation = ModelOutputInterpretation::RawProbability;
    int personClassIndex = 0;
    float probabilityThreshold = 0.5f;
    bool retainSoftProbability = true;
    bool producesPersonMask = true;
    bool producesSkinMask = false;
    bool producesProtectionMask = false;
    bool producesNormalMap = false;
    bool producesDepthMap = false;
    bool producesFaceLandmarks = false;

    static bool TensorGeometry(const std::vector<int64_t>& dims, TensorLayout layout, int& w, int& h, int& channels) {
        if (dims.size() != 4 || dims[0] != 1 || (layout != TensorLayout::NCHW && layout != TensorLayout::NHWC)) return false;
        size_t count = 1;
        for (int64_t d : dims) { if (d <= 0 || d > kMaxDimension || static_cast<size_t>(d) > kMaxTensorElements / count) return false; count *= static_cast<size_t>(d); }
        channels = static_cast<int>(dims[layout == TensorLayout::NCHW ? 1 : 3]);
        h = static_cast<int>(dims[layout == TensorLayout::NCHW ? 2 : 1]);
        w = static_cast<int>(dims[layout == TensorLayout::NCHW ? 3 : 2]);
        return channels <= kMaxClasses;
    }
    bool InputGeometry(int& w, int& h) const { int c; return TensorGeometry(inputDims, inputLayout, w, h, c) && c == 3; }
    bool OutputGeometry(int& w, int& h, int& classes) const { return TensorGeometry(outputDims, outputLayout, w, h, classes); }
    bool Validate() const {
        auto nameValid = [](const std::string& s) { return !s.empty() && s.size() <= 1024 && s.find('\0') == std::string::npos; };
        if (!nameValid(modelName) || !nameValid(modelVersion) || !nameValid(inputTensorName) || !nameValid(outputTensorName)) return false;
        int iw, ih, ow, oh, classes;
        if (!InputGeometry(iw, ih) || !OutputGeometry(ow, oh, classes) || personClassIndex < 0 || personClassIndex >= classes) return false;
        if (aspectMode != AspectMode::LetterboxPad && aspectMode != AspectMode::CropToFill && aspectMode != AspectMode::Stretch) return false;
        if (normConfig.mean.size() != 3 || normConfig.std.size() != 3) return false;
        for (float m : normConfig.mean) if (!std::isfinite(m)) return false;
        for (float s : normConfig.std) if (!std::isfinite(s) || s < 1e-6f) return false;
        if (!std::isfinite(probabilityThreshold) || probabilityThreshold < 0.0f || probabilityThreshold > 1.0f) return false;
        switch (outputInterpretation) {
        case ModelOutputInterpretation::RawProbability: case ModelOutputInterpretation::LogitsSigmoid: break;
        case ModelOutputInterpretation::SoftmaxTwoClass: if (classes != 2) return false; break;
        case ModelOutputInterpretation::ArgMax: if (classes < 2) return false; break;
        default: return false;
        }
        // This adapter exposes exactly a person plane. Other provider types need separate contracts.
        return producesPersonMask && !producesSkinMask && !producesProtectionMask && !producesNormalMap &&
            !producesDepthMap && !producesFaceLandmarks;
    }

    static bool LoadFromFile(const std::wstring& path, ModelManifest& outManifest) {
        try {
            std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
            if (!file) return false;
            const auto length = file.tellg(); if (length <= 0 || length > 64 * 1024) return false;
            std::string content(static_cast<size_t>(length), '\0'); file.seekg(0);
            if (!file.read(content.data(), static_cast<std::streamsize>(content.size()))) return false;
            std::map<std::string, ManifestJson::Value> values;
            if (!ManifestJson::Reader(content).Parse(values)) return false;
            // Explicit names, axes and dimensions are mandatory for external models.
            for (const char* key : {"modelName", "inputTensorName", "inputDims", "inputLayout", "outputTensorName", "outputDims", "outputLayout", "outputInterpretation"})
                if (values.find(key) == values.end()) return false;
            ModelManifest m;
            for (const auto& entry : values) {
                const auto& k = entry.first; const auto& v = entry.second;
                auto string = [&](std::string& dst) { if (v.type != ManifestJson::Value::Type::String) return false; dst = v.text; return true; };
                auto boolean = [&](bool& dst) { if (v.type != ManifestJson::Value::Type::Boolean) return false; dst = v.boolean; return true; };
                auto dims = [&](std::vector<int64_t>& dst) { if (v.type != ManifestJson::Value::Type::Array || v.array.size() != 4) return false; dst.clear(); for (double d : v.array) { if (d < 1 || d > kMaxDimension || std::floor(d) != d) return false; dst.push_back(static_cast<int64_t>(d)); } return true; };
                auto norm = [&](std::vector<float>& dst) { if (v.type != ManifestJson::Value::Type::Array || v.array.size() != 3) return false; dst.clear(); for (double d : v.array) { if (std::abs(d) > std::numeric_limits<float>::max()) return false; dst.push_back(static_cast<float>(d)); } return true; };
                auto layout = [&](TensorLayout& dst) { if (v.type != ManifestJson::Value::Type::String) return false; if (v.text == "NCHW") dst = TensorLayout::NCHW; else if (v.text == "NHWC") dst = TensorLayout::NHWC; else return false; return true; };
                bool valid = false;
                if (k == "modelName") valid = string(m.modelName);
                else if (k == "modelVersion") valid = string(m.modelVersion);
                else if (k == "inputTensorName") valid = string(m.inputTensorName);
                else if (k == "outputTensorName") valid = string(m.outputTensorName);
                else if (k == "inputDims") valid = dims(m.inputDims);
                else if (k == "outputDims") valid = dims(m.outputDims);
                else if (k == "inputLayout") valid = layout(m.inputLayout);
                else if (k == "outputLayout") valid = layout(m.outputLayout);
                else if (k == "mean") valid = norm(m.normConfig.mean);
                else if (k == "std") valid = norm(m.normConfig.std);
                else if (k == "scaleTo01") valid = boolean(m.normConfig.scaleTo01);
                else if (k == "retainSoftProbability") valid = boolean(m.retainSoftProbability);
                else if (k == "producesPersonMask") valid = boolean(m.producesPersonMask);
                else if (k == "producesSkinMask") valid = boolean(m.producesSkinMask);
                else if (k == "producesProtectionMask") valid = boolean(m.producesProtectionMask);
                else if (k == "producesNormalMap") valid = boolean(m.producesNormalMap);
                else if (k == "producesDepthMap") valid = boolean(m.producesDepthMap);
                else if (k == "producesFaceLandmarks") valid = boolean(m.producesFaceLandmarks);
                else if (k == "personClassIndex") { valid = v.type == ManifestJson::Value::Type::Number && v.number >= 0 && v.number < kMaxClasses && std::floor(v.number) == v.number; if (valid) m.personClassIndex = static_cast<int>(v.number); }
                else if (k == "probabilityThreshold") { valid = v.type == ManifestJson::Value::Type::Number && v.number >= 0 && v.number <= 1; if (valid) m.probabilityThreshold = static_cast<float>(v.number); }
                else if (k == "aspectMode") { valid = v.type == ManifestJson::Value::Type::String; if (valid && v.text == "LetterboxPad") m.aspectMode = AspectMode::LetterboxPad; else if (valid && v.text == "CropToFill") m.aspectMode = AspectMode::CropToFill; else if (valid && v.text == "Stretch") m.aspectMode = AspectMode::Stretch; else valid = false; }
                else if (k == "outputInterpretation") { valid = v.type == ManifestJson::Value::Type::String; if (valid && v.text == "RawProbability") m.outputInterpretation = ModelOutputInterpretation::RawProbability; else if (valid && v.text == "LogitsSigmoid") m.outputInterpretation = ModelOutputInterpretation::LogitsSigmoid; else if (valid && v.text == "SoftmaxTwoClass") m.outputInterpretation = ModelOutputInterpretation::SoftmaxTwoClass; else if (valid && v.text == "ArgMax") m.outputInterpretation = ModelOutputInterpretation::ArgMax; else valid = false; }
                if (!valid) return false;
            }
            if (!m.Validate()) return false;
            outManifest = std::move(m); return true;
        } catch (...) { return false; }
    }
};

} // namespace PrismForge::Providers
