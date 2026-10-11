#include <wx/init.h>
#include <wx/filename.h>
#include <wx/log.h>
#include "gui/lookpresets.h"
#include <iostream>
#include <stdexcept>
#include <functional>

using PrismForge::Effects::Look;
static void Check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct TemporaryFile {
    wxString path = wxFileName::CreateTempFileName("PrismForge-look-test-");
    ~TemporaryFile() { if (wxFileExists(path)) wxRemoveFile(path); }
};
static void WriteFile(const wxString& path, const std::string& text) {
    wxFileOutputStream output(path);
    Check(output.IsOk(), "could not open fixture");
    output.Write(text.data(), text.size());
    Check(output.LastWrite() == text.size(), "could not write fixture");
}
static std::string ReadFile(const wxString& path) {
    wxFileInputStream input(path);
    Check(input.IsOk(), "could not read fixture");
    std::string text(static_cast<size_t>(input.GetLength()), '\0');
    input.Read(text.data(), text.size());
    Check(input.LastRead() == text.size(), "short fixture read");
    return text;
}
static bool Parse(const std::string& text, DesktopLookPreset& preset) {
    wxMemoryInputStream input(text.data(), text.size());
    wxFileConfig config(input);
    return DesktopLookPreset::Read(config, preset);
}
static const std::string valid = "kind=PrismForgeDesktopLook\nversion=1\nlook=warm\n";

static void BypassRestoresActualPixels() {
    auto* input = av_frame_alloc();
    Check(input != nullptr, "frame allocation failed");
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(input, [](AVFrame* f) { av_frame_free(&f); });
    input->format = AV_PIX_FMT_BGR24; input->width = 8; input->height = 8;
    input->color_range = AVCOL_RANGE_JPEG;
    Check(av_frame_get_buffer(input, 32) == 0, "pixel allocation failed");
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        auto* p = input->data[0] + y * input->linesize[0] + x * 3;
        p[0] = 20; p[1] = 50; p[2] = 80;
    }
    const int expected[4][3] = {{20,50,80}, {8,54,94}, {34,52,68}, {56,56,56}};
    DesktopLookState state;
    PrismForge::Effects::FramePipeline pipeline;
    for (int look = 0; look < 4; ++look) {
        state.Select(look);
        for (int iteration = 0; iteration < 5; ++iteration) {
            state.bypassed = true;
            pipeline.SetLook(state.EffectiveLook());
            Check(pipeline.Process(input) == input, "bypass lost input pointer identity");
            Check(static_cast<int>(state.selected) == look, "bypass erased chosen look");
            state.bypassed = false;
            pipeline.SetLook(state.EffectiveLook());
            auto* output = pipeline.Process(input);
            Check(output && (look == 0 || output != input), "active look bypassed unexpectedly");
            for (int c = 0; c < 3; ++c) Check(output->data[0][c] == expected[look][c], "restored look has wrong pixels");
            Check(input->data[0][0] == 20 && input->data[0][1] == 50 && input->data[0][2] == 80, "input mutated");
        }
    }
    state.Select(-1); Check(state.selected == Look::Original, "missing selection not normalized");
    state.Select(99); Check(state.selected == Look::Original, "bad selection not normalized");
}

static void ProfileRoundTrips() {
    for (int look = 0; look < 4; ++look) {
        DesktopLookPreset preset{static_cast<Look>(look)};
        wxMemoryInputStream empty("", 0); wxFileConfig config(empty);
        Check(preset.Write(config), "valid look serialization failed");
        wxMemoryOutputStream serialized; Check(config.Save(serialized), "serialization failed");
        std::string text(serialized.GetSize(), '\0'); serialized.CopyTo(text.data(), text.size());
        DesktopLookPreset restored;
        Check(Parse(text, restored) && restored.look == preset.look, "look round trip changed values");
        Check(text.find("DEVICE") == std::string::npos && text.find("bitrate") == std::string::npos &&
              text.find("PIN_") == std::string::npos && text.find("bypass") == std::string::npos, "export contains unrelated state");
    }
}

static void RejectForeignAndMalformedProfiles() {
    for (const auto& text : {"", "version=1\nlook=warm\n", "kind=PrismForgeDesktopLook\nversion=2\nlook=warm\n",
             "kind=PrismForgeDesktopLook\nversion=1junk\nlook=warm\n", "kind=OtherProduct\nversion=1\nlook=warm\n",
             "kind=PrismForgeDesktopLook\nversion=1\nlook=7\n", "kind=PrismForgeDesktopLook\nversion=1\n",
             "version=1\nwidth=1920\nheight=1080\nbitrate=6000\n"}) {
        DesktopLookPreset preset{Look::Cool};
        Check(!Parse(text, preset), "invalid or foreign preset accepted");
        Check(preset.look == Look::Cool, "failed parse changed active look");
    }
}

static void BoundedFileImport() {
    TemporaryFile file;
    WriteFile(file.path, valid);
    DesktopLookPreset preset;
    Check(DesktopLookPreset::ReadFile(file.path, preset) && preset.look == Look::Warm, "valid import rejected");
    WriteFile(file.path, valid + std::string(32769, '#'));
    preset.look = Look::Cool;
    Check(!DesktopLookPreset::ReadFile(file.path, preset) && preset.look == Look::Cool, "oversized import changed state");
    WriteFile(file.path, "");
    Check(!DesktopLookPreset::ReadFile(file.path, preset), "empty file accepted");
}

static void PresetNames() {
    Check(LookPresetStore::ValidName("Meeting warm"), "valid name rejected");
    Check(LookPresetStore::ValidName(wxString::FromUTF8("Caf\xc3\xa9")), "Unicode name rejected");
    for (const auto& name : {"", "Custom", "Original", "Warm", "Cool", "Mono", " name", "name ",
                            "a/b", "a\\b", "[name]", "a\tb", "a\nb", "a\rb"})
        Check(!LookPresetStore::ValidName(name), "unsafe or reserved name accepted");
    Check(!LookPresetStore::ValidName(wxString('a', 49)), "overlong name accepted");
}

static void AtomicStoreAndReload() {
    TemporaryFile file;
    LookPresetStore store(file.path);
    const auto unicode = wxString::FromUTF8("Caf\xc3\xa9 warm");
    store.custom.emplace(unicode, DesktopLookPreset{Look::Warm});
    store.custom.emplace("Evening mono", DesktopLookPreset{Look::Mono});
    Check(store.Save(), "store save failed");
    LookPresetStore loaded(file.path);
    Check(loaded.Load() && loaded.custom.size() == 2 && loaded.custom.at(unicode).look == Look::Warm &&
          loaded.custom.at("Evening mono").look == Look::Mono, "store reload changed data");
    const auto original = ReadFile(file.path);
    store.custom.emplace("bad/name", DesktopLookPreset{Look::Cool});
    Check(!store.Save() && ReadFile(file.path) == original, "rejected store damaged previous file");
    store.custom.erase("bad/name");
    store.custom.at(unicode).look = static_cast<Look>(99);
    Check(!store.Save() && ReadFile(file.path) == original, "invalid look damaged previous file");
    store.custom.clear();
    Check(store.Save() && loaded.Load() && loaded.custom.empty(), "deleting the last saved look did not reload an empty store");
}

static void CapacityAndMalformedStore() {
    TemporaryFile file;
    LookPresetStore store(file.path);
    for (int i = 0; i != 50; ++i) store.custom.emplace(wxString::Format("Preset %d", i), DesktopLookPreset{Look::Warm});
    Check(store.Save(), "50 valid presets rejected");
    const auto original = ReadFile(file.path);
    store.custom.emplace("Overflow", DesktopLookPreset{Look::Cool});
    Check(!store.Save() && ReadFile(file.path) == original, "capacity failure damaged previous file");
    WriteFile(file.path, "[Valid]\n" + valid + "[Bad]\nkind=PrismForgeDesktopLook\nversion=1\nlook=invalid\n");
    LookPresetStore loaded(file.path);
    Check(loaded.Load() && loaded.custom.size() == 1 && loaded.custom.at("Valid").look == Look::Warm, "malformed stored entry was not skipped");
    WriteFile(file.path, std::string(65537, '#'));
    Check(!loaded.Load() && loaded.custom.size() == 1, "oversized store erased loaded presets");
}

static void ExportAndSaveFailures() {
    TemporaryFile file;
    WriteFile(file.path, "existing-data");
    Check(!DesktopLookPreset{static_cast<Look>(99)}.Export(file.path) && ReadFile(file.path) == "existing-data", "invalid export damaged destination");
    Check(DesktopLookPreset{Look::Mono}.Export(file.path), "file export failed");
    DesktopLookPreset restored;
    Check(DesktopLookPreset::ReadFile(file.path, restored) && restored.look == Look::Mono, "exported file did not reload");
    LookPresetStore missingParent(file.path + ".missing/presets.ini");
    missingParent.custom.emplace("Saved look", DesktopLookPreset{Look::Cool});
    wxLogNull suppressExpectedFileError;
    Check(!missingParent.Save(), "missing parent save succeeded");
    Check(missingParent.custom.at("Saved look").look == Look::Cool, "failed disk write corrupted memory");
}

int main() {
    wxInitializer initialize;
    if (!initialize) return 1;
    const std::pair<const char*, void(*)()> tests[] = {
        {"BypassRestoresActualPixels", BypassRestoresActualPixels}, {"ProfileRoundTrips", ProfileRoundTrips},
        {"RejectForeignAndMalformedProfiles", RejectForeignAndMalformedProfiles}, {"BoundedFileImport", BoundedFileImport},
        {"PresetNames", PresetNames}, {"AtomicStoreAndReload", AtomicStoreAndReload},
        {"CapacityAndMalformedStore", CapacityAndMalformedStore}, {"ExportAndSaveFailures", ExportAndSaveFailures}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << (8 - failures) << "/8 desktop look suites passed\n";
    return failures == 0 ? 0 : 1;
}
