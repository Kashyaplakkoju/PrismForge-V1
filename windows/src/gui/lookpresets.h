#pragma once
#include "effects/framepipeline.h"
#include <wx/fileconf.h>
#include <wx/wfstream.h>
#include <wx/mstream.h>
#include <map>
#include <vector>
#include <utility>

// GUI-thread state. Only EffectiveLook is published to the atomic FramePipeline.
// Bypass is a session comparison control; it never overwrites the chosen look.
struct DesktopLookState {
    PrismForge::Effects::Look selected = PrismForge::Effects::Look::Original;
    bool bypassed = false;
    void Select(int value) {
        selected = value >= 0 && value <= 3 ? static_cast<PrismForge::Effects::Look>(value) : PrismForge::Effects::Look::Original;
    }
    PrismForge::Effects::Look EffectiveLook() const {
        return bypassed ? PrismForge::Effects::Look::Original : selected;
    }
};

struct DesktopLookPreset {
    PrismForge::Effects::Look look = PrismForge::Effects::Look::Original;
    static wxString Token(PrismForge::Effects::Look value) {
        switch (value) {
            case PrismForge::Effects::Look::Original: return "original";
            case PrismForge::Effects::Look::Warm: return "warm";
            case PrismForge::Effects::Look::Cool: return "cool";
            case PrismForge::Effects::Look::Mono: return "mono";
            default: return {};
        }
    }
    static bool Read(wxFileConfig& config, DesktopLookPreset& result) {
        wxString kind, version, token;
        if (!config.Read("kind", &kind) || kind != "PrismForgeDesktopLook" ||
            !config.Read("version", &version) || version != "1" || !config.Read("look", &token)) return false;
        for (int i = 0; i != 4; ++i) {
            auto candidate = static_cast<PrismForge::Effects::Look>(i);
            if (token == Token(candidate)) { result.look = candidate; return true; }
        }
        return false;
    }
    bool Write(wxFileConfig& config) const {
        const auto token = Token(look);
        return !token.empty() && config.Write("kind", "PrismForgeDesktopLook") &&
            config.Write("version", "1") && config.Write("look", token);
    }
    // Read a bounded memory snapshot, so a growing input file cannot feed an unbounded parser.
    static bool ReadFile(const wxString& path, DesktopLookPreset& result) {
        wxFileInputStream input(path);
        const auto length = input.GetLength();
        if (!input.IsOk() || length <= 0 || length > 32768) return false;
        std::vector<char> bytes(static_cast<size_t>(length));
        input.Read(bytes.data(), bytes.size());
        if (input.LastRead() != bytes.size()) return false;
        wxMemoryInputStream snapshot(bytes.data(), bytes.size());
        wxFileConfig config(snapshot);
        return Read(config, result);
    }
    bool Export(const wxString& path) const {
        wxMemoryInputStream empty("", 0);
        wxFileConfig config(empty);
        if (!Write(config)) return false;
        wxTempFileOutputStream output(path);
        return output.IsOk() && config.Save(output) && output.Commit();
    }
};

class LookPresetStore {
public:
    explicit LookPresetStore(wxString path = {}) : path_(std::move(path)) {}
    std::map<wxString, DesktopLookPreset> custom;
    static bool ValidName(const wxString& name) {
        if (name.empty() || name.size() > 48 || name != name.Strip(wxString::both) ||
            name == "Custom" || name == "Original" || name == "Warm" || name == "Cool" || name == "Mono") return false;
        for (auto ch : name) {
            const auto c = ch.GetValue();
            if (c < 32 || c == 127 || c == '/' || c == '\\' || c == '[' || c == ']') return false;
        }
        return true;
    }
    bool Load() {
        wxFileInputStream input(path_);
        const auto length = input.GetLength();
        if (!input.IsOk() || length < 0 || length > 65536) return false;
        if (length == 0) { custom.clear(); return true; }
        std::vector<char> bytes(static_cast<size_t>(length));
        input.Read(bytes.data(), bytes.size());
        if (input.LastRead() != bytes.size()) return false;
        wxMemoryInputStream snapshot(bytes.data(), bytes.size());
        wxFileConfig config(snapshot);
        std::map<wxString, DesktopLookPreset> loaded;
        wxString name; long index;
        for (bool more = config.GetFirstGroup(name, index); more; more = config.GetNextGroup(name, index)) {
            config.SetPath("/" + name);
            DesktopLookPreset preset;
            if (ValidName(name) && DesktopLookPreset::Read(config, preset)) loaded.emplace(name, preset);
            config.SetPath("/");
            if (loaded.size() == 50) break;
        }
        custom = std::move(loaded);
        return true;
    }
    bool Save() const {
        if (path_.empty() || custom.size() > 50) return false;
        wxMemoryInputStream empty("", 0);
        wxFileConfig config(empty);
        for (const auto& [name, preset] : custom) {
            if (!ValidName(name)) return false;
            config.SetPath("/" + name);
            if (!preset.Write(config)) return false;
            config.SetPath("/");
        }
        wxTempFileOutputStream output(path_);
        return output.IsOk() && config.Save(output) && output.Commit();
    }
private:
    wxString path_;
};
