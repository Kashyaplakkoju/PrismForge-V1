#include <wx/init.h>
#include "gui/presets.h"
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    wxInitializer initialize;
    if (!initialize) return 1;
    try {
        const std::string valid = "version=1\nwidth=1280\nheight=720\nfps=30\nbitrate=4000\nadaptive=1\nminBitrate=500\nmaxBitrate=8000\n";
        auto parse = [](const std::string& text, QualityPreset& preset) {
            wxMemoryInputStream stream(text.data(), text.size()); wxFileConfig config(stream);
            return QualityPreset::Read(config, preset);
        };
        QualityPreset expected;
        check(parse(valid, expected), "valid profile rejected");
        check(expected.width == 1280 && expected.adaptive, "profile values changed");
        wxMemoryInputStream empty("", 0); wxFileConfig exportConfig(empty); expected.Write(exportConfig);
        wxMemoryOutputStream serialized; check(exportConfig.Save(serialized), "export failed");
        std::string payload(serialized.GetSize(), '\0'); serialized.CopyTo(payload.data(), payload.size());
        QualityPreset roundtrip; check(parse(payload, roundtrip) && roundtrip == expected, "profile round trip changed settings");
        check(payload.find("PIN_") == std::string::npos && payload.find("DEVICE") == std::string::npos, "export contains unrelated settings");
        for (const auto& edit : {std::pair{"version=1", "version=2"}, {"fps=30", "fps=999"}, {"width=1280", "width=-1"}, {"bitrate=4000", "bitrate=50000"}, {"minBitrate=500", "minBitrate=9000"}, {"adaptive=1", "adaptive=3"}}) {
            auto invalid = valid; invalid.replace(invalid.find(edit.first), strlen(edit.first), edit.second);
            check(!parse(invalid, roundtrip), "invalid profile accepted");
        }
        check(!parse("version=1\n", roundtrip), "incomplete profile accepted");
        check(PresetStore::ValidName("Desk camera") && !PresetStore::ValidName("../device") && !PresetStore::ValidName("Meeting"), "profile name validation failed");
        check(PresetStore::Nearest({{0,0},{640,480},{1920,1080}}, 1920,1080) == DeviceDescriptor::Resolution(1920,1080), "capability selection failed");
        check(PresetStore::Nearest({{0,0}}, 1280,720) == DeviceDescriptor::Resolution(0,0), "invalid capability accepted");
        std::cout << "Profile validation, round trip, names and capability checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
