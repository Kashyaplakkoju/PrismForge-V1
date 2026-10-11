#pragma once
#include "gui/studio.h"
#include "effects/cubelut.h"
#include "effects/packs/pack_manager.h"
#include <wx/listbox.h>
#include <wx/statbmp.h>
#include <wx/timer.h>
#include <functional>
#include <thread>
#include <atomic>

struct EffectPackSelection {
    std::string packId, version, assetId;
    bool Empty() const { return packId.empty() || version.empty() || assetId.empty(); }
};
struct EffectPackLut {
    EffectPackSelection selection;
    std::shared_ptr<const PrismForge::Effects::CubeLut> lut;
    wxString label;
};
struct EffectPackRestoreResult {
    std::atomic<bool> done{false};
    EffectPackLut value;
    std::string error;
};

// One owned worker at a time. Results are polled on the GUI thread: workers never
// capture a window or post callbacks, and closing waits for a transaction to finish.
class PackLibraryDialog : public Studio::GlassDialogBase {
public:
    using Applied = std::function<void(const EffectPackLut&)>;
    using Removed = std::function<void(const std::string&,const std::string&)>;
    PackLibraryDialog(wxWindow*, Applied, Removed);
    ~PackLibraryDialog() override;
    static std::string LibraryRoot();
    static EffectPackSelection ReadSavedSelection();
    static bool SaveSelection(const EffectPackSelection&);
    static EffectPackLut LoadSelection(const std::string& root,const EffectPackSelection&,std::string&);
    static bool VerifyPresentation(wxWindow*);
    static bool VerifyOfflineWorkflow(wxWindow*,const wxString& fixtureRoot);
private:
    PackLibraryDialog(wxWindow*, Applied, Removed, const std::string& isolatedRoot);
    enum class Operation { Catalogue, Detail, Apply, ImportFolder, ImportZip, Remove };
    struct Result;
    Applied applied_;
    Removed removed_;
    std::string root_;
    wxListBox *packs_=nullptr, *assets_=nullptr;
    wxStaticBitmap* preview_=nullptr;
    Studio::GlassLabel *detail_=nullptr,*status_=nullptr;
    wxButton *apply_=nullptr,*remove_=nullptr,*refresh_=nullptr,*folder_=nullptr,*zip_=nullptr,*close_=nullptr;
    wxTimer timer_;
    std::thread worker_;
    std::shared_ptr<Result> result_;
    std::vector<PrismForge::Effects::Packs::PackCatalogueEntry> catalogue_;
    std::vector<std::string> lutIds_;
    bool busy_=false,closeRequested_=false,previewMode_=false;
    void Begin(Operation,const wxString& source={});
    void Poll();
    void UpdateEnabled();
    void SelectPack();
    void CloseWhenReady();
};
