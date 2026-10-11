#pragma once
#include <wx/fileconf.h>
#include <wx/wfstream.h>
#include <wx/mstream.h>
#include <wx/filename.h>
#include <algorithm>
#include <cmath>
#include "net/devicedescriptor.h"
#include <map>
#include "settings.h"

// Quality profiles deliberately exclude torch, credentials, device IDs and output registration.
struct QualityPreset {
    int width = 1920, height = 1080, fps = 30, bitrate = 6000;
    bool adaptive = false;
    int minBitrate = 2000, maxBitrate = 8000;
    bool operator==(const QualityPreset& p) const {
        return width == p.width && height == p.height && fps == p.fps && bitrate == p.bitrate &&
            adaptive == p.adaptive && minBitrate == p.minBitrate && maxBitrate == p.maxBitrate;
    }
    static QualityPreset From(const StreamOptions& s) {
        return {s.resolution.first, s.resolution.second, s.fps, s.bitrate, s.adaptiveBitrate, s.minBitrate, s.maxBitrate};
    }
    static bool Read(wxFileConfig& cfg, QualityPreset& p) {
        long version, w, h, fps, br, lo, hi, adaptive;
        if (!cfg.Read("version", &version) || version != 1 || !cfg.Read("width", &w) ||
            !cfg.Read("height", &h) || !cfg.Read("fps", &fps) || !cfg.Read("bitrate", &br) ||
            !cfg.Read("minBitrate", &lo) || !cfg.Read("maxBitrate", &hi) || !cfg.Read("adaptive", &adaptive)) return false;
        if (w < 160 || w > 7680 || h < 120 || h > 7680 || (fps != 15 && fps != 24 && fps != 30 && fps != 60) ||
            br < RTSP::MIN_BITRATE || br > RTSP::MAX_BITRATE || lo < RTSP::MIN_BITRATE || hi > RTSP::MAX_BITRATE ||
            lo > hi || (adaptive != 0 && adaptive != 1)) return false;
        p = {(int)w, (int)h, (int)fps, (int)br, adaptive == 1, (int)lo, (int)hi};
        return true;
    }
    void Write(wxFileConfig& cfg) const {
        cfg.Write("version", 1L); cfg.Write("width", width); cfg.Write("height", height);
        cfg.Write("fps", fps); cfg.Write("bitrate", bitrate); cfg.Write("adaptive", adaptive ? 1L : 0L);
        cfg.Write("minBitrate", minBitrate); cfg.Write("maxBitrate", maxBitrate);
    }
};

class PresetStore {
public:
    std::map<wxString, QualityPreset> custom;
    static wxString Path() { auto p = Settings::GetDocumentsDirectoryPath(); p.SetFullName("studio-presets.ini"); return p.GetFullPath(); }
    void Load() {
        if (!wxFileExists(Path())) return;
        wxFileConfig config("", "", Path(), "", wxCONFIG_USE_LOCAL_FILE);
        wxString name; long index;
        for (bool more = config.GetFirstGroup(name, index); more; more = config.GetNextGroup(name, index)) {
            if (custom.size() >= 50) break;
            config.SetPath("/" + name);
            QualityPreset preset;
            if (ValidName(name) && QualityPreset::Read(config, preset)) custom[name] = preset;
            config.SetPath("/");
        }
    }
    bool Save() const {
        wxMemoryInputStream empty("", 0);
        wxFileConfig config(empty);
        for (const auto& [name, preset] : custom) { config.SetPath("/" + name); preset.Write(config); config.SetPath("/"); }
        // Write atomically so interruption cannot truncate the previous presets.
        wxTempFileOutputStream stream(Path());
        return stream.IsOk() && config.Save(stream) && stream.Commit();
    }
    static bool ValidName(const wxString& name) {
        return !name.empty() && name.size() <= 48 && name.Find('/') == wxNOT_FOUND && name.Find('\\') == wxNOT_FOUND &&
            name.Find('[') == wxNOT_FOUND && name.Find(']') == wxNOT_FOUND && name.Find('\n') == wxNOT_FOUND && name.Find('\r') == wxNOT_FOUND &&
            name != "Meeting" && name != "Low bandwidth" && name != "High quality" && name != "Custom";
    }
    static DeviceDescriptor::Resolution Nearest(const std::vector<DeviceDescriptor::Resolution>& sizes, int w, int h) {
        if (sizes.empty()) return {0, 0};
        auto cost = [=](const auto& r) { return r.first > 0 && r.second > 0 ? std::abs(std::log(double(r.first) / w)) + std::abs(std::log(double(r.second) / h)) : 1e9; };
        auto best = *std::min_element(sizes.begin(), sizes.end(), [&](const auto& a, const auto& b) { return cost(a) < cost(b); });
        return best.first > 0 && best.second > 0 ? best : DeviceDescriptor::Resolution(0, 0);
    }
};
