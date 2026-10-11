#include "gui/packlibrarydialog.h"
#include "logger.h"
#include "effects/packs/pack_preview_decoder.h"
#include "effects/packs/pack_validator.h"
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/mstream.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace Packs=PrismForge::Effects::Packs;
namespace {
wxString StorageDirectory() { return wxStandardPaths::Get().GetUserLocalDataDir(); }
wxString SelectionFile() { return StorageDirectory()+"\\effect-pack-selection.ini"; }
wxString Text(const std::string& value) { return wxString::FromUTF8(value); }
std::string NativePath(const wxString& value) { return std::filesystem::path(value.ToStdWstring()).u8string(); }
wxString Integrity(const Packs::PackCatalogueEntry& entry) {
    return entry.isUsable ? "Ready" : "Unavailable";
}
}
struct PackLibraryDialog::Result {
    std::atomic<bool> done{false};
    Operation operation=Operation::Catalogue;
    std::vector<Packs::PackCatalogueEntry> catalogue;
    std::shared_ptr<Packs::LoadedPack> loaded;
    EffectPackLut value;
    EffectPackSelection identity;
    std::vector<uint8_t> thumbnail;
    unsigned width=0,height=0;
    std::string error;
    bool success=false;
};
std::string PackLibraryDialog::LibraryRoot() {
    return NativePath(StorageDirectory()+"\\EffectPacks");
}
EffectPackSelection PackLibraryDialog::ReadSavedSelection() {
    EffectPackSelection selection;
    if(!wxFileExists(SelectionFile()))return selection;
    wxFileInputStream input(SelectionFile());
    if(!input.IsOk()) return selection;
    const auto length=input.GetLength();
    if(length<=0||length>8192) return selection;
    std::vector<char> bytes(size_t(length),0);input.Read(bytes.data(),bytes.size());
    if(input.LastRead()!=bytes.size()) return selection;
    wxMemoryInputStream memory(bytes.data(),bytes.size());wxFileConfig config(memory);
    wxString kind,id,version,asset;
    if(!config.Read("kind",&kind)||kind!="PrismForgeEffectPackSelection1"||
        !config.Read("packId",&id)||!config.Read("version",&version)||!config.Read("assetId",&asset)) return selection;
    std::string error;
    selection={std::string(id.utf8_str()),std::string(version.utf8_str()),std::string(asset.utf8_str())};
    if(!Packs::PackValidator::ValidateIdentifier(selection.packId,false,error)||
        !Packs::PackValidator::ValidateIdentifier(selection.version,true,error)||
        selection.assetId.empty()||selection.assetId.size()>64) return {};
    return selection;
}
bool PackLibraryDialog::SaveSelection(const EffectPackSelection& selection) {
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)) return true;
    if(!wxFileName::DirExists(StorageDirectory())&&
        !wxFileName::Mkdir(StorageDirectory(),0755,wxPATH_MKDIR_FULL)) return false;
    wxMemoryInputStream empty("",0);wxFileConfig config(empty);
    if(!config.Write("kind","PrismForgeEffectPackSelection1")||
        !config.Write("packId",Text(selection.packId))||!config.Write("version",Text(selection.version))||
        !config.Write("assetId",Text(selection.assetId))) return false;
    wxTempFileOutputStream output(SelectionFile());
    return output.IsOk()&&config.Save(output)&&output.Commit();
}
EffectPackLut PackLibraryDialog::LoadSelection(const std::string& root,const EffectPackSelection& selection,std::string& error) {
    EffectPackLut value;
    if(selection.Empty()) {error="No pack colour asset selected";return value;}
    if(!Packs::PackValidator::ValidateIdentifier(selection.packId,false,error)||
        !Packs::PackValidator::ValidateIdentifier(selection.version,true,error)) return value;
    auto path=std::filesystem::u8path(root)/selection.packId/selection.version;
    auto pack=Packs::PackManager::LoadPack(path.u8string(),error);
    if(!pack) return value;
    const auto* manifest=pack->GetVerifiedManifest();
    const auto* descriptor=pack->GetAssetDescriptor(selection.assetId);
    if(!manifest||manifest->packId!=selection.packId||manifest->version!=selection.version||
        !descriptor||descriptor->assetType!=Packs::PackAssetType::CubeLut) {
        error="Selected pack identity or colour asset no longer matches";return value;
    }
    const auto bytes=pack->ReadAssetBytes(selection.assetId);
    if(bytes.empty()||bytes.size()>PrismForge::Effects::CubeLut::MaxPayloadBytes) {
        error="Selected LUT is missing or exceeds the supported size";return value;
    }
    auto lut=PrismForge::Effects::CubeLut::Parse(
        std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),&error);
    if(!lut) return value;
    value.selection=selection;value.lut=std::move(lut);
    value.label=Text(manifest->displayName)+" / "+Text(selection.assetId);
    return value;
}
PackLibraryDialog::PackLibraryDialog(wxWindow* parent,Applied applied,Removed removed)
    :PackLibraryDialog(parent,std::move(applied),std::move(removed),{}) {}
PackLibraryDialog::PackLibraryDialog(wxWindow* parent,Applied applied,Removed removed,const std::string& isolatedRoot)
    :Studio::GlassDialogBase(parent,wxID_ANY,"Effects packs",wxDefaultPosition,parent->FromDIP(wxSize(780,580))),
    applied_(std::move(applied)),removed_(std::move(removed)),root_(isolatedRoot.empty()?LibraryRoot():isolatedRoot),timer_(this) {
    previewMode_=isolatedRoot.empty()&&wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr);
    SetMinSize(FromDIP(wxSize(630,460)));
    auto* layout=new wxBoxSizer(wxVERTICAL);
    auto* intro=Studio::Label(this,"Import a downloaded pack ZIP or pack folder. Apply colour LUT effects offline.",10,true);
    intro->Wrap(FromDIP(720));layout->Add(intro,0,wxEXPAND|wxALL,FromDIP(14));
    auto* actions=new wxBoxSizer(wxHORIZONTAL);
    zip_=Studio::Button(this,"Import ZIP","Install or update a downloaded effects pack.");
    folder_=Studio::Button(this,"Import folder","Install or update an unpacked effects pack.");
    refresh_=Studio::Button(this,"Refresh","Recheck installed packs and their integrity.");
    for(auto* b:{zip_,folder_,refresh_})actions->Add(b,0,wxRIGHT,FromDIP(8));
    layout->Add(actions,0,wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(14));
    auto* content=new wxBoxSizer(wxHORIZONTAL);
    packs_=new wxListBox(this,wxID_ANY);packs_->SetMinSize(FromDIP(wxSize(280,-1)));
    packs_->SetBackgroundColour(Studio::Surface());packs_->SetForegroundColour(Studio::Text());
    content->Add(packs_,1,wxEXPAND|wxRIGHT,FromDIP(14));
    auto* selected=new wxBoxSizer(wxVERTICAL);
    preview_=new wxStaticBitmap(this,wxID_ANY,wxBitmap(192,128));
    preview_->SetMinSize(FromDIP(wxSize(192,128)));selected->Add(preview_,0,wxBOTTOM,FromDIP(10));
    detail_=Studio::Label(this,"Choose an installed pack.",9,true);detail_->Wrap(FromDIP(360));
    selected->Add(detail_,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    selected->Add(Studio::Label(this,"Colour effects",9,true),0,wxBOTTOM,FromDIP(6));
    auto* scope=Studio::Label(this,"This library applies colour LUTs. Other pack asset types are not applied here.",9,true);
    scope->Wrap(FromDIP(340));selected->Add(scope,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    assets_=new wxListBox(this,wxID_ANY);assets_->SetBackgroundColour(Studio::Surface());assets_->SetForegroundColour(Studio::Text());
    selected->Add(assets_,1,wxEXPAND|wxBOTTOM,FromDIP(10));
    apply_=Studio::Button(this,"Apply colour effect","Use this colour effect in preview, snapshots and virtual camera.",true);
    remove_=Studio::Button(this,"Remove version","Remove the selected exact pack version.");
    auto* selectedActions=new wxBoxSizer(wxHORIZONTAL);selectedActions->Add(apply_,1,wxRIGHT,FromDIP(8));selectedActions->Add(remove_,0);
    selected->Add(selectedActions,0,wxEXPAND);content->Add(selected,1,wxEXPAND);
    layout->Add(content,1,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(14));
    status_=Studio::Label(this,"Reading installed packs...",9,true);status_->Wrap(FromDIP(720));
    layout->Add(status_,0,wxEXPAND|wxALL,FromDIP(14));
    close_=Studio::Button(this,"Close","Close the effects pack library.");
    layout->Add(close_,0,wxALIGN_RIGHT|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(14));
    SetSizer(layout);CentreOnParent();
    packs_->Bind(wxEVT_LISTBOX,[this](wxCommandEvent&){SelectPack();});
    assets_->Bind(wxEVT_LISTBOX,[this](wxCommandEvent&){UpdateEnabled();});
    apply_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Begin(Operation::Apply);});
    remove_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
        const int n=packs_->GetSelection();if(n<0||n>=int(catalogue_.size()))return;
        const auto& p=catalogue_[size_t(n)];
        if(wxMessageBox("Remove "+Text(p.displayName.empty()?p.packId:p.displayName)+" ("+Text(p.version)+")?",
            "Remove effects pack",wxYES_NO|wxICON_QUESTION,this)==wxYES)Begin(Operation::Remove);
    });
    refresh_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Begin(Operation::Catalogue);});
    zip_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
        wxFileDialog file(this,"Import effects pack ZIP",{}, {},"Effects pack ZIP (*.zip)|*.zip",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(file.ShowModal()==wxID_OK)Begin(Operation::ImportZip,file.GetPath());
    });
    folder_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
        wxDirDialog folder(this,"Choose the folder containing manifest.json",{},wxDD_DEFAULT_STYLE|wxDD_DIR_MUST_EXIST);
        if(folder.ShowModal()==wxID_OK)Begin(Operation::ImportFolder,folder.GetPath());
    });
    close_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){CloseWhenReady();});
    Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& e){if(busy_&&e.CanVeto()){e.Veto();CloseWhenReady();}else{if(IsModal())EndModal(wxID_CANCEL);else e.Skip();}});
    Bind(wxEVT_BUTTON,[this](wxCommandEvent&){CloseWhenReady();},wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_ESCAPE)CloseWhenReady();else e.Skip();});
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){Poll();},timer_.GetId());
    if(previewMode_){status_->SetLabel("Pack file operations are disabled in UI preview mode.");UpdateEnabled();}
    else Begin(Operation::Catalogue);
}
PackLibraryDialog::~PackLibraryDialog(){timer_.Stop();if(worker_.joinable())worker_.join();}
void PackLibraryDialog::CloseWhenReady(){
    if(busy_){closeRequested_=true;status_->SetLabel("Finishing the current pack operation before closing...");close_->Enable(false);return;}
    if(IsModal())EndModal(wxID_CANCEL);else Hide();
}
void PackLibraryDialog::UpdateEnabled(){
    const int n=packs_->GetSelection();const bool selected=n>=0&&n<int(catalogue_.size());
    for(auto* b:{zip_,folder_,refresh_})b->Enable(!busy_&&!previewMode_);
    packs_->Enable(!busy_);assets_->Enable(!busy_);
    remove_->Enable(!busy_&&!previewMode_&&selected);
    apply_->Enable(!busy_&&!previewMode_&&selected&&catalogue_[size_t(n)].isUsable&&assets_->GetSelection()!=wxNOT_FOUND);
}
void PackLibraryDialog::SelectPack(){
    assets_->Clear();lutIds_.clear();preview_->SetBitmap(wxNullBitmap);
    const int n=packs_->GetSelection();if(n<0||n>=int(catalogue_.size())){UpdateEnabled();return;}
    const auto& p=catalogue_[size_t(n)];
    detail_->SetLabel(Text(p.displayName.empty()?p.packId:p.displayName)+"\nVersion "+Text(p.version)+" | "+Integrity(p)+
        (p.author.empty()?wxString{}:"\nBy "+Text(p.author))+"\n"+Text(p.description)+
        (p.isUsable?wxString{}:"\n"+Text(p.actionableError)));
    detail_->Wrap(FromDIP(360));
    for(const auto& a:p.assets)if(a.assetType==Packs::PackAssetType::CubeLut){assets_->Append(Text(a.assetId));lutIds_.push_back(a.assetId);}
    if(!lutIds_.empty())assets_->SetSelection(0);
    UpdateEnabled();Layout();if(p.isUsable&&!previewMode_)Begin(Operation::Detail);
}
void PackLibraryDialog::Begin(Operation operation,const wxString& source){
    if(busy_||previewMode_)return;
    auto result=std::make_shared<Result>();result->operation=operation;
    if(operation==Operation::Detail||operation==Operation::Apply||operation==Operation::Remove){
        const int n=packs_->GetSelection();if(n<0||n>=int(catalogue_.size()))return;
        const auto& p=catalogue_[size_t(n)];result->identity={p.packId,p.version,{}};
        if(operation==Operation::Apply){const int a=assets_->GetSelection();if(a<0||a>=int(lutIds_.size()))return;result->identity.assetId=lutIds_[size_t(a)];}
    }
    if(worker_.joinable())worker_.join();result_=result;busy_=true;UpdateEnabled();
    status_->SetLabel(operation==Operation::Catalogue?"Checking installed packs...":operation==Operation::Detail?"Checking pack preview...":
        operation==Operation::Apply?"Loading verified colour effect...":operation==Operation::Remove?"Removing pack version...":"Verifying and installing pack...");
    const auto root=root_;const auto path=source.empty()?std::string{}:NativePath(source);
    try{worker_=std::thread([result,root,path]{
        try{
            switch(result->operation){
            case Operation::Catalogue:
                if(!Packs::PackManager::RecoverInterruptedTransactions(root,result->error))break;
                result->catalogue=Packs::PackManager::GetCatalogue(root);result->success=true;break;
            case Operation::Apply:
                result->value=LoadSelection(root,result->identity,result->error);result->success=bool(result->value.lut);break;
            case Operation::Detail:{
                auto pack=Packs::PackManager::LoadPack((std::filesystem::u8path(root)/result->identity.packId/result->identity.version).u8string(),result->error);
                if(!pack)break;const auto* descriptor=pack->GetPreviewDescriptor();
                if(descriptor){auto bytes=pack->GetPreviewBytes();auto decoded=Packs::PackPreviewDecoder::DecodePreviewAsset(*descriptor,bytes.data(),bytes.size());
                    if(!decoded.success){result->error=decoded.errorReason;break;}
                    const auto& image=decoded.image;
                    const double ratio=std::min(192.0/image.Width(),128.0/image.Height());
                    result->width=std::max(1u,unsigned(image.Width()*ratio));result->height=std::max(1u,unsigned(image.Height()*ratio));
                    result->thumbnail.resize(size_t(result->width)*result->height*4);
                    for(unsigned y=0;y<result->height;++y)for(unsigned x=0;x<result->width;++x){
                        const auto src=image.GetPixelAddress(std::min(image.Width()-1,unsigned(x/ratio)),std::min(image.Height()-1,unsigned(y/ratio)));
                        std::copy_n(src,4,result->thumbnail.data()+(size_t(y)*result->width+x)*4);
                    }
                }
                result->success=true;break;
            }
            case Operation::ImportZip:result->success=Packs::PackManager::InstallPackFromArchive(path,root,result->error);break;
            case Operation::ImportFolder:result->success=Packs::PackManager::InstallPack(path,root,result->error);break;
            case Operation::Remove:result->success=Packs::PackManager::UninstallPack(root,result->identity.packId,result->identity.version,result->error);break;
            }
            if(result->success&&(result->operation==Operation::ImportZip||result->operation==Operation::ImportFolder||result->operation==Operation::Remove))
                result->catalogue=Packs::PackManager::GetCatalogue(root);
        }catch(const std::exception& e){result->success=false;result->error=e.what();}catch(...){result->success=false;result->error="Unexpected pack operation failure";}
        result->done.store(true,std::memory_order_release);
    });
    if(!timer_.Start(40)){
        // The worker must never outlive an unobservable busy state. This rare
        // timer failure uses a safe synchronous completion instead of detaching.
        logger<<"[PACK] Status timer unavailable; waiting for owned operation"<<std::endl;
        if(worker_.joinable())worker_.join();
        Poll();
    }
    }catch(const std::exception& e){busy_=false;result_.reset();status_->SetLabel("Could not start pack operation: "+Text(e.what()));UpdateEnabled();}
}
void PackLibraryDialog::Poll(){
    if(!result_||!result_->done.load(std::memory_order_acquire))return;
    timer_.Stop();if(worker_.joinable())worker_.join();auto result=std::move(result_);busy_=false;
    logger<<"[PACK] operation="<<int(result->operation)<<" success="<<result->success<<" error="<<result->error<<std::endl;
    if(!result->success)status_->SetLabel("Pack operation failed: "+Text(result->error));
    else if(result->operation==Operation::Apply){if(applied_)applied_(result->value);status_->SetLabel("Colour effect applied. LUT strength and Bypass effects remain available.");}
    else if(result->operation==Operation::Detail){
        if(!result->thumbnail.empty()){
            wxImage image(int(result->width),int(result->height));image.InitAlpha();auto rgb=image.GetData();auto alpha=image.GetAlpha();
            for(size_t i=0;i<size_t(result->width)*result->height;++i){std::copy_n(result->thumbnail.data()+i*4,3,rgb+i*3);alpha[i]=result->thumbnail[i*4+3];}
            preview_->SetBitmap(wxBitmap(image));
        }
        status_->SetLabel("Pack verified. Select a colour effect to apply.");
    }else{
        if(result->operation==Operation::Remove&&removed_)removed_(result->identity.packId,result->identity.version);
        catalogue_=std::move(result->catalogue);packs_->Clear();assets_->Clear();lutIds_.clear();preview_->SetBitmap(wxNullBitmap);
        for(const auto& p:catalogue_)packs_->Append(Text(p.displayName.empty()?p.packId:p.displayName)+"  "+Text(p.version)+"  ["+Integrity(p)+"]");
        detail_->SetLabel(catalogue_.empty()?"No packs installed. Import a pack ZIP or folder to begin.":"Choose an installed pack.");detail_->Wrap(FromDIP(360));
        wxString message="Pack installed. Choose it to load an effect.";
        if(result->operation==Operation::Remove)message="Pack version removed.";
        else if(result->operation==Operation::Catalogue)message=wxString::Format("%zu installed pack version(s).",catalogue_.size());
        status_->SetLabel(message);
    }
    status_->Wrap(FromDIP(720));UpdateEnabled();Layout();if(closeRequested_)CloseWhenReady();
}
bool PackLibraryDialog::VerifyPresentation(wxWindow* parent){
    PackLibraryDialog dialog(parent,{},{});
    return dialog.previewMode_&&!dialog.busy_&&!dialog.zip_->IsEnabled()&&!dialog.folder_->IsEnabled()&&
        !dialog.apply_->IsEnabled()&&!dialog.remove_->IsEnabled()&&dialog.packs_->IsEmpty();
}

bool PackLibraryDialog::VerifyOfflineWorkflow(wxWindow* parent,const wxString& fixtureRoot)
{
    // Verification is explicitly opt-in and rooted beneath the isolated executable,
    // never the customer's library or selection file. Artifacts are retained.
    if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)||fixtureRoot.empty())return false;
    namespace fs=std::filesystem;
    try{
        const fs::path executable=fs::path(wxStandardPaths::Get().GetExecutablePath().ToStdWstring());
        const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path scratch=executable.parent_path()/fs::u8path("pack-workflow-日本語-"+std::to_string(ticks));
        fs::create_directories(scratch);
        const fs::path fixtures=fs::path(fixtureRoot.ToStdWstring());
        const fs::path archive=scratch/fs::u8path("cool-色.zip");
        {
            wxFileOutputStream output(wxString(archive.wstring()));wxZipOutputStream zip(output);
            const auto source=fixtures/"pack_cool_broadcast";
            for(const auto& entry:fs::recursive_directory_iterator(source))if(entry.is_regular_file()){
                const auto relative=fs::relative(entry.path(),source).generic_u8string();
                if(!zip.PutNextEntry(Text(relative)))return false;
                wxFileInputStream input(wxString(entry.path().wstring()));if(!input.IsOk())return false;
                zip.Write(input);if(!zip.IsOk()||!zip.CloseEntry())return false;
            }
            if(!zip.Close()||!output.Close())return false;
        }
        EffectPackLut selected;unsigned removals=0;
        PackLibraryDialog dialog(parent,[&](const EffectPackLut& value){selected=value;},
            [&](const std::string&,const std::string&){++removals;},(scratch/"library").u8string());
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        auto wait=[&]{
            while(dialog.busy_&&std::chrono::steady_clock::now()<deadline){wxMilliSleep(5);wxYieldIfNeeded();dialog.Poll();}
            return !dialog.busy_&&dialog.status_->GetLabel().find("failed")==wxString::npos;
        };
        if(!wait()||!dialog.catalogue_.empty())return false;
        dialog.Begin(Operation::ImportFolder,wxString((fixtures/"pack_warm_studio").wstring()));
        if(!wait()||dialog.catalogue_.size()!=1)return false;
        dialog.packs_->SetSelection(0);dialog.SelectPack();
        if(!wait()||dialog.lutIds_.empty()||!dialog.preview_->GetBitmap().IsOk())return false;
        dialog.assets_->SetSelection(0);dialog.Begin(Operation::Apply);
        if(!wait()||!selected.lut||selected.selection.version!="1.0.0"||selected.selection.assetId!=dialog.lutIds_[0])return false;
        const auto before=selected.lut->SampleBytes(112,90,60);
        dialog.Begin(Operation::Remove);
        if(!wait()||!dialog.catalogue_.empty()||removals!=1)return false;
        const auto after=selected.lut->SampleBytes(112,90,60);
        if(before.r!=after.r||before.g!=after.g||before.b!=after.b)return false;
        dialog.Begin(Operation::ImportZip,wxString(archive.wstring()));dialog.CloseWhenReady();
        if(!dialog.closeRequested_||!wait()||dialog.catalogue_.size()!=1||dialog.worker_.joinable())return false;
        return true;
    }catch(...){return false;}
}
