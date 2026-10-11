#include "application.h"
#include <cmath>
#include <chrono>
#include <cstring>
extern "C" {
#include <libavutil/pixdesc.h>
}
#include "mfsource/VCamDeviceManager.h"

#include "gui/imgadjdlg.h"
#include "gui/streamconfigdlg.h"
#include "gui/devicesview.h"
#include "gui/qrconview.h"
#include "settings.h"
#include "brand.h"
#include "logger.h"
#include "video/guipreviewscaler.h"
#include <iomanip>
#include <wx/wx.h>
#include <wx/aboutdlg.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
#include <wx/wfstream.h>
#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/stdpaths.h>
#include <wx/mstream.h>
#include <filesystem>
#include <wx/colordlg.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include "gui/studio.h"
#include "gui/appicons.h"

static std::string FavoriteKey(const std::string& name);
static PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings LoadModelPerformanceSettings()
{
    using namespace PrismForge::Effects::Overlays::Realistic;
    ModelPerformanceSettings s;
    auto bounded=[](const char* key,int low,int high,int fallback){const int value=Settings::Get(key);return value>=low&&value<=high?value:fallback;};
    s.mode=ModelPerformanceMode(bounded("STUDIO_MODEL_MODE",0,4,0));
    s.customNodes=bounded("STUDIO_MODEL_NODES",512,16384,4096);
    s.customVertices=bounded("STUDIO_MODEL_VERTICES",1000,HardModelVertices,200000);
    s.customTriangles=bounded("STUDIO_MODEL_TRIANGLES",1000,HardModelTriangles,200000);
    s.customRenderEdge=bounded("STUDIO_MODEL_RENDER_EDGE",256,HardModelRenderEdge,1024);
    return s;
}
static PrismForge::Effects::Overlays::Realistic::ModelHardwareResources DetectModelHardware()
{
    using Microsoft::WRL::ComPtr;
    PrismForge::Effects::Overlays::Realistic::ModelHardwareResources resources;
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory))resources.ramBytes=memory.ullTotalPhys;
    resources.logicalProcessors=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    ComPtr<ID3D11Device> device;const D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0;D3D_FEATURE_LEVEL actual{};
    if(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&requested,1,D3D11_SDK_VERSION,&device,&actual,nullptr))){
        resources.hardwareGpu=true;ComPtr<IDXGIDevice> dxgiDevice;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};
        if(SUCCEEDED(device.As(&dxgiDevice))&&SUCCEEDED(dxgiDevice->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&description))){
            resources.dedicatedVideoBytes=description.DedicatedVideoMemory;resources.adapterName=wxString(description.Description).ToStdString(wxConvUTF8);
        }
    }
    return resources;
}
static float LoadTouchupSetting(const std::string& key,int fallback)
{
    const int value=Settings::Get(key);return (value>=0&&value<=100?value:fallback)/100.f;
}

static float LoadAccessoryScale()
{
    const int value=Settings::Get("STUDIO_ACCESSORY_SCALE");return (value>=50&&value<=150?value:100)/100.f;
}

static int LoadAccessoryCapStyle()
{
    // Only the accepted Pokemon cap is available in the public selector.
    return 2;
}
static bool LoadAccessoryCapEnabled()
{
    return Settings::Get("STUDIO_CAP")==1&&Settings::Get("STUDIO_CAP_STYLE")==2;
}
static int LoadAccessoryGlassesStyle()
{
    const int value=Settings::Get("STUDIO_GLASSES_STYLE");return value>=0&&value<=3?value:0;
}
static float LoadCapScale()
{
    const int value=Settings::Get("STUDIO_CAP_SCALE");return (value>=75&&value<=150?value:100)/100.f;
}
static float LoadCapHeight()
{
    const int value=Settings::Get("STUDIO_CAP_HEIGHT");return (value>=0&&value<=50?value:20)/10.f-2.f;
}

static int LoadFaceFilterStyle()
{
    const int value=Settings::Get("STUDIO_FACE_FILTER");return value>=0&&value<=5?value:0;
}
static float LoadFaceFilterScale(int style=0)
{
    const int value=Settings::Get(style==5?"STUDIO_ROYAL_CROWN_SCALE":"STUDIO_FACE_FILTER_SCALE");return (value>=75&&value<=150?value:100)/100.f;
}
static float LoadFaceFilterHeight(int style=0)
{
    const int value=Settings::Get(style==5?"STUDIO_ROYAL_CROWN_HEIGHT":"STUDIO_FACE_FILTER_HEIGHT");return (value>=0&&value<=60?value:30)/10.f-3.f;
}

static wxString LocalFaceFilterStorage()
{
    return wxStandardPaths::Get().GetUserLocalDataDir()+"\\FaceFilters";
}
static std::wstring LocalFaceFilterLibrary()
{
    wxString isolated;
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)&&wxGetEnv("PRISMFORGE_UI_LOCAL_FILTER_LIBRARY",&isolated)&&!isolated.empty())return isolated.ToStdWstring();
    return (LocalFaceFilterStorage()+"\\Local").ToStdWstring();
}
static wxString LocalFaceFilterSelectionFile()
{
    return LocalFaceFilterStorage()+"\\local-selection.ini";
}
static void LoadLocalFaceFilterIdentity(PrismForge::Effects::DesktopStudioSettings& studio)
{
    const auto file=LocalFaceFilterSelectionFile();if(!wxFileExists(file))return;
    wxFileName info(file);const auto length=info.GetSize();if(length==wxInvalidSize||length>8192)return;
    wxFileInputStream input(file);if(!input.IsOk())return;
    wxFileConfig config(input);wxString kind,path,name;
    if(!config.Read("kind",&kind)||kind!="PrismForgeLocalFaceFilter1"||!config.Read("path",&path)||!config.Read("name",&name))return;
    if(path.length()>4096||name.length()>160)return;
    studio.faceFilterCustomPath=std::string(path.utf8_str());studio.faceFilterCustomName=std::string(name.utf8_str());
}
static void LoadLocalFaceFilterFit(PrismForge::Effects::DesktopStudioSettings& studio)
{
    auto load=[](const char* key,int limit,float divisor){const int value=Settings::Get(key);return (value>=0&&value<=limit*2?value:limit)/divisor-limit/divisor;};
    auto& fit=studio.faceFilterCustomFit;
    fit.xCm=load("STUDIO_LOCAL_FILTER_X",200,10);fit.yCm=load("STUDIO_LOCAL_FILTER_Y",200,10);fit.zCm=load("STUDIO_LOCAL_FILTER_Z",200,10);
    fit.tiltDegrees=load("STUDIO_LOCAL_FILTER_TILT",180,1);fit.turnDegrees=load("STUDIO_LOCAL_FILTER_TURN",180,1);fit.rollDegrees=load("STUDIO_LOCAL_FILTER_ROLL",180,1);
    fit.headOcclusion=Settings::Get("STUDIO_LOCAL_FILTER_OCCLUSION")!=0;
}
static void LoadAccessorySettings(PrismForge::Effects::DesktopStudioSettings& settings)
{
    settings.faceFilterStyle=LoadFaceFilterStyle();settings.faceFilterScale=LoadFaceFilterScale(settings.faceFilterStyle);settings.faceFilterVerticalOffset=LoadFaceFilterHeight(settings.faceFilterStyle);
    LoadLocalFaceFilterFit(settings);
    settings.glasses=Settings::Get("STUDIO_GLASSES")==1;
    settings.glassesStyle=LoadAccessoryGlassesStyle();
    settings.cap=LoadAccessoryCapEnabled();settings.capStyle=LoadAccessoryCapStyle();
    settings.accessoryScale=LoadAccessoryScale();settings.capScale=LoadCapScale();settings.capVerticalOffset=LoadCapHeight();
}

Application::Application()
{
    // wxApp::Initialize later uses the class name for the process shell identity.
    SetClassName("PrismForge.Studio");
	wxInitAllImageHandlers();

	SetAppName("PrismForge Studio");

	if (!wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) Settings::Load();
    const int theme = std::clamp(Settings::Get("STUDIO_THEME"),0,3);
    Studio::appearance = theme;
    SetAppearance((theme == 1 || theme == 3) ? Appearance::Dark : theme == 2 ? Appearance::Light : Appearance::System);
    if (!wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) presets.Load();
	stateRegistry = Settings::GetDeviceStates();
    desktopStudio.light=static_cast<PrismForge::Effects::DesktopLightMode>(std::clamp(Settings::Get("STUDIO_LIGHT_MODE"),0,4));
    desktopStudio.direction=static_cast<PrismForge::Effects::DesktopLightDirection>(std::clamp(Settings::Get("STUDIO_LIGHT_DIRECTION"),0,2));
    const int lightStrength=Settings::Get("STUDIO_LIGHT_STRENGTH");
    desktopStudio.strength=(lightStrength>=0&&lightStrength<=100?lightStrength:50)/100.f;
    desktopStudio.touchupStrength=LoadTouchupSetting("STUDIO_TOUCHUP_STRENGTH",0);
    desktopStudio.touchupDetail=LoadTouchupSetting("STUDIO_TOUCHUP_DETAIL",75);
    desktopStudio.faceLighting=Settings::Get("STUDIO_FACE_LIGHTING")==1;
    LoadAccessorySettings(desktopStudio);
    modelPerformance=LoadModelPerformanceSettings();
    if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr))LoadLocalFaceFilterIdentity(desktopStudio);
    desktopStudio.lightingSubjectOnly=desktopStudio.faceLighting||Settings::Get("STUDIO_LIGHT_SUBJECT_ONLY")==1;
    auto savedValue=[](const std::string& key,int low,int high,int fallback){
        const int value=Settings::Get(key);return value>=low&&value<=high?value:fallback;
    };
    // Offset signed positions so a missing Settings value (-1) remains unambiguous.
    auto loadLight=[&](PrismForge::Effects::Lighting::StudioLight& light,const std::string& prefix){
        light.posX=savedValue(prefix+"_X",0,2000,int(std::lround((light.posX+1)*1000)))/1000.f-1;
        light.posY=savedValue(prefix+"_Y",0,2000,int(std::lround((light.posY+1)*1000)))/1000.f-1;
        light.intensity=savedValue(prefix+"_INTENSITY",0,200,int(std::lround(light.intensity*100)))/100.f;
        light.colorTemperature=float(savedValue(prefix+"_TEMPERATURE",1500,15000,int(std::lround(light.colorTemperature))));
    };
    loadLight(desktopStudio.keyLight,"STUDIO_KEY");loadLight(desktopStudio.fillLight,"STUDIO_FILL");
    desktopStudio.studioFrame=Settings::Get("STUDIO_FRAME")==1;
    desktopStudio.background=static_cast<PrismForge::Effects::DesktopBackgroundMode>(std::clamp(Settings::Get("STUDIO_BACKGROUND_MODE"),0,2));
    const int blur=Settings::Get("STUDIO_BACKGROUND_BLUR");desktopStudio.blurStrength=blur>=0&&blur<=100?blur:35;
    desktopStudio.highQualityMask=Settings::Get("STUDIO_BACKGROUND_QUALITY")!=0;
    const int colour=Settings::Get("STUDIO_BACKGROUND_COLOUR");
    if(colour>=0&&colour<=0xffffff){desktopStudio.backgroundR=(colour>>16)&255;desktopStudio.backgroundG=(colour>>8)&255;desktopStudio.backgroundB=colour&255;}
    if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)) {
        auto configPath=Settings::GetDocumentsDirectoryPath();configPath.SetFullName("desktop-studio.ini");
        wxFileConfig config("PrismForgeStudio",wxEmptyString,configPath.GetFullPath(),wxEmptyString,wxCONFIG_USE_LOCAL_FILE);
        wxString imagePath;if(config.Read("BackgroundImage",&imagePath)&&!imagePath.empty())LoadBackgroundImage(imagePath);
    }
    PublishDesktopStudio();
    const int savedLutIntensity=Settings::Get("DESKTOP_LUT_INTENSITY");
    desktopLutIntensity=savedLutIntensity>=0&&savedLutIntensity<=100?savedLutIntensity:100;
    desktopLook.Select(Settings::Get("DESKTOP_LOOK"));
    PublishDesktopLook();
    if (!wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) {
        auto path = Settings::GetDocumentsDirectoryPath();
        path.SetFullName("desktop-look-presets.ini");
        lookPresets = LookPresetStore(path.GetFullPath());
        if (wxFileExists(path.GetFullPath())) lookPresets.Load();
    }
}

bool Application::OnInit()
{
	if (!wxApp::OnInit())
		return false;

    modelHardware=DetectModelHardware();
    ConfigureDesktopStudio();
    const auto identityResult = PrismForge::AppIcons::InitializeProcessIdentity();
    if (FAILED(identityResult)) logger << "[UI] Shell identity failed: " << identityResult << std::endl;

    // Isolated UI inspection: no sockets, ADB, camera registration, or persisted settings.
    if (wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) {
        mainWindow = new Window({"UI preview", "127.0.0.1", "7969"});
        mainWindow->SetOutputStatus("UI preview mode\nCamera services are not started.");
        mainWindow->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            if (HandleBackgroundAction(e)) return;
            if (HandleDesktopStudioAction(e)) return;
            if (HandleTouchupAction(e)) return;
            if (HandleAccessoryAction(e)) return;
            if (HandleFaceFilterAction(e)) return;
            if (HandleDesktopLookAction(e)) return;
            if (e.GetId() >= Window::THEME_SYSTEM && e.GetId() <= Window::THEME_GLASS) {
                int theme=e.GetId()-Window::THEME_SYSTEM; Studio::appearance=theme;
                SetAppearance((theme==1||theme==3)?Appearance::Dark:theme==2?Appearance::Light:Appearance::System);
                mainWindow->ApplyTheme();
            } else if (e.GetId() == Window::MenuIDs::QR) {
                QrconView qrview("DESKTOP-EG2PIAT", "172.16.0.2", "7969", wxSize(240, 240), mainWindow);
                qrview.ShowModal();
            } else if (e.GetId() == wxID_ABOUT) {
                Studio::AboutDialog(mainWindow).ShowModal();
            } else if (e.GetId() == wxID_EXIT) mainWindow->Close(true);
            else e.Skip();
        });
        PublishDesktopLook();
        PublishDesktopStudio();
        RefreshLookPresets();
        mainWindow->Show();
        if (wxGetEnv("PRISMFORGE_UI_VERIFY_LOOKS", nullptr)) CallAfter([this] {
            bool passed = true;
            auto check = [&](bool condition) { passed = passed && condition; };
            for (int selection = 0; selection != 4; ++selection) {
                mainWindow->GetLookChoice()->SetSelection(selection);
                mainWindow->Dispatch(Window::LOOK_CHANGED);
                check(static_cast<int>(effectsPipeline.GetLook()) == selection);
                check(Settings::Get("DESKTOP_LOOK") == selection);
                mainWindow->Dispatch(Window::LOOK_BYPASS);
                check(effectsPipeline.GetLook() == PrismForge::Effects::Look::Original);
                check(mainWindow->GetLookChoice()->GetSelection() == selection);
                check(mainWindow->GetLookBypassButton()->selected);
                check(mainWindow->GetLookBypassButton()->GetLabel() == "Restore effects");
                mainWindow->Dispatch(Window::LOOK_BYPASS);
                check(static_cast<int>(effectsPipeline.GetLook()) == selection);
                check(!mainWindow->GetLookBypassButton()->selected);
            }
            // Preset application uses the same menu route even without an RTSP manager.
            lookPresets.custom.emplace("UI test preset", DesktopLookPreset{PrismForge::Effects::Look::Cool});
            RefreshLookPresets();
            mainWindow->Dispatch(Window::LOOK_BYPASS);
            check(mainWindow->GetLookPresetChoice()->SetStringSelection("UI test preset"));
            mainWindow->Dispatch(Window::LOOK_PRESET_APPLY);
            check(effectsPipeline.GetLook() == PrismForge::Effects::Look::Cool && !desktopLook.bypassed);
            check(mainWindow->GetLookChoice()->GetSelection() == 2);
            for (int action : {Window::LOOK_PRESET_SAVE, Window::LOOK_PRESET_IMPORT,
                    Window::LOOK_PRESET_EXPORT, Window::LOOK_PRESET_DELETE}) mainWindow->Dispatch(action);
            check(lookPresets.custom.size() == 1);
            // Exercise LUT settings through the actual GUI command route.
            desktopLut = PrismForge::Effects::CubeLut::Parse(
                "LUT_3D_SIZE 2\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n");
            desktopLutName = "UI test.cube";
            desktopLook.selected = PrismForge::Effects::Look::Original;
            desktopLook.bypassed = false;
            PublishDesktopLook();
            check(desktopLut && mainWindow->GetLutClearButton()->IsEnabled());
            AVFrame* sample = av_frame_alloc();
            if (sample) {
                sample->format = AV_PIX_FMT_BGR24; sample->width = 4; sample->height = 2;
                if (av_frame_get_buffer(sample, 32) == 0) {
                    for (int y = 0; y != 2; ++y) for (int x = 0; x != 4; ++x) {
                        auto* p = sample->data[0] + y * sample->linesize[0] + x * 3;
                        p[0] = 40; p[1] = 100; p[2] = 160;
                    }
                    auto* result = effectsPipeline.Process(sample);
                    check(result && result != sample && result->data[0][0] == 160 && result->data[0][2] == 40);
                    mainWindow->GetLutIntensitySlider()->SetValue(50);
                    mainWindow->Dispatch(Window::LUT_INTENSITY);
                    result = effectsPipeline.Process(sample);
                    check(result && result->data[0][0] == 100 && result->data[0][2] == 100);
                    mainWindow->Dispatch(Window::LOOK_BYPASS);
                    check(effectsPipeline.Process(sample) == sample && effectsPipeline.GetSettings().lut == desktopLut);
                    mainWindow->Dispatch(Window::LOOK_BYPASS);
                    result = effectsPipeline.Process(sample);
                    check(result && result->data[0][0] == 100);
                    mainWindow->GetLutIntensitySlider()->SetValue(0);
                    mainWindow->Dispatch(Window::LUT_INTENSITY);
                    check(effectsPipeline.Process(sample) == sample);
                } else check(false);
                av_frame_free(&sample);
            } else check(false);
            mainWindow->Dispatch(Window::LUT_IMPORT); // preview guard: no dialog/files
            check(effectsPipeline.GetSettings().lut == desktopLut);
            mainWindow->Dispatch(Window::LUT_CLEAR);
            check(!effectsPipeline.GetSettings().lut && !mainWindow->GetLutClearButton()->IsEnabled());
            check(!mainWindow->GetLutIntensitySlider()->IsEnabled());
            logger << (passed ? "[UI-VERIFY] Custom LUT controls/pixels PASS" : "[UI-VERIFY] Custom LUT controls/pixels FAIL") << std::endl;
            {
                bool studioPassed=true;auto studioCheck=[&](bool ok){studioPassed=studioPassed&&ok;check(ok);};
                mainWindow->GetLookChoice()->SetSelection(0);mainWindow->Dispatch(Window::LOOK_CHANGED);
                AVFrame* frame=av_frame_alloc();studioCheck(frame!=nullptr);
                if(frame){frame->format=AV_PIX_FMT_BGR24;frame->width=161;frame->height=91;
                    const bool ready=av_frame_get_buffer(frame,64)>=0;studioCheck(ready);
                    if(ready){for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],100,frame->width*3);
                        mainWindow->GetStudioLightChoice()->SetSelection(1);
                        mainWindow->GetStudioLightStrengthSlider()->SetValue(50);mainWindow->Dispatch(Window::STUDIO_LIGHT_MODE);
                        auto* warm=effectsPipeline.Process(frame,101);auto r=effectsPipeline.GetLastReport();
                        studioCheck(r.sourceGeneration==101&&r.frameSequence==1);studioCheck(warm&&warm!=frame&&r.lightingStatus==PrismForge::Effects::StudioStack::StageStatus::Applied);
                        if(warm){auto* pixel=warm->data[0]+45*warm->linesize[0]+80*3;studioCheck(pixel[2]>pixel[0]);}
                        studioCheck(mainWindow->GetStudioLightDirectionChoice()->IsEnabled());
                        mainWindow->GetStudioLightChoice()->SetSelection(2);mainWindow->Dispatch(Window::STUDIO_LIGHT_MODE);
                        mainWindow->GetStudioLightDirectionChoice()->SetSelection(2);mainWindow->Dispatch(Window::STUDIO_LIGHT_DIRECTION);
                        auto* cool=effectsPipeline.Process(frame,102);r=effectsPipeline.GetLastReport();studioCheck(r.sourceGeneration==102&&r.frameSequence==1);
                        if(cool){auto* pixel=cool->data[0]+45*cool->linesize[0]+80*3;studioCheck(pixel[0]>pixel[2]);}else studioCheck(false);
                        mainWindow->GetStudioLightStrengthSlider()->SetValue(0);mainWindow->Dispatch(Window::STUDIO_LIGHT_STRENGTH);
                        studioCheck(effectsPipeline.Process(frame)==frame);
                        mainWindow->GetStudioFrameChoice()->SetSelection(1);mainWindow->Dispatch(Window::STUDIO_FRAME_CHANGED);
                        auto* border=effectsPipeline.Process(frame);studioCheck(border&&border!=frame&&effectsPipeline.GetLastReport().overlayStatus==PrismForge::Effects::StudioStack::StageStatus::Applied);
                        bool changed=false;if(border)for(int y=0;y<91;++y)for(int x=0;x<161*3;++x)if(border->data[0][y*border->linesize[0]+x]!=100)changed=true;
                        studioCheck(changed);mainWindow->Dispatch(Window::LOOK_BYPASS);studioCheck(effectsPipeline.Process(frame)==frame);
                        studioCheck(effectsPipeline.GetStudioSettings().studioFrame);mainWindow->Dispatch(Window::LOOK_BYPASS);
                        mainWindow->GetStudioLightChoice()->SetSelection(0);mainWindow->GetStudioFrameChoice()->SetSelection(0);mainWindow->Dispatch(Window::STUDIO_FRAME_CHANGED);
                        studioCheck(effectsPipeline.Process(frame)==frame&&!mainWindow->GetStudioLightStrengthSlider()->IsEnabled());
                    }av_frame_free(&frame);
                }
                logger<<(studioPassed?"[UI-VERIFY] Studio lighting/frame controls/pixels PASS":"[UI-VERIFY] Studio lighting/frame controls/pixels FAIL")<<std::endl;
            }
            {
                bool customPassed=true;auto customCheck=[&](bool ok){customPassed=customPassed&&ok;check(ok);};
                const auto held=desktopStudio;
                mainWindow->GetStudioLightChoice()->SetSelection(4);
                mainWindow->GetStudioLightStrengthSlider()->SetValue(75);
                mainWindow->GetStudioLightScopeChoice()->SetSelection(0);
                mainWindow->GetStudioLightPositionPad()->SetPositions(-.7f,-.25f,.6f,.35f);
                mainWindow->GetStudioKeyIntensitySlider()->SetValue(125);
                mainWindow->GetStudioFillIntensitySlider()->SetValue(35);
                mainWindow->GetStudioKeyTemperatureSlider()->SetValue(2700);
                mainWindow->GetStudioFillTemperatureSlider()->SetValue(8500);
                mainWindow->Dispatch(Window::STUDIO_LIGHT_MODE);
                auto configured=effectsPipeline.GetStudioSettings();
                customCheck(configured.light==PrismForge::Effects::DesktopLightMode::Custom&&!configured.lightingSubjectOnly&&!configured.faceLighting);
                customCheck(std::abs(configured.keyLight.posX+.7f)<.001f&&std::abs(configured.fillLight.posY-.35f)<.001f);
                customCheck(configured.keyLight.intensity==1.25f&&configured.fillLight.intensity==.35f);
                customCheck(configured.keyLight.colorTemperature==2700&&configured.fillLight.colorTemperature==8500);
                customCheck(Settings::Get("STUDIO_KEY_X")==300&&Settings::Get("STUDIO_FILL_Y")==1350);
                customCheck(Settings::Get("STUDIO_KEY_INTENSITY")==125&&Settings::Get("STUDIO_FILL_TEMPERATURE")==8500);
                customCheck(!mainWindow->GetStudioLightDirectionChoice()->IsEnabled()&&mainWindow->GetStudioLightPositionPad()->IsEnabled());
                customCheck(mainWindow->VerifyCustomLightingControls());
                configured=effectsPipeline.GetStudioSettings();
                customCheck(std::abs(configured.keyLight.posX+.7f)<.001f&&std::abs(configured.fillLight.posY-.35f)<.001f);
                AVFrame* frame=av_frame_alloc();customCheck(frame!=nullptr);
                if(frame){frame->format=AV_PIX_FMT_BGR24;frame->width=161;frame->height=91;
                    const bool ready=av_frame_get_buffer(frame,64)>=0;customCheck(ready);
                    if(ready){for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],100,frame->width*3);
                        auto pixel=[&](AVFrame* f){std::array<uint8_t,3> value{};if(f){auto* p=f->data[0]+45*f->linesize[0]+80*3;std::copy_n(p,3,value.begin());}return value;};
                        auto* result=effectsPipeline.Process(frame,111);const auto first=pixel(result);
                        customCheck(result&&result!=frame&&effectsPipeline.GetLastReport().lightingStatus==PrismForge::Effects::StudioStack::StageStatus::Applied);
                        mainWindow->GetStudioKeyIntensitySlider()->SetValue(0);mainWindow->Dispatch(Window::STUDIO_KEY_INTENSITY);
                        const auto fill=pixel(effectsPipeline.Process(frame));customCheck(fill!=first);
                        mainWindow->GetStudioFillIntensitySlider()->SetValue(0);mainWindow->Dispatch(Window::STUDIO_FILL_INTENSITY);
                        customCheck(effectsPipeline.Process(frame)==frame);
                        mainWindow->GetStudioKeyIntensitySlider()->SetValue(125);mainWindow->Dispatch(Window::STUDIO_KEY_INTENSITY);
                        mainWindow->GetStudioKeyTemperatureSlider()->SetValue(8500);mainWindow->Dispatch(Window::STUDIO_KEY_TEMPERATURE);
                        const auto cool=pixel(effectsPipeline.Process(frame));
                        mainWindow->GetStudioKeyTemperatureSlider()->SetValue(2700);mainWindow->Dispatch(Window::STUDIO_KEY_TEMPERATURE);
                        customCheck(pixel(effectsPipeline.Process(frame))!=cool);
                        const auto layoutsBeforeDrag=mainWindow->GetInspectorLayoutRevisionForTesting();
                        for(int i=0;i<96;++i){
                            const float x=-.9f+i*.018f;
                            mainWindow->GetStudioLightPositionPad()->SetPositions(x,-.25f,-x,.35f);
                            mainWindow->Dispatch(Window::STUDIO_LIGHT_POSITION);
                            customCheck(std::abs(effectsPipeline.GetStudioSettings().keyLight.posX-x)<.00001f);
                        }
                        customCheck(mainWindow->GetInspectorLayoutRevisionForTesting()==layoutsBeforeDrag);
                        logger<<(mainWindow->GetInspectorLayoutRevisionForTesting()==layoutsBeforeDrag?
                            "[UI-VERIFY] Light dragging avoids inspector layout PASS":"[UI-VERIFY] Light dragging avoids inspector layout FAIL")<<std::endl;
                        mainWindow->GetStudioLightPositionPad()->SetPositions(.7f,.25f,-.6f,-.35f);
                        mainWindow->Dispatch(Window::STUDIO_LIGHT_POSITION);
                        customCheck(Settings::Get("STUDIO_KEY_X")==1700&&Settings::Get("STUDIO_FILL_Y")==650);
                        const auto saved=effectsPipeline.GetStudioSettings();mainWindow->Dispatch(Window::LOOK_BYPASS);
                        customCheck(effectsPipeline.Process(frame)==frame&&!mainWindow->GetStudioLightPositionPad()->IsEnabled());
                        mainWindow->GetStudioKeyIntensitySlider()->SetValue(5);mainWindow->Dispatch(Window::STUDIO_KEY_INTENSITY);
                        customCheck(effectsPipeline.GetStudioSettings().keyLight.intensity==saved.keyLight.intensity);
                        mainWindow->Dispatch(Window::LOOK_BYPASS);
                        customCheck(mainWindow->GetStudioKeyIntensitySlider()->GetValue()==125&&mainWindow->GetStudioLightPositionPad()->IsEnabled());
                        mainWindow->GetStudioLightScopeChoice()->SetSelection(1);mainWindow->Dispatch(Window::STUDIO_LIGHT_SCOPE);
                        customCheck(effectsPipeline.GetStudioSettings().lightingSubjectOnly&&Settings::Get("STUDIO_LIGHT_SUBJECT_ONLY")==1);
                        bool faceScopePassed=true;auto faceCheck=[&](bool ok){faceScopePassed=faceScopePassed&&ok;customCheck(ok);};
                        const auto* scope=mainWindow->GetStudioLightScopeChoice();
                        faceCheck(scope->GetCount()==3&&scope->GetString(0)=="Whole frame"&&scope->GetString(1)=="Person only"&&scope->GetString(2)=="Face with shadows");
                        faceCheck(!effectsPipeline.GetStudioSettings().faceLighting&&Settings::Get("STUDIO_FACE_LIGHTING")==0);
                        mainWindow->GetStudioLightScopeChoice()->SetSelection(2);mainWindow->Dispatch(Window::STUDIO_LIGHT_SCOPE);
                        faceCheck(effectsPipeline.GetStudioSettings().faceLighting&&effectsPipeline.GetStudioSettings().lightingSubjectOnly);
                        faceCheck(Settings::Get("STUDIO_FACE_LIGHTING")==1&&Settings::Get("STUDIO_LIGHT_SUBJECT_ONLY")==1);
                        PublishDesktopStudio();faceCheck(scope->GetSelection()==2);
                        mainWindow->Dispatch(Window::LOOK_BYPASS);
                        faceCheck(!scope->IsEnabled()&&scope->GetSelection()==2&&effectsPipeline.GetStudioSettings().faceLighting);
                        mainWindow->GetStudioLightScopeChoice()->SetSelection(0);mainWindow->Dispatch(Window::STUDIO_LIGHT_SCOPE);
                        faceCheck(effectsPipeline.GetStudioSettings().faceLighting&&Settings::Get("STUDIO_FACE_LIGHTING")==1);
                        mainWindow->Dispatch(Window::LOOK_BYPASS);faceCheck(scope->IsEnabled()&&scope->GetSelection()==2);
                        mainWindow->GetStudioLightScopeChoice()->SetSelection(0);mainWindow->Dispatch(Window::STUDIO_LIGHT_SCOPE);
                        faceCheck(!effectsPipeline.GetStudioSettings().lightingSubjectOnly&&!effectsPipeline.GetStudioSettings().faceLighting);
                        faceCheck(Settings::Get("STUDIO_LIGHT_SUBJECT_ONLY")==0&&Settings::Get("STUDIO_FACE_LIGHTING")==0);
                        mainWindow->GetStudioLightScopeChoice()->SetSelection(1);mainWindow->Dispatch(Window::STUDIO_LIGHT_SCOPE);
                        logger<<(faceScopePassed?"[UI-VERIFY] Face lighting scope/persistence PASS":"[UI-VERIFY] Face lighting scope/persistence FAIL")<<std::endl;
                        customCheck(effectsPipeline.GetStudioSettings().background==PrismForge::Effects::DesktopBackgroundMode::Off);
                        mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
                        customCheck(effectsPipeline.Process(frame)==frame&&effectsPipeline.GetStudioSettings().light==PrismForge::Effects::DesktopLightMode::Off);
                        customCheck(effectsPipeline.GetStudioSettings().keyLight.intensity==saved.keyLight.intensity);
                    }av_frame_free(&frame);
                }
                desktopStudio=held;PublishDesktopStudio();
                logger<<(customPassed?"[UI-VERIFY] Custom two-light controls/pixels PASS":"[UI-VERIFY] Custom two-light controls/pixels FAIL")<<std::endl;
            }
            {
                const auto held=desktopStudio;const auto heldLook=desktopLook;const int heldTheme=Studio::appearance;
                bool touchupPassed=mainWindow->VerifyTouchupControls();
                auto touchupCheck=[&](bool ok){touchupPassed=touchupPassed&&ok;check(ok);};
                touchupCheck(touchupPassed);
                Settings::Set("STUDIO_TOUCHUP_STRENGTH",-1);Settings::Set("STUDIO_TOUCHUP_DETAIL",-1);
                touchupCheck(LoadTouchupSetting("STUDIO_TOUCHUP_STRENGTH",0)==0&&LoadTouchupSetting("STUDIO_TOUCHUP_DETAIL",75)==.75f);
                Settings::Set("STUDIO_TOUCHUP_STRENGTH",101);Settings::Set("STUDIO_TOUCHUP_DETAIL",101);
                touchupCheck(LoadTouchupSetting("STUDIO_TOUCHUP_STRENGTH",0)==0&&LoadTouchupSetting("STUDIO_TOUCHUP_DETAIL",75)==.75f);
                desktopLook.bypassed=false;desktopStudio.touchupStrength=0;desktopStudio.touchupDetail=.75f;
                PublishDesktopLook();PublishDesktopStudio();
                auto* strength=mainWindow->GetTouchupStrengthSlider();auto* detail=mainWindow->GetTouchupDetailSlider();
                touchupCheck(strength->GetValue()==0&&detail->GetValue()==75&&strength->GetMin()==0&&strength->GetMax()==100);
                desktopStudio.faceLighting=true;desktopStudio.lightingSubjectOnly=true;
                desktopStudio.light=PrismForge::Effects::DesktopLightMode::Custom;
                desktopStudio.background=PrismForge::Effects::DesktopBackgroundMode::Blur;PublishDesktopStudio();
                const auto heldKey=desktopStudio.keyLight,heldFill=desktopStudio.fillLight;
                const auto layout=mainWindow->GetInspectorLayoutRevisionForTesting();
                for(int value:{0,1,37,100,63,0,45}){
                    strength->SetValue(value);mainWindow->Dispatch(Window::TOUCHUP_STRENGTH);
                    const auto configured=effectsPipeline.GetStudioSettings();
                    touchupCheck(std::abs(configured.touchupStrength-value/100.f)<.00001f&&Settings::Get("STUDIO_TOUCHUP_STRENGTH")==value);
                    touchupCheck(configured.faceLighting&&configured.light==PrismForge::Effects::DesktopLightMode::Custom&&
                        configured.background==PrismForge::Effects::DesktopBackgroundMode::Blur&&
                        configured.keyLight.posX==heldKey.posX&&configured.fillLight.posY==heldFill.posY);
                    touchupCheck(mainWindow->GetInspectorLayoutRevisionForTesting()==layout&&Studio::appearance==heldTheme);
                }
                for(int value:{0,28,75,100,82}){
                    detail->SetValue(value);mainWindow->Dispatch(Window::TOUCHUP_DETAIL);
                    touchupCheck(std::abs(effectsPipeline.GetStudioSettings().touchupDetail-value/100.f)<.00001f&&Settings::Get("STUDIO_TOUCHUP_DETAIL")==value);
                    touchupCheck(mainWindow->GetInspectorLayoutRevisionForTesting()==layout);
                }
                detail->SetValue(100);mainWindow->Dispatch(Window::TOUCHUP_DETAIL);RefreshSegmentationStatus();
                auto* touchupStatus=wxWindow::FindWindowByName("Studio face touch-up status",mainWindow);
                touchupCheck(touchupStatus&&touchupStatus->GetLabel()=="Touch-up: Off"&&touchupStatus->GetToolTipText().Contains("100%"));
                touchupCheck(effectsPipeline.GetStudioSettings().touchupStrength==.45f&&effectsPipeline.GetStudioSettings().touchupDetail==1.f&&
                    Settings::Get("STUDIO_TOUCHUP_STRENGTH")==45&&Settings::Get("STUDIO_TOUCHUP_DETAIL")==100);
                detail->SetValue(82);mainWindow->Dispatch(Window::TOUCHUP_DETAIL);
                touchupCheck(LoadTouchupSetting("STUDIO_TOUCHUP_STRENGTH",0)==.45f&&LoadTouchupSetting("STUDIO_TOUCHUP_DETAIL",75)==.82f);
                mainWindow->Dispatch(Window::LOOK_BYPASS);
                touchupCheck(!strength->IsEnabled()&&!detail->IsEnabled()&&strength->GetValue()==45&&detail->GetValue()==82);
                strength->SetValue(9);detail->SetValue(10);mainWindow->Dispatch(Window::TOUCHUP_STRENGTH);mainWindow->Dispatch(Window::TOUCHUP_DETAIL);
                touchupCheck(effectsPipeline.GetStudioSettings().touchupStrength==.45f&&effectsPipeline.GetStudioSettings().touchupDetail==.82f&&
                    Settings::Get("STUDIO_TOUCHUP_STRENGTH")==45&&Settings::Get("STUDIO_TOUCHUP_DETAIL")==82);
                mainWindow->Dispatch(Window::LOOK_BYPASS);
                touchupCheck(strength->IsEnabled()&&detail->IsEnabled()&&strength->GetValue()==45&&detail->GetValue()==82);
                desktopStudio.light=PrismForge::Effects::DesktopLightMode::Off;PublishDesktopStudio();RefreshSegmentationStatus();
                touchupCheck(effectsPipeline.GetStudioSettings().touchupStrength==.45f);
                mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
                touchupCheck(effectsPipeline.GetStudioSettings().touchupStrength==0&&Settings::Get("STUDIO_TOUCHUP_DETAIL")==82);
                desktopStudio=held;desktopLook=heldLook;PublishDesktopStudio();PublishDesktopLook();
                logger<<(touchupPassed?"[UI-VERIFY] Face touch-up settings/coexistence/bypass PASS":"[UI-VERIFY] Face touch-up settings/coexistence/bypass FAIL")<<std::endl;
            }
            {
                const bool accessories=mainWindow->VerifyAccessoryControls()&&VerifyAccessorySettings();
                check(accessories);
                logger<<(accessories?"[UI-VERIFY] Face accessories settings/coexistence/bypass PASS":
                    "[UI-VERIFY] Face accessories settings/coexistence/bypass FAIL")<<std::endl;
            }
            {
                const bool filters=mainWindow->VerifyFaceFilterControls()&&VerifyFaceFilterSettings();check(filters);
                logger<<(filters?"[UI-VERIFY] 3D face filters settings/coexistence/bypass PASS":"[UI-VERIFY] 3D face filters settings/coexistence/bypass FAIL")<<std::endl;
            }
            {
                wxString fixtures;
                if(wxGetEnv("PRISMFORGE_UI_VERIFY_IMAGE_FIXTURES",&fixtures)) {
                    bool imagesPassed=true;auto imageCheck=[&](bool ok){imagesPassed=imagesPassed&&ok;check(ok);};
                    auto heldImage=desktopStudio.backgroundImage;auto heldName=backgroundImageName,heldPath=backgroundImagePath;
                    imageCheck(LoadBackgroundImage(wxFileName(fixtures,"two-colour.png").GetFullPath()));
                    auto loaded=desktopStudio.backgroundImage;imageCheck(loaded&&loaded->width==2&&loaded->height==1);
                    if(loaded)imageCheck(loaded->data[0][0]==7&&loaded->data[0][1]==90&&loaded->data[0][2]==180&&loaded->data[0][3]==83&&loaded->data[0][4]==52&&loaded->data[0][5]==21);
                    imageCheck(!LoadBackgroundImage(wxFileName(fixtures,"invalid.png").GetFullPath())&&desktopStudio.backgroundImage==loaded);
                    imageCheck(!LoadBackgroundImage(wxFileName(fixtures,"too-wide.png").GetFullPath())&&desktopStudio.backgroundImage==loaded);
                    desktopStudio.backgroundImage=std::move(heldImage);backgroundImageName=heldName;backgroundImagePath=heldPath;
                    logger<<(imagesPassed?"[UI-VERIFY] Background image decode/bounds PASS":"[UI-VERIFY] Background image decode/bounds FAIL")<<std::endl;
                }
                const bool backgroundsPassed=mainWindow->VerifyBackgroundControls();check(backgroundsPassed);
                const bool heldQuality=desktopStudio.highQualityMask;
                mainWindow->GetBackgroundQualityChoice()->SetSelection(0);mainWindow->Dispatch(Window::BACKGROUND_QUALITY);
                bool qualityPassed=!effectsPipeline.GetStudioSettings().highQualityMask&&Settings::Get("STUDIO_BACKGROUND_QUALITY")==0;
                logger<<(backgroundsPassed?"[UI-VERIFY] Background controls PASS":"[UI-VERIFY] Background controls FAIL")<<std::endl;
                mainWindow->GetEffectsPresetChoice()->SetSelection(2);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
                check(effectsPipeline.GetStudioSettings().background==PrismForge::Effects::DesktopBackgroundMode::Blur);
                check(effectsPipeline.GetStudioSettings().blurStrength==35);
                qualityPassed=qualityPassed&&!effectsPipeline.GetStudioSettings().highQualityMask;
                mainWindow->Dispatch(Window::LOOK_BYPASS);
                check(mainWindow->GetBackgroundChoice()->GetSelection()==1&&!mainWindow->GetBackgroundChoice()->IsEnabled());
                qualityPassed=qualityPassed&&!mainWindow->GetBackgroundQualityChoice()->IsEnabled()
                    &&mainWindow->GetBackgroundQualityChoice()->GetSelection()==0;
                mainWindow->GetBackgroundQualityChoice()->SetSelection(1);mainWindow->Dispatch(Window::BACKGROUND_QUALITY);
                qualityPassed=qualityPassed&&!effectsPipeline.GetStudioSettings().highQualityMask;
                mainWindow->Dispatch(Window::LOOK_BYPASS);check(mainWindow->GetBackgroundChoice()->IsEnabled());
                qualityPassed=qualityPassed&&mainWindow->GetBackgroundQualityChoice()->IsEnabled()
                    &&mainWindow->GetBackgroundQualityChoice()->GetSelection()==0;
                mainWindow->GetBackgroundQualityChoice()->SetSelection(1);mainWindow->Dispatch(Window::BACKGROUND_QUALITY);
                qualityPassed=qualityPassed&&effectsPipeline.GetStudioSettings().highQualityMask
                    &&Settings::Get("STUDIO_BACKGROUND_QUALITY")==1;
                logger<<(qualityPassed?"[UI-VERIFY] Background quality routing PASS":"[UI-VERIFY] Background quality routing FAIL")<<std::endl;
                check(qualityPassed);
                mainWindow->GetEffectsPresetChoice()->SetSelection(3);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
                check(effectsPipeline.GetStudioSettings().background==PrismForge::Effects::DesktopBackgroundMode::Replace);
                check(effectsPipeline.GetStudioSettings().light==PrismForge::Effects::DesktopLightMode::Warm);
                mainWindow->Dispatch(Window::BACKGROUND_IMPORT);mainWindow->Dispatch(Window::BACKGROUND_COLOUR);
                mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
                check(effectsPipeline.GetStudioSettings().background==PrismForge::Effects::DesktopBackgroundMode::Off);
                check(effectsPipeline.GetStudioSettings().light==PrismForge::Effects::DesktopLightMode::Off&&!desktopLook.bypassed);
                desktopStudio.highQualityMask=heldQuality;PublishDesktopStudio();
                RefreshSegmentationStatus();
            }
            if (wxGetEnv("PRISMFORGE_UI_VERIFY_SOURCES", nullptr)) {
                const bool sourcesPassed=mainWindow->VerifySourceControls(); check(sourcesPassed);
                logger << (sourcesPassed ? "[UI-VERIFY] Source controls PASS" : "[UI-VERIFY] Source controls FAIL") << std::endl;
            }

            const bool packUiPassed=PackLibraryDialog::VerifyPresentation(mainWindow);
            check(packUiPassed);
            mainWindow->Dispatch(Window::PACK_LIBRARY); // preview guard must avoid opening dialogs/files
            logger<<(packUiPassed?"[UI-VERIFY] Effects pack controls and file guards PASS":"[UI-VERIFY] Effects pack controls and file guards FAIL")<<std::endl;
            wxString packFixtures;
            if(wxGetEnv("PRISMFORGE_UI_VERIFY_PACK_FIXTURES",&packFixtures)){
                const bool workflowPassed=PackLibraryDialog::VerifyOfflineWorkflow(mainWindow,packFixtures);
                check(workflowPassed);
                logger<<(workflowPassed?"[UI-VERIFY] Effects pack worker import/apply/remove/close and Unicode paths PASS":"[UI-VERIFY] Effects pack worker import/apply/remove/close and Unicode paths FAIL")<<std::endl;
            }
            if (wxGetEnv("PRISMFORGE_UI_VERIFY_CONTROLS", nullptr)) check(mainWindow->VerifyControlsLayout());
            if (wxGetEnv("PRISMFORGE_UI_VERIFY_SCROLLING", nullptr)) check(mainWindow->VerifyInspectorScrolling());
            logger << (passed ? "[UI-VERIFY] Desktop look event routing PASS" : "[UI-VERIFY] Desktop look event routing FAIL") << std::endl;
            uiVerificationExitCode = passed ? 0 : 1;
            logger << "[UI-VERIFY] Closing verification window" << std::endl;
            const bool closed = mainWindow->Close(true);
            logger << "[UI-VERIFY] Close returned " << closed << ", top-level windows=" << wxTopLevelWindows.GetCount() << std::endl;
        });
        return true;
    }
    int dsRes = Settings::Get("DIRECTSHOW_RESOLUTION");
	if (dsRes < 0 || dsRes > 3) dsRes = 2; // Default: FHD 1080p
	switch (dsRes + Window::MenuIDs::DS_SD)
	{
		case Window::MenuIDs::DS_SD:  dsSource = std::make_unique<DirectShowSource>(640, 480);   break;
		case Window::MenuIDs::DS_HD:  dsSource = std::make_unique<DirectShowSource>(1280, 720);  break;
		case Window::MenuIDs::DS_FHD: dsSource = std::make_unique<DirectShowSource>(1920, 1080); break;
		case Window::MenuIDs::DS_QHD: dsSource = std::make_unique<DirectShowSource>(3840, 2160); break;
		default:                      dsSource = std::make_unique<DirectShowSource>(1920, 1080); break;
	}

    dsSource->Pause(); // No active input at startup.
	int controlPort = Settings::Get("CONTROL_PORT");
	if (controlPort < 1024 || controlPort > 65535) controlPort = PrismForge::DEFAULT_CONTROL_PORT;
	Settings::Set("CONTROL_PORT", controlPort);
	try {
		server = std::make_unique<Server>(controlPort, *this);
		// Accept only after the window, input manager and callbacks exist below.
	}
	catch (const std::exception& e) {
		wxMessageBox(
			wxString::Format("Critical Error: Could not start network server.\n\nReason: %s\n\nPlease check if control port %d is in use.", e.what(), controlPort),
			"Initialization Failed",
			wxOK | wxICON_ERROR | wxCENTRE,
			nullptr
		);

		// Exit application if the server fails to start
		return false;
	}

	logger << "[APP] Resolving host endpoint" << std::endl;
	auto hostInfo = server->GetHostInfo();
	logger << "[APP] Constructing Studio window" << std::endl;
	mainWindow = new Window(hostInfo);
	logger << "[APP] Studio window constructed" << std::endl;

    rtspManager = std::make_unique<RTSP::Manager>(*server,
        RTSP::Receiver::OnFrameReceivedCallback{},RTSP::Receiver::OnStatsReceivedCallback{});
    systemCamera = std::make_unique<PrismForge::Capture::SystemCameraSource>();
    if (!frameDelivery.Start(inputGate.Current(), [this](uint64_t token, AVFrame* frame, double waitMs) {
        ProcessInputFrame(token,frame,waitMs);
    })) { logger << "[APP] Frame delivery worker could not start" << std::endl; return false; }

	BindEventListeners();
    // A fast USB companion can connect immediately: descriptors must not be dropped
    // while mainWindow or rtspManager is still absent. USB recovery also starts here.
    server->Start();
	mainWindow->Show();
	logger << "[APP] Studio window shown" << std::endl;
	logger << "[CAM] Initializing modern camera" << std::endl;
	VCamDeviceManager::Instance().Initialize();
	logger << "[CAM] Modern camera initialization returned" << std::endl;
    RefreshPresets();
    RefreshLookPresets();
    PublishDesktopLook();
    PublishDesktopStudio();
    segmentationTimer.SetOwner(this);
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(!closing&&mainWindow)RefreshSegmentationStatus();},segmentationTimer.GetId());
    segmentationTimer.Start(1000);
    RefreshSystemCameras();
    RestorePackSelection();
    RestoreLocalFaceFilter();
    logger << "[APP] Startup complete" << std::endl;

	return true;
}

StreamOptions& Application::GetCurrentDeviceStreamOptions()
{
	auto deviceId = rtspManager->GetStreamingDevice();
	const auto& descs = rtspManager->GetDescriptors();
	if (deviceId >= 0 && deviceId < (int)descs.size())
	{
		return stateRegistry[descs[deviceId].name()];
	}
	static StreamOptions s_fallback;
	return s_fallback;
}

void Application::BindEventListeners()
{
	mainWindow->Bind(wxEVT_CLOSE_WINDOW, &Application::OnWindowCloseEvent, this);
	mainWindow->Bind(wxEVT_MENU, &Application::OnMenuEvent, this);

	mainWindow->GetSourceChoice()->Bind(wxEVT_CHOICE, &Application::OnSourceChanged, this);
	mainWindow->GetAdjustmentsButton()->Bind(wxEVT_BUTTON, &Application::ShowAdjustmentsDialog, this);
	mainWindow->GetStreamOptionsButton()->Bind(wxEVT_BUTTON, &Application::ShowStreamConfigDialog, this);

	mainWindow->GetRotateLeftButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
        if (sourceKind == PrismForge::Input::Kind::Mobile) rtspManager->Rotate(-90);
	});

	mainWindow->GetRotateRightButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
        if (sourceKind == PrismForge::Input::Kind::Mobile) rtspManager->Rotate(90);
	});

	mainWindow->GetFlipButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
        if (sourceKind != PrismForge::Input::Kind::Mobile) return;
		rtspManager->FlipHorizontally();
		auto* b = dynamic_cast<Studio::SmoothButton*>(mainWindow->GetFlipButton());
		if (b) { b->selected = !b->selected; b->Refresh(); }
	});

	mainWindow->GetFlipVerticalButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
        if (sourceKind != PrismForge::Input::Kind::Mobile) return;
		rtspManager->FlipVertically();
		auto* b = dynamic_cast<Studio::SmoothButton*>(mainWindow->GetFlipVerticalButton());
		if (b) { b->selected = !b->selected; b->Refresh(); }
	});

	// Check if devices exist before accessing options to prevent crashes
	mainWindow->GetTorchButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
		if (rtspManager->GetStreamingDevice() < 0) return;
		auto& options = GetCurrentDeviceStreamOptions();
		auto flash = options.flashEnabled = !options.flashEnabled;
		rtspManager->SetFlash(flash);
        RefreshStudio();
	});

	mainWindow->GetSwapButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
		if (rtspManager->GetStreamingDevice() < 0) return;
		auto& options = GetCurrentDeviceStreamOptions();
		options.backCameraActive = !options.backCameraActive;
		rtspManager->SwapCamera();
        RefreshStudio();
	});

	mainWindow->GetZoomInButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
		if (rtspManager->GetStreamingDevice() < 0) return;
		auto& options = GetCurrentDeviceStreamOptions();
		options.zoom = std::min(10.0f, options.zoom + 0.5f);
		mainWindow->GetZoomLevelLabel()->SetLabelText(wxString::Format("%.1fx", options.zoom));
		rtspManager->Zoom(options.zoom);
	});

	mainWindow->GetZoomOutButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
		if (rtspManager->GetStreamingDevice() < 0) return;
		auto& options = GetCurrentDeviceStreamOptions();
		options.zoom = std::max(1.0f, options.zoom - 0.5f);
		mainWindow->GetZoomLevelLabel()->SetLabelText(wxString::Format("%.1fx", options.zoom));
		rtspManager->Zoom(options.zoom);
	});

	snapshotManager.onSaved = [this](bool ok) {
        if (!closing && mainWindow) mainWindow->Notify(ok ? "Snapshot saved. Open snapshots to view your photo." : "Snapshot could not be saved. Check available space and folder permissions.");
    };
	mainWindow->GetSnapshotButton()->Bind(wxEVT_BUTTON, [&](const wxEvent& arg) {
        if (sourceKind==PrismForge::Input::Kind::None) return;
		snapshotManager.RequestSnapshot();
        mainWindow->Notify("Snapshot requested. The next received frame will be saved.");
	});
}

void Application::OnDeviceConnected(DeviceDescriptor& descriptor) const
{
	DeviceDescriptor descCopy = descriptor;
	if (mainWindow && mainWindow->GetEventHandler())
	{
		mainWindow->GetEventHandler()->CallAfter([this, descCopy]() mutable {
            if (closing || !rtspManager || !mainWindow) return;
			if (mainWindow && mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon("New stream available", "Streaming device " + descCopy.name() + " available!", 10, wxICON_INFORMATION);
			rtspManager->AddDescriptor(descCopy);
			const_cast<Application*>(this)->UpdateAvailableDevices();
		});
	}
}

void Application::OnDeviceDisconnected(DeviceDescriptor& descriptor) const
{
	DeviceDescriptor descCopy = descriptor;
	if (mainWindow && mainWindow->GetEventHandler())
	{
		mainWindow->GetEventHandler()->CallAfter([this, descCopy]() {
            if (closing || !rtspManager || !mainWindow) return;
			if (mainWindow && mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon("Stream ended", "Streaming device " + descCopy.name() + " disconnected!", 10, wxICON_INFORMATION);

			int streamingDeviceId = rtspManager->GetStreamingDevice();
			if (streamingDeviceId >= 0 && streamingDeviceId < (int)rtspManager->GetDescriptors().size())
			{
				const auto& streamingDescriptor = rtspManager->GetDescriptors()[streamingDeviceId];
				if (streamingDescriptor == descCopy && mainWindow && mainWindow->GetCanvas())
					mainWindow->GetCanvas()->Clear();
			}

            auto* app=const_cast<Application*>(this);
            if (app->activeSourceKey==PrismForge::Input::MobileKey(descCopy.name(),descCopy.url())) {
                app->StopInputs(); app->sourceKind=PrismForge::Input::Kind::None; app->activeSourceKey.clear();
            }
			rtspManager->RemoveDescriptor(const_cast<DeviceDescriptor&>(descCopy));
			const_cast<Application*>(this)->UpdateAvailableDevices();
		});
	}
}

void Application::OnDeviceErrorReported(DeviceDescriptor& descriptor, const Connection::ErrorReport& error) const
{
	std::string name = descriptor.name();
	auto report = error;
	if (mainWindow && mainWindow->GetEventHandler())
	{
		mainWindow->GetEventHandler()->CallAfter([this, name, report]() {
            if (closing || !rtspManager || !mainWindow) return;
			auto icon = report.severity == Connection::ErrorReport::SEVERITY_WARNING ? wxICON_WARNING : wxICON_ERROR;
			if (mainWindow && mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon(name + " " + report.error, report.description, 1000, icon);
            mainWindow->Notify(wxString::FromUTF8(report.error + ": " + report.description));
		});
	}
}

void Application::StopInputs()
{
    const auto generation=inputGate.Advance(); // Drain the current processed frame before source teardown.
    frameDelivery.SetGeneration(generation); // Discard pending old frames and reject late old submissions.
    deliveredFpsEstimate=0;
    if (rtspManager) rtspManager->DisconnectStream();
    if (systemCamera) systemCamera->Stop();
    if (cameraModesWorker.joinable()) cameraModesWorker.join();
    if (dsSource) dsSource->Pause();
}

void Application::DeliverInputFrame(uint64_t token, AVFrame* frame)
{
    if (!closing && frame && inputGate.IsCurrent(token)) frameDelivery.Submit(token,frame);
}

void Application::ProcessInputFrame(uint64_t token, AVFrame* frame, double queueWaitMs)
{
    if (closing || !frame) return;
    inputGate.Deliver(token, [this, frame, token, queueWaitMs] {
        if (closing) return;
        const auto begin=std::chrono::steady_clock::now();
        AVFrame* processed = effectsPipeline.Process(frame,token);
        const auto effectsDone=std::chrono::steady_clock::now();
        if (!processed) return;
        if (mainWindow && mainWindow->GetCanvas()) mainWindow->GetCanvas()->ProcessRawFrameAsync(processed);
        const auto previewDone=std::chrono::steady_clock::now();
        DirectShowSource::DeliveryTiming outputTiming;
        if (dsSource) dsSource->SendRawFrame(processed, &outputTiming); // MF and DirectShow share this result.
        const auto outputDone=std::chrono::steady_clock::now();
        snapshotManager.ProcessFrame(processed);
        const auto end=std::chrono::steady_clock::now();
        auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        if(perfGeneration!=token){perfGeneration=token;perfFrames.clear();perfEffects=perfPreview=perfOutput=perfMf=perfDs=0;}
        if(perfFrames.empty()) perfWallStart=begin;
        perfFrames.push_back(ms(begin,end));perfWaits[perfFrames.size()-1]=queueWaitMs;perfEffects+=ms(begin,effectsDone);
        perfPreview+=ms(effectsDone,previewDone);perfOutput+=ms(previewDone,outputDone);
        perfMf+=outputTiming.mediaFoundationMs;perfDs+=outputTiming.directShowMs;
        if(perfFrames.size()==120){
            const double interval=ms(perfWallStart,begin);
            const double deliveredFps=interval>0 ? 119000.0/interval : 0;
            deliveredFpsEstimate=static_cast<int>(std::lround(deliveredFps));
            const char* format=av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            std::sort(perfFrames.begin(),perfFrames.end());
            std::sort(perfWaits.begin(),perfWaits.end());
            const auto queueStats=frameDelivery.GetStats();
            logger<<"[PERF] Live shared delivery "<<processed->width<<"x"<<processed->height
                <<" samples=120 p50_ms="<<perfFrames[59]<<" p95_ms="<<perfFrames[113]
                <<" effects_mean_ms="<<perfEffects/120<<" preview_mean_ms="<<perfPreview/120
                <<" output_mean_ms="<<perfOutput/120<<" mf_mean_ms="<<perfMf/120<<" ds_mean_ms="<<perfDs/120
                <<" delivered_fps="<<deliveredFps<<" input_format="<<(format?format:"unknown")
                <<" input_range="<<frame->color_range<<" input_matrix="<<frame->colorspace
                <<" queue_wait_p95_ms="<<perfWaits[113]<<" replaced_total="<<queueStats.replaced
                <<" max_outstanding="<<queueStats.maxOutstanding<<std::endl;
            perfFrames.clear();perfEffects=perfPreview=perfOutput=perfMf=perfDs=0;
        }
    });
}

void Application::RefreshSystemCameras()
{
    const auto report = PrismForge::Capture::SystemCameraCatalog::Enumerate();
    systemDevices = report.devices;
    if (systemDevice) {
        auto found = std::find_if(systemDevices.begin(), systemDevices.end(), [this](const auto& d) { return d.id == systemDevice->id; });
        if (found == systemDevices.end()) {
            StopInputs(); systemDevice.reset(); activeSourceKey.clear();
            sourceKind = PrismForge::Input::Kind::None;
            mainWindow->Notify("Selected camera is no longer available.");
        }
    }
    UpdateAvailableDevices();
    for (const auto& result : report.backendResults)
        if (!result.success) mainWindow->Notify("Some camera inputs could not be listed. Use Refresh to retry.");
}

void Application::StartSystemCamera(const PrismForge::Capture::CaptureDevice& selected,
    const PrismForge::Capture::CaptureMode* requested)
{
    using namespace PrismForge::Capture;
    const CaptureDevice device = selected; // StopInputs may run pending owner cleanup.
    const auto mode = requested ? std::optional<CaptureMode>(*requested) : std::nullopt;
    StopInputs();
    sourceKind = PrismForge::Input::Kind::System;
    activeSourceKey = "system:" + device.id;
    systemDevice = device;
    selectedSystemMode = mode;
    systemStatus = {}; systemStatus.state = CaptureState::Starting;
    const uint64_t token = inputGate.Current();
    if (dsSource) dsSource->Resume();
    mainWindow->GetCanvas()->Clear(); mainWindow->BeginStream();
    mainWindow->GetCanvas()->SetEmptyState("Connecting to your camera", "Waiting for the first video frame...");
    RefreshStudio();
    auto stats = std::make_shared<CameraFrameStats>();
    systemCamera->SetFrameCallback([this, token, stats](AVFrame* frame) {
        DeliverInputFrame(token,frame);
        if (!inputGate.IsCurrent(token) || closing) return;
        ++stats->frames;
        auto now=std::chrono::steady_clock::now();
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-stats->start).count();
        if (ms >= 1000 || stats->frames == 1) {
            int fps=ms >= 1000 ? static_cast<int>(stats->frames*1000/ms) : 0;
            const int width=frame->width, height=frame->height;
            CallAfter([this,token,width,height,fps] {
                if (!closing && inputGate.IsCurrent(token) && mainWindow) {
                    const int delivered=deliveredFpsEstimate.load();
                    mainWindow->SetStreamStats(width,height,delivered>0?delivered:fps,0);
                }
            });
            if (ms >= 1000) { stats->start=now; stats->frames=0; }
        }
    });
    systemCamera->SetStatusCallback([this, token](const CaptureStatus& value) {
        if (closing) return;
        CallAfter([this,token,value] {
            if (closing || !inputGate.IsCurrent(token)) return;
            systemStatus=value;
            if (dsSource) {
                if (value.state==PrismForge::Capture::CaptureState::Running) dsSource->Resume();
                else if (value.state==PrismForge::Capture::CaptureState::Error || value.state==PrismForge::Capture::CaptureState::Disconnected)
                    dsSource->Pause();
            }
            RefreshStudio();
        });
    });
    if (mode) {
        systemCamera->Start(device,&*mode);
        return;
    }
    // Query only the chosen camera, off the UI thread. Start occurs on the UI
    // owner after checking generation so an obsolete query cannot reopen a source.
    cameraModesWorker = std::thread([this,device,token] {
        auto modes=SystemCameraCatalog::QueryCapabilities(device);
        if (closing || !inputGate.IsCurrent(token)) return;
        modes.erase(std::remove_if(modes.begin(),modes.end(),[](const CaptureMode& m) {
            return m.width <= 0 || m.width > 16384 || m.height <= 0 || m.height > 16384 || m.interlaced ||
                (m.pixelFormat != "NV12" && m.pixelFormat != "YUY2" && m.pixelFormat != "RGB24" &&
                 m.pixelFormat != "RGB32" && m.pixelFormat != "MJPG" && m.pixelFormat != "H264") ||
                (m.pixelFormat=="NV12" && ((m.width%2) || (m.height%2))) || (m.pixelFormat=="YUY2" && (m.width%2));
        }),modes.end());
        CallAfter([this,device,token,modes=std::move(modes)] {
            if (closing || !inputGate.IsCurrent(token)) return;
            systemModes=modes; RefreshStudio();
            systemCamera->Start(device);
        });
    });
}

void Application::UpdateAvailableDevices()
{
    using namespace PrismForge::Input;
    auto* choice=mainWindow->GetSourceChoice(); choice->Clear();
    sourceChoices.clear();
    auto add=[&](Kind kind,const std::string& key,const wxString& label) {
        sourceChoices.push_back({kind,key,label.ToUTF8().data()});
        if (kind==Kind::Heading) choice->AppendHeading(label); else choice->Append(label);
    };
    add(Kind::None,"","No camera selected");
    add(Kind::Heading,"","Mobile cameras");
    const auto& phones=rtspManager->GetDescriptors();
    for (const auto& d : phones) add(Kind::Mobile,MobileKey(d.name(),d.url()),
        (Settings::Get(FavoriteKey(d.name()))==1 ? "[Pinned] " : "") + wxString::FromUTF8(d.name()) +
        (d.url().find("127.0.0.1")!=std::string::npos ? "  |  USB" : "  |  Wi-Fi"));
    if (phones.empty()) add(Kind::Heading,"","No phone connected");
    add(Kind::Heading,"","System cameras");
    for (const auto& d : systemDevices) add(Kind::System,"system:"+d.id,wxString::FromUTF8(d.displayName));
    if (systemDevices.empty()) add(Kind::Heading,"","No system camera found");
    choice->SetSelection(FindChoice(sourceChoices,activeSourceKey));
    if (allowPhoneAutoSelect && sourceKind==Kind::None && activeSourceKey.empty() && phones.size()==1) {
        int index=FindChoice(sourceChoices,MobileKey(phones[0].name(),phones[0].url()));
        choice->SetSelection(index); wxCommandEvent event(wxEVT_CHOICE); OnSourceChanged(event);
    } else RefreshStudio();
}

void Application::OnMenuEvent(wxCommandEvent& event)
{
    if (HandleBackgroundAction(event)) return;
    if (HandleDesktopStudioAction(event)) return;
    if (HandleTouchupAction(event)) return;
    if (HandleAccessoryAction(event)) return;
    if (HandleFaceFilterAction(event)) return;
    if (HandleDesktopLookAction(event)) return;
    if (HandleStudioAction(event)) return;
	switch (event.GetId())
	{
		case Window::MenuIDs::DEVICES:
		{
			DevicesView devlistview(mainWindow, rtspManager->GetDescriptors());
			devlistview.ShowModal();
			break;
		}

		case Window::MenuIDs::QR:
		{
			auto info = server->GetHostInfo();
			QrconView qrview(std::get<0>(info), std::get<1>(info), std::get<2>(info), wxSize(240, 240), mainWindow);
			qrview.ShowModal();
			break;
		}

		case Window::MenuIDs::HIDE2TRAY:
		{
			mainWindow->HideToTray();
			break;
		}

		case Window::MenuIDs::MINIMIZE_ON_CLOSE:
		{
			Settings::Set("MINIMIZE_TASKBAR", event.IsChecked() ? 1 : 0);
			break;
		}

		case Window::MenuIDs::SHOWSTATS:
		{
			Settings::Set("SHOW_STATS", event.IsChecked() ? 1 : 0);
			mainWindow->GetStatsText()->Show(event.IsChecked());
			mainWindow->Layout(); // Refresh layout to hide/show properly
			break;
		}

		case Window::MenuIDs::SAVESTATE:
		{
			Settings::Set("SAVE_DEVICE_STATES", event.IsChecked() ? 1 : 0);
			break;
		}

		case wxID_ABOUT:
		{
			Studio::AboutDialog(mainWindow).ShowModal();
			break;
		}

		case wxID_EXIT:
		{
			mainWindow->Close(true);
			break;
		}

		case Window::MenuIDs::VCAM_DYNAMIC:
		{
			VCamDeviceManager::Instance().SetMode(VCamMode::Dynamic, (HWND)mainWindow->GetHWND());
			if (mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon(
					"Windows Camera Device",
					"Camera set to show only while PrismForge is active (Dynamic).",
					2000,
					wxICON_INFORMATION
				);
			break;
		}

		case Window::MenuIDs::VCAM_ALWAYS:
		{
			bool ok = VCamDeviceManager::Instance().SetMode(VCamMode::Always, (HWND)mainWindow->GetHWND());
			if (ok && mainWindow->GetTaskbarIcon()) {
				mainWindow->GetTaskbarIcon()->ShowBalloon(
					"Windows Camera Device",
					"Camera is now permanently active in Windows Settings.",
					2000,
					wxICON_INFORMATION
				);
			}
			break;
		}

		case Window::MenuIDs::VCAM_DISABLED:
		{
			VCamDeviceManager::Instance().SetMode(VCamMode::Disabled, (HWND)mainWindow->GetHWND());
			if (mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon(
					"Windows Camera Device",
					"Virtual camera hidden from Windows Settings (DirectShow only).",
					2000,
					wxICON_INFORMATION
				);
			break;
		}

		case Window::MenuIDs::DS_SD:
		case Window::MenuIDs::DS_HD:
		case Window::MenuIDs::DS_FHD:
		case Window::MenuIDs::DS_QHD:
		{
			int resIndex = event.GetId() - Window::MenuIDs::DS_SD;
			Settings::Set("DIRECTSHOW_RESOLUTION", resIndex);
			Settings::Save();

			int w = 1280, h = 720;
			switch (event.GetId())
			{
				case Window::MenuIDs::DS_SD:  w = 640;  h = 480;  break;
				case Window::MenuIDs::DS_HD:  w = 1280; h = 720;  break;
				case Window::MenuIDs::DS_FHD: w = 1920; h = 1080; break;
				case Window::MenuIDs::DS_QHD: w = 3840; h = 2160; break;
			}

			if (dsSource)
			{
				dsSource->Recreate(w, h);
                if (sourceKind==PrismForge::Input::Kind::None ||
                    (sourceKind==PrismForge::Input::Kind::System && systemStatus.state!=PrismForge::Capture::CaptureState::Starting &&
                     systemStatus.state!=PrismForge::Capture::CaptureState::Running)) dsSource->Pause();
			}

			if (mainWindow->GetTaskbarIcon())
				mainWindow->GetTaskbarIcon()->ShowBalloon(
					"DirectShow Resolution",
					wxString::Format("Virtual camera resolution set to %dx%d.", w, h),
					2000,
					wxICON_INFORMATION
				);
			break;
		}
        default: event.Skip(); break;
	}
    if (!closing) RefreshStudio();
}

void Application::OnSourceChanged(wxEvent& event)
{
    using namespace PrismForge::Input;
    const int sel=mainWindow->GetSourceChoice()->GetSelection();
    if (sel < 0 || sel >= (int)sourceChoices.size()) return;
    const auto selected=sourceChoices[sel];
    if (selected.kind==Kind::Heading) return;
    allowPhoneAutoSelect = false; // Explicit selection/stop is preserved on Refresh and phone discovery.
    if (selected.kind==Kind::None) {
        StopInputs(); sourceKind=Kind::None; activeSourceKey.clear(); systemDevice.reset(); RefreshStudio(); return;
    }
    if (selected.kind==Kind::System) {
        for (const auto& d : systemDevices) if ("system:"+d.id==selected.key) {
            systemModes.clear(); StartSystemCamera(d); return;
        }
        RefreshSystemCameras(); return;
    }
    const auto& phones=rtspManager->GetDescriptors();
    int deviceId=-1;
    for (size_t i=0;i<phones.size();++i)
        if (MobileKey(phones[i].name(),phones[i].url())==selected.key) { deviceId=(int)i; break; }
    if (deviceId < 0) return;
    StopInputs(); sourceKind=Kind::Mobile; activeSourceKey=selected.key; systemDevice.reset();
    const uint64_t token=inputGate.Current();
    if (dsSource) dsSource->Resume();

	const auto& descriptor = rtspManager->GetDescriptors()[deviceId];

	EnsureStateInitialized(descriptor.name(), descriptor);
	auto& state = stateRegistry[descriptor.name()];

	// Validate Resolution exists in current capabilities
	bool resFound = false;
	const auto& resList = state.backCameraActive ? descriptor.backResolutions() : descriptor.frontResolutions();

	if (!resList.empty())
	{
		for (size_t i = 0; i < resList.size(); i++)
		{
			if (resList[i] == state.resolution)
			{
				resFound = true;
				break;
			}
		}
		if (!resFound)
		{
			// Try finding 1920x1080 first
			for (const auto& r : resList) {
				if (r.first == 1920 && r.second == 1080) {
					state.resolution = r;
					resFound = true;
					break;
				}
			}
			// If not found, try 1280x720
			if (!resFound) {
				for (const auto& r : resList) {
					if (r.first == 1280 && r.second == 720) {
						state.resolution = r;
						resFound = true;
						break;
					}
				}
			}
			// Fallback to first available resolution
			if (!resFound) {
				state.resolution = resList[0];
			}
		}
	}
	else
	{
		// Handle error case: Device reported 0 resolutions
		if (mainWindow->GetTaskbarIcon())
			mainWindow->GetTaskbarIcon()->ShowBalloon("Error", "Device reported no supported resolutions.", 10, wxICON_WARNING);
	}

	mainWindow->GetCanvas()->Clear();
    mainWindow->GetCanvas()->SetEmptyState("Connecting to your camera", "Waiting for the first video frame...");
    mainWindow->BeginStream();
    rtspManager->Connect2Stream(deviceId, state,
        [this,token](AVFrame* frame) { DeliverInputFrame(token,frame); },
        [this,token](const RTSP::Receiver::Stats& stats) {
            if (closing) return;
            CallAfter([this,token,stats] {
                if (!closing && inputGate.IsCurrent(token) && mainWindow) {
                    const int delivered=deliveredFpsEstimate.load();
                    mainWindow->SetStreamStats(stats.width,stats.height,delivered>0?delivered:stats.fps,stats.bitrate);
                }
            });
        });
    RefreshStudio();
    mainWindow->Notify("Connecting. If video does not arrive, use Reconnect (Ctrl+R).");

	// Update UI Zoom Label to match state
	mainWindow->GetZoomLevelLabel()->SetLabelText(wxString::Format("%.1fx", state.zoom));
}

void Application::OnWindowCloseEvent(wxCloseEvent& event)
{
	if (event.CanVeto() && Settings::Get("MINIMIZE_TASKBAR") == 1)
	{
		event.Veto();
		mainWindow->HideToTray();
		return;
	}

	closing = true;
    localFaceFilterTimer.Stop();
    if(localFaceFilterResult)localFaceFilterResult->cancel.store(true,std::memory_order_release);
    mainWindow->SaveGeometry();
    // Hide window for responsive UI close feeling
	mainWindow->Hide();
    StopInputs(); frameDelivery.Stop(); systemCamera.reset();
	VCamDeviceManager::Instance().Shutdown();

	rtspManager.reset();
    snapshotManager.Wait();
	server->Close();

	Settings::UpdateDeviceStates(stateRegistry);
	Settings::Save();

	event.Skip();
}

void Application::EnsureStateInitialized(std::string name, const DeviceDescriptor& descriptor)
{
	// operator[] creates the entry if it doesn't exist
	auto& state = stateRegistry[name];

	// Ensure defaults if this is a fresh entry
	if (state.zoom < 1.0f) state.zoom = 1.0f;

	// 1. Initialize Sliders (Default 50 if empty)
	if (descriptor.filters().count(Video::Filter::Category::CORRECTION))
	{
		for (const auto& fname : descriptor.filters().at(Video::Filter::Category::CORRECTION))
		{
			if (state.filterSliderValues.find(fname) == state.filterSliderValues.end())
			{
				state.filterSliderValues[fname] = 50;
			}
		}
	}
}

void Application::ShowAdjustmentsDialog(wxCommandEvent& event)
{
    if (sourceKind != PrismForge::Input::Kind::Mobile) return;
	const auto& descriptors = rtspManager->GetDescriptors();
	if (descriptors.empty()) return;

	int currentDeviceId = rtspManager->GetStreamingDevice();
	if (currentDeviceId < 0 || currentDeviceId >= (int)descriptors.size())
		currentDeviceId = 0;

	const auto& desc = descriptors[currentDeviceId];

	EnsureStateInitialized(desc.name(), desc);
	auto& state = stateRegistry[desc.name()];

	ImgAdjDlg dialog(mainWindow, desc, state.filterSliderValues, state.activeEffectFilter);

	dialog.Bind(EVT_FILTER_PARAM_CHANGED, [&](const wxCommandEvent& event)
	{
		auto name = event.GetString().ToStdString();
		auto value = event.GetInt();

		rtspManager->ApplyCorrectionFilter(name, value);
		state.filterSliderValues[name] = value;
	});

	dialog.Bind(EVT_FILTER_SWITCH_CHANGED, [&](const wxCommandEvent& event)
	{
		auto name = event.GetString().ToStdString();
		// auto category = event.GetInt();

		rtspManager->ApplyEffectFilter(name);
		state.activeEffectFilter = name;
	});

	dialog.ShowModal();
}

void Application::ShowStreamConfigDialog(wxCommandEvent& event)
{
    if (sourceKind != PrismForge::Input::Kind::Mobile) return;
	const auto& descriptors = rtspManager->GetDescriptors();
	if (descriptors.empty())
		return;

	int deviceId = rtspManager->GetStreamingDevice();
	if (deviceId < 0 || deviceId >= (int)descriptors.size())
		deviceId = 0;

	const auto& desc = descriptors[deviceId];
	std::string deviceName = desc.name();

	EnsureStateInitialized(deviceName, desc);
	auto& state = stateRegistry[deviceName];

	StreamConfigDlg::Config config;

	config.resIndex = 0;
	const auto& resList = state.backCameraActive ? desc.backResolutions() : desc.frontResolutions();
	for (size_t i = 0; i < resList.size(); i++)
	{
		if (resList[i] == state.resolution)
		{
			config.resIndex = (int)i;
			break;
		}
	}

	config.fps = state.fps;
	config.adaptiveBitrate = state.adaptiveBitrate;
	config.bitrate = state.bitrate;
	config.minBitrate = state.minBitrate;
	config.maxBitrate = state.maxBitrate;
	config.stabilizationEnabled = state.stabilizationEnabled;
	config.flashEnabled = state.flashEnabled;
	config.focusMode = (state.focusMode == 1) ? 1 : 0;
	config.h265Enabled = state.h265Enabled;

	StreamConfigDlg dlg(mainWindow, desc, state.backCameraActive, config);

	dlg.Bind(EVT_STREAM_RESOLUTION_CHANGED, [this, deviceName](wxCommandEvent& e)
	{
		wxString resStr = e.GetString(); // "1920 x 1080" — note spaces around 'x'
		long w = 0, h = 0;
		// BeforeFirst/AfterFirst return the parts around the first 'x',
		// then Trim() removes any surrounding whitespace before parsing
		wxString wPart = resStr.BeforeFirst('x').Trim(true).Trim(false);
		wxString hPart = resStr.AfterFirst('x').Trim(true).Trim(false);
		if (wPart.ToLong(&w) && hPart.ToLong(&h) && w > 0 && h > 0)
		{
			rtspManager->SetResolution((uint16_t)w, (uint16_t)h);
			stateRegistry[deviceName].resolution = { (int)w, (int)h };
		}
	});

	dlg.Bind(EVT_STREAM_FPS_CHANGED, [this, deviceName](wxCommandEvent& e)
	{
		int fps = e.GetInt();
		rtspManager->SetFPS(fps);
		stateRegistry[deviceName].fps = fps;
	});

	dlg.Bind(EVT_STREAM_BITRATE_CHANGED, [this, deviceName, dlgPtr = &dlg](wxCommandEvent& e)
	{
		// Use pointer instead of reference to avoid dangling ref if dialog moves
		auto& regState = stateRegistry[deviceName];
		regState.adaptiveBitrate = dlgPtr->IsAdaptiveBitrate();
		regState.bitrate = dlgPtr->GetStaticBitrate();
		regState.minBitrate = dlgPtr->GetMinBitrate();
		regState.maxBitrate = dlgPtr->GetMaxBitrate();

		if (regState.adaptiveBitrate)
		{
			rtspManager->SetAdaptiveBitrate(regState.minBitrate, regState.maxBitrate);
		}
		else
		{
			rtspManager->SetBitrate(regState.bitrate);
		}
	});

	dlg.Bind(EVT_STREAM_CONFIG_CHANGED, [this, deviceName, dlgPtr = &dlg](wxCommandEvent& e)
	{
		auto& regState = stateRegistry[deviceName];

		if (regState.stabilizationEnabled != dlgPtr->IsStabilizationEnabled())
		{
			regState.stabilizationEnabled = dlgPtr->IsStabilizationEnabled();
			rtspManager->SetStabilization(regState.stabilizationEnabled);
		}

		if (regState.flashEnabled != dlgPtr->IsFlashEnabled())
		{
			regState.flashEnabled = dlgPtr->IsFlashEnabled();
			rtspManager->SetFlash(regState.flashEnabled);
		}

		if (regState.focusMode != dlgPtr->GetFocusMode())
		{
			regState.focusMode = dlgPtr->GetFocusMode();
			rtspManager->SetFocusMode(regState.focusMode);
		}

		if (regState.h265Enabled != dlgPtr->IsH265Enabled())
		{
			regState.h265Enabled = dlgPtr->IsH265Enabled();
			rtspManager->SetH265Codec(regState.h265Enabled);
		}
	});

	dlg.ShowModal();
    RefreshStudio();
}
int Application::OnExit()
{
    if (wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) logger << "[UI-VERIFY] OnExit begin" << std::endl;
    closing=true;
    segmentationTimer.Stop();
    localFaceFilterTimer.Stop();
    if(localFaceFilterResult)localFaceFilterResult->cancel.store(true,std::memory_order_release);
    if(localFaceFilterWorker.joinable())localFaceFilterWorker.join();
    localFaceFilterResult.reset();
    packRestoreTimer.Stop();
    if(packRestoreWorker.joinable())packRestoreWorker.join();
    packRestoreResult.reset();
    frameDelivery.Stop();
	VCamDeviceManager::Instance().Shutdown();
	const int baseResult = wxApp::OnExit();
    if (wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) logger << "[UI-VERIFY] OnExit complete" << std::endl;
    return uiVerificationExitCode != 0 ? uiVerificationExitCode : baseResult;
}

static std::string FavoriteKey(const std::string& name)
{
    const char* hex = "0123456789abcdef"; std::string key = "PIN_";
    for (unsigned char c : name) { key += hex[c >> 4]; key += hex[c & 15]; }
    return key;
}
void Application::PublishDesktopLook()
{
    effectsPipeline.Configure({desktopLook.selected, desktopLook.bypassed, desktopLut, desktopLutIntensity / 100.0f});
    if (mainWindow) mainWindow->SetLutPresentation(desktopLutName, desktopLutIntensity);
    Settings::Set("DESKTOP_LOOK", static_cast<int>(desktopLook.selected));
    if (mainWindow) {
        mainWindow->SetBackgroundPresentation(static_cast<int>(desktopStudio.background),desktopStudio.blurStrength,backgroundImageName,
            wxColour(desktopStudio.backgroundR,desktopStudio.backgroundG,desktopStudio.backgroundB),desktopLook.bypassed,desktopStudio.highQualityMask);
        mainWindow->SetEffectsPresetPresentation(activeEffectsPreset);
        RefreshSegmentationStatus();
        mainWindow->SetLookPresentation(static_cast<int>(desktopLook.selected), desktopLook.bypassed);
        mainWindow->SetStudioPresentation(static_cast<int>(desktopStudio.light),static_cast<int>(desktopStudio.direction),
            static_cast<int>(std::lround(desktopStudio.strength*100)),desktopStudio.studioFrame,desktopLook.bypassed);
        mainWindow->SetCustomLightingPresentation(desktopStudio.keyLight,desktopStudio.fillLight,
            desktopStudio.lightingSubjectOnly,desktopLook.bypassed,desktopStudio.faceLighting);
        mainWindow->SetTouchupPresentation(desktopStudio.touchupStrength,desktopStudio.touchupDetail,desktopLook.bypassed);
        mainWindow->SetAccessoryPresentation(desktopStudio.glasses,desktopStudio.cap,desktopStudio.accessoryScale,desktopLook.bypassed,
            desktopStudio.capStyle,desktopStudio.glassesStyle,desktopStudio.capScale,desktopStudio.capVerticalOffset);
        mainWindow->SetFaceFilterPresentation(desktopStudio.faceFilterStyle,desktopStudio.faceFilterScale,desktopStudio.faceFilterVerticalOffset,desktopLook.bypassed);
        RefreshLocalFaceFilterPresentation();
    }
}

void Application::ConfigureDesktopStudio()
{
    using namespace PrismForge::Effects::Overlays::Realistic;
    modelBudget=ResolveModelResourceBudget(modelPerformance,modelHardware);
    desktopStudio.faceFilterRenderEdge=modelBudget.renderEdge;
    const auto& model=desktopStudio.faceFilterCustomModel;
    localModelBudgetHeld=desktopStudio.faceFilterStyle==4&&model&&
        (model->vertices.size()>modelBudget.vertices||model->triangles.size()>modelBudget.triangles);
    auto effective=desktopStudio;
    // Hold the immutable model and fit while a smaller geometry policy is selected.
    if(localModelBudgetHeld){effective.faceFilterStyle=0;effective.glasses=false;effective.cap=false;}
    effectsPipeline.ConfigureStudio(effective);
}
wxString Application::ModelPerformanceSummary() const
{
    using namespace PrismForge::Effects::Overlays::Realistic;
    ModelPerformanceSettings automatic;
    const auto recommended=ResolveModelResourceBudget(automatic,modelHardware).resolvedMode;
    wxString summary=wxString::Format("%s: %u nodes, %u vertices, %u triangles; %u px. ",
        wxString::FromUTF8(ModelPerformanceModeName(modelBudget.resolvedMode)),modelBudget.nodes,modelBudget.vertices,modelBudget.triangles,modelBudget.renderEdge);
    summary+=wxString::Format("RAM %.0f GB, GPU %.1f GB. Auto estimate: %s.",
        modelHardware.ramBytes/double(1ull<<30),modelHardware.dedicatedVideoBytes/double(1ull<<30),wxString::FromUTF8(ModelPerformanceModeName(recommended)));
    if(localModelBudgetHeld)summary="Paused: model exceeds geometry limit. Increase the budget. "+summary;
    return summary;
}
void Application::PublishDesktopStudio()
{
    ConfigureDesktopStudio();
    Settings::Set("STUDIO_LIGHT_MODE",static_cast<int>(desktopStudio.light));
    Settings::Set("STUDIO_LIGHT_DIRECTION",static_cast<int>(desktopStudio.direction));
    Settings::Set("STUDIO_LIGHT_STRENGTH",static_cast<int>(std::lround(desktopStudio.strength*100)));
    Settings::Set("STUDIO_LIGHT_SUBJECT_ONLY",desktopStudio.lightingSubjectOnly?1:0);
    Settings::Set("STUDIO_FACE_LIGHTING",desktopStudio.faceLighting?1:0);
    Settings::Set("STUDIO_GLASSES",desktopStudio.glasses?1:0);
    Settings::Set("STUDIO_CAP",desktopStudio.cap?1:0);
    Settings::Set("STUDIO_CAP_STYLE",desktopStudio.capStyle);
    Settings::Set("STUDIO_GLASSES_STYLE",desktopStudio.glassesStyle);
    Settings::Set("STUDIO_ACCESSORY_SCALE",int(std::lround(desktopStudio.accessoryScale*100)));
    Settings::Set("STUDIO_CAP_SCALE",int(std::lround(desktopStudio.capScale*100)));
    Settings::Set("STUDIO_CAP_HEIGHT",int(std::lround((desktopStudio.capVerticalOffset+2.f)*10)));
    Settings::Set("STUDIO_MODEL_MODE",int(modelPerformance.mode));
    Settings::Set("STUDIO_MODEL_NODES",int(modelPerformance.customNodes));
    Settings::Set("STUDIO_MODEL_VERTICES",int(modelPerformance.customVertices));
    Settings::Set("STUDIO_MODEL_TRIANGLES",int(modelPerformance.customTriangles));
    Settings::Set("STUDIO_MODEL_RENDER_EDGE",int(modelPerformance.customRenderEdge));
    Settings::Set("STUDIO_FACE_FILTER",desktopStudio.faceFilterStyle);
    Settings::Set(desktopStudio.faceFilterStyle==5?"STUDIO_ROYAL_CROWN_SCALE":"STUDIO_FACE_FILTER_SCALE",int(std::lround(desktopStudio.faceFilterScale*100)));
    Settings::Set(desktopStudio.faceFilterStyle==5?"STUDIO_ROYAL_CROWN_HEIGHT":"STUDIO_FACE_FILTER_HEIGHT",int(std::lround((desktopStudio.faceFilterVerticalOffset+3.f)*10)));
    const auto& localFit=desktopStudio.faceFilterCustomFit;
    Settings::Set("STUDIO_LOCAL_FILTER_X",int(std::lround((localFit.xCm+20.f)*10)));
    Settings::Set("STUDIO_LOCAL_FILTER_Y",int(std::lround((localFit.yCm+20.f)*10)));
    Settings::Set("STUDIO_LOCAL_FILTER_Z",int(std::lround((localFit.zCm+20.f)*10)));
    Settings::Set("STUDIO_LOCAL_FILTER_TILT",int(std::lround(localFit.tiltDegrees+180.f)));
    Settings::Set("STUDIO_LOCAL_FILTER_TURN",int(std::lround(localFit.turnDegrees+180.f)));
    Settings::Set("STUDIO_LOCAL_FILTER_ROLL",int(std::lround(localFit.rollDegrees+180.f)));
    Settings::Set("STUDIO_LOCAL_FILTER_OCCLUSION",localFit.headOcclusion?1:0);
    Settings::Set("STUDIO_TOUCHUP_STRENGTH",int(std::lround(desktopStudio.touchupStrength*100)));
    Settings::Set("STUDIO_TOUCHUP_DETAIL",int(std::lround(desktopStudio.touchupDetail*100)));
    auto saveLight=[](const PrismForge::Effects::Lighting::StudioLight& light,const std::string& prefix){
        Settings::Set(prefix+"_X",int(std::lround((light.posX+1)*1000)));
        Settings::Set(prefix+"_Y",int(std::lround((light.posY+1)*1000)));
        Settings::Set(prefix+"_INTENSITY",int(std::lround(light.intensity*100)));
        Settings::Set(prefix+"_TEMPERATURE",int(std::lround(light.colorTemperature)));
    };
    saveLight(desktopStudio.keyLight,"STUDIO_KEY");saveLight(desktopStudio.fillLight,"STUDIO_FILL");
    Settings::Set("STUDIO_FRAME",desktopStudio.studioFrame?1:0);
    Settings::Set("STUDIO_BACKGROUND_MODE",static_cast<int>(desktopStudio.background));
    Settings::Set("STUDIO_BACKGROUND_BLUR",desktopStudio.blurStrength);
    Settings::Set("STUDIO_BACKGROUND_QUALITY",desktopStudio.highQualityMask?1:0);
    Settings::Set("STUDIO_BACKGROUND_COLOUR",(int(desktopStudio.backgroundR)<<16)|(int(desktopStudio.backgroundG)<<8)|desktopStudio.backgroundB);
    if(mainWindow){mainWindow->SetBackgroundPresentation(static_cast<int>(desktopStudio.background),desktopStudio.blurStrength,backgroundImageName,
        wxColour(desktopStudio.backgroundR,desktopStudio.backgroundG,desktopStudio.backgroundB),desktopLook.bypassed,desktopStudio.highQualityMask);
        mainWindow->SetEffectsPresetPresentation(activeEffectsPreset);RefreshSegmentationStatus();}
    if (mainWindow) {
        mainWindow->SetStudioPresentation(static_cast<int>(desktopStudio.light),static_cast<int>(desktopStudio.direction),
            static_cast<int>(std::lround(desktopStudio.strength*100)),desktopStudio.studioFrame,desktopLook.bypassed);
        mainWindow->SetCustomLightingPresentation(desktopStudio.keyLight,desktopStudio.fillLight,
            desktopStudio.lightingSubjectOnly,desktopLook.bypassed,desktopStudio.faceLighting);
        mainWindow->SetTouchupPresentation(desktopStudio.touchupStrength,desktopStudio.touchupDetail,desktopLook.bypassed);
        mainWindow->SetAccessoryPresentation(desktopStudio.glasses,desktopStudio.cap,desktopStudio.accessoryScale,desktopLook.bypassed,
            desktopStudio.capStyle,desktopStudio.glassesStyle,desktopStudio.capScale,desktopStudio.capVerticalOffset);
        mainWindow->SetFaceFilterPresentation(desktopStudio.faceFilterStyle,desktopStudio.faceFilterScale,desktopStudio.faceFilterVerticalOffset,desktopLook.bypassed);
        RefreshLocalFaceFilterPresentation();
    }
}
bool Application::HandleDesktopStudioAction(wxCommandEvent& event)
{
    const int id=event.GetId();
    if (id<Window::STUDIO_LIGHT_MODE || id>Window::STUDIO_FRAME_CHANGED) return false;
    if (closing || !mainWindow || desktopLook.bypassed) return true;
    desktopStudio.light=static_cast<PrismForge::Effects::DesktopLightMode>(std::clamp(mainWindow->GetStudioLightChoice()->GetSelection(),0,4));
    desktopStudio.direction=static_cast<PrismForge::Effects::DesktopLightDirection>(std::clamp(mainWindow->GetStudioLightDirectionChoice()->GetSelection(),0,2));
    desktopStudio.strength=std::clamp(mainWindow->GetStudioLightStrengthSlider()->GetValue(),0,100)/100.f;
    const int lightingArea=std::clamp(mainWindow->GetStudioLightScopeChoice()->GetSelection(),0,2);
    desktopStudio.lightingSubjectOnly=lightingArea!=0;desktopStudio.faceLighting=lightingArea==2;
    const auto positions=mainWindow->GetStudioLightPositionPad()->GetPositions();
    desktopStudio.keyLight.posX=positions[0];desktopStudio.keyLight.posY=positions[1];
    desktopStudio.fillLight.posX=positions[2];desktopStudio.fillLight.posY=positions[3];
    desktopStudio.keyLight.intensity=std::clamp(mainWindow->GetStudioKeyIntensitySlider()->GetValue(),0,200)/100.f;
    desktopStudio.fillLight.intensity=std::clamp(mainWindow->GetStudioFillIntensitySlider()->GetValue(),0,200)/100.f;
    desktopStudio.keyLight.colorTemperature=float(std::clamp(mainWindow->GetStudioKeyTemperatureSlider()->GetValue(),1500,15000));
    desktopStudio.fillLight.colorTemperature=float(std::clamp(mainWindow->GetStudioFillTemperatureSlider()->GetValue(),1500,15000));
    desktopStudio.studioFrame=mainWindow->GetStudioFrameChoice()->GetSelection()==1;
    activeEffectsPreset=0;
    if(id==Window::STUDIO_LIGHT_POSITION){
        // Dragging changes processing state only, never panel geometry or unrelated controls.
        ConfigureDesktopStudio();
        Settings::Set("STUDIO_KEY_X",int(std::lround((desktopStudio.keyLight.posX+1)*1000)));
        Settings::Set("STUDIO_KEY_Y",int(std::lround((desktopStudio.keyLight.posY+1)*1000)));
        Settings::Set("STUDIO_FILL_X",int(std::lround((desktopStudio.fillLight.posX+1)*1000)));
        Settings::Set("STUDIO_FILL_Y",int(std::lround((desktopStudio.fillLight.posY+1)*1000)));
        mainWindow->SetEffectsPresetPresentation(activeEffectsPreset);
    }else PublishDesktopStudio();
    return true;
}
bool Application::HandleTouchupAction(wxCommandEvent& event)
{
    const int id=event.GetId();if(id!=Window::TOUCHUP_STRENGTH&&id!=Window::TOUCHUP_DETAIL)return false;
    if(closing||!mainWindow||desktopLook.bypassed)return true;
    desktopStudio.touchupStrength=std::clamp(mainWindow->GetTouchupStrengthSlider()->GetValue(),0,100)/100.f;
    desktopStudio.touchupDetail=std::clamp(mainWindow->GetTouchupDetailSlider()->GetValue(),0,100)/100.f;
    activeEffectsPreset=0;ConfigureDesktopStudio();
    Settings::Set("STUDIO_TOUCHUP_STRENGTH",int(std::lround(desktopStudio.touchupStrength*100)));
    Settings::Set("STUDIO_TOUCHUP_DETAIL",int(std::lround(desktopStudio.touchupDetail*100)));
    mainWindow->SetTouchupPresentation(desktopStudio.touchupStrength,desktopStudio.touchupDetail,false);
    mainWindow->SetEffectsPresetPresentation(activeEffectsPreset);
    // Only the changed controls repaint. The existing one-second timer updates face status.
    return true;
}
bool Application::HandleAccessoryAction(wxCommandEvent& event)
{
    const int id=event.GetId();
    if(id<Window::ACCESSORY_GLASSES||id>Window::ACCESSORY_CAP_HEIGHT)return false;
    if(closing||!mainWindow||desktopLook.bypassed)return true;
    const int glassesSelection=std::clamp(mainWindow->GetAccessoryGlassesChoice()->GetSelection(),0,4);
    desktopStudio.glasses=glassesSelection>0;if(glassesSelection>0)desktopStudio.glassesStyle=glassesSelection-1;
    desktopStudio.cap=mainWindow->GetAccessoryCapChoice()->GetSelection()==1;desktopStudio.capStyle=2;
    desktopStudio.accessoryScale=std::clamp(mainWindow->GetAccessoryScaleSlider()->GetValue(),50,150)/100.f;
    desktopStudio.capScale=std::clamp(mainWindow->GetCapScaleSlider()->GetValue(),75,150)/100.f;
    desktopStudio.capVerticalOffset=std::clamp(mainWindow->GetCapHeightSlider()->GetValue(),0,50)/10.f-2.f;
    activeEffectsPreset=0;ConfigureDesktopStudio();
    Settings::Set("STUDIO_GLASSES",desktopStudio.glasses?1:0);
    Settings::Set("STUDIO_CAP",desktopStudio.cap?1:0);
    Settings::Set("STUDIO_CAP_STYLE",desktopStudio.capStyle);
    Settings::Set("STUDIO_GLASSES_STYLE",desktopStudio.glassesStyle);
    Settings::Set("STUDIO_ACCESSORY_SCALE",int(std::lround(desktopStudio.accessoryScale*100)));
    Settings::Set("STUDIO_CAP_SCALE",int(std::lround(desktopStudio.capScale*100)));
    Settings::Set("STUDIO_CAP_HEIGHT",int(std::lround((desktopStudio.capVerticalOffset+2.f)*10)));
    mainWindow->SetAccessoryPresentation(desktopStudio.glasses,desktopStudio.cap,desktopStudio.accessoryScale,false,
        desktopStudio.capStyle,desktopStudio.glassesStyle,desktopStudio.capScale,desktopStudio.capVerticalOffset);
    mainWindow->SetEffectsPresetPresentation(activeEffectsPreset);
    // Repaint the changed controls; the existing timer updates face status without layout.
    return true;
}


void Application::RefreshLocalFaceFilterPresentation()
{
    if(mainWindow){
        mainWindow->SetLocalFaceFilterPresentation(wxString::FromUTF8(desktopStudio.faceFilterCustomName),desktopStudio.faceFilterCustomFit,
            bool(desktopStudio.faceFilterCustomModel),bool(localFaceFilterResult),desktopLook.bypassed);
        mainWindow->SetModelPerformancePresentation(modelPerformance,ModelPerformanceSummary(),bool(localFaceFilterResult),desktopLook.bypassed);
    }
}
bool Application::HandleFaceFilterAction(wxCommandEvent& event)
{
    const int id=event.GetId();if(id<Window::FACE_FILTER_STYLE||id>Window::FACE_FILTER_PERFORMANCE)return false;
    if(closing||!mainWindow)return true;
    if(id==Window::FACE_FILTER_PERFORMANCE){
        if(localFaceFilterResult){RefreshLocalFaceFilterPresentation();return true;}
        const auto requested=mainWindow->GetModelPerformanceSettings();std::string error;
        if(!PrismForge::Effects::Overlays::Realistic::ValidateModelPerformanceSettings(requested,error)){mainWindow->Notify(wxString::FromUTF8(error));RefreshLocalFaceFilterPresentation();return true;}
        modelPerformance=requested;PublishDesktopStudio();RefreshSegmentationStatus();return true;
    }
    if(desktopLook.bypassed)return true;
    if(id==Window::FACE_FILTER_IMPORT){
        if(localFaceFilterResult)return true;
        if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)){mainWindow->Notify("Local model file operations are disabled in UI preview mode.");return true;}
        wxFileDialog file(mainWindow,"Import a local 3D face model",wxEmptyString,wxEmptyString,"Embedded GLB models (*.glb)|*.glb",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(file.ShowModal()==wxID_OK)StartLocalFaceFilterOperation(file.GetPath().ToStdWstring());return true;
    }
    if(id==Window::FACE_FILTER_REMOVE){
        if(!localFaceFilterResult&&!desktopStudio.faceFilterCustomPath.empty())StartLocalFaceFilterOperation(wxString::FromUTF8(desktopStudio.faceFilterCustomPath).ToStdWstring(),false,true);return true;
    }
    if(localFaceFilterResult)return true;
    const int requested=std::clamp(mainWindow->GetFaceFilterChoice()->GetSelection(),0,5);
    if(requested==4&&!desktopStudio.faceFilterCustomModel){mainWindow->Notify("Import a local .glb model first.");PublishDesktopStudio();return true;}
    // Crown fit is an adjustment to its baked default; the other models keep their saved fit.
    const bool changedFitGroup=requested!=desktopStudio.faceFilterStyle&&(requested==5||desktopStudio.faceFilterStyle==5);
    ++localFaceFilterEpoch;desktopStudio.faceFilterStyle=requested;
    desktopStudio.faceFilterScale=changedFitGroup?LoadFaceFilterScale(requested):std::clamp(mainWindow->GetFaceFilterScaleSlider()->GetValue(),75,150)/100.f;
    desktopStudio.faceFilterVerticalOffset=changedFitGroup?LoadFaceFilterHeight(requested):std::clamp(mainWindow->GetFaceFilterHeightSlider()->GetValue(),0,60)/10.f-3.f;
    if(id==Window::FACE_FILTER_CUSTOM_RESET){desktopStudio.faceFilterCustomFit={};desktopStudio.faceFilterScale=1.f;desktopStudio.faceFilterVerticalOffset=0.f;}
    else if(id==Window::FACE_FILTER_CUSTOM_FIT)desktopStudio.faceFilterCustomFit=mainWindow->GetLocalFaceFilterFit();
    activeEffectsPreset=0;PublishDesktopStudio();RefreshSegmentationStatus();return true;
}
void Application::SaveLocalFaceFilterIdentity()
{
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr))return;
    const auto directory=LocalFaceFilterStorage();
    if(!wxFileName::DirExists(directory)&&!wxFileName::Mkdir(directory,0755,wxPATH_MKDIR_FULL)){if(mainWindow)mainWindow->Notify("Local model is active, but its selection could not be saved.");return;}
    wxMemoryInputStream empty("",0);wxFileConfig config(empty);
    config.Write("kind","PrismForgeLocalFaceFilter1");config.Write("path",wxString::FromUTF8(desktopStudio.faceFilterCustomPath));config.Write("name",wxString::FromUTF8(desktopStudio.faceFilterCustomName));
    wxTempFileOutputStream output(LocalFaceFilterSelectionFile());
    if((!output.IsOk()||!config.Save(output)||!output.Commit())&&mainWindow)mainWindow->Notify("Local model is active, but its selection could not be saved.");
}
void Application::RestoreLocalFaceFilter()
{
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)||closing)return;
    if(!desktopStudio.faceFilterCustomPath.empty())StartLocalFaceFilterOperation(wxString::FromUTF8(desktopStudio.faceFilterCustomPath).ToStdWstring(),true);
    else if(desktopStudio.faceFilterStyle==4){desktopStudio.faceFilterStyle=0;PublishDesktopStudio();mainWindow->Notify("Saved local model unavailable. Import its .glb again.");}
}
void Application::StartLocalFaceFilterOperation(const std::wstring& path,bool restore,bool remove)
{
    if(closing||localFaceFilterResult)return;
    auto result=std::make_shared<LocalFaceFilterResult>();result->restore=restore;result->remove=remove;result->epoch=++localFaceFilterEpoch;
    localFaceFilterResult=result;const auto library=LocalFaceFilterLibrary();const auto budget=modelBudget;
    try{
        localFaceFilterWorker=std::thread([result,path,library,budget]{
            using namespace PrismForge::Effects::Overlays::Realistic;
            try{
                if(result->cancel.load(std::memory_order_acquire)){result->error="Operation cancelled";}
                else if(result->remove)result->success=RemoveOwnedLocalFaceFilter(path,library,result->error);
                else if(result->restore){
                    if(IsOwnedLocalFaceFilterPath(path,library,result->error)){
                        result->value.nativePath=path;result->value.model=LoadFaceFilterFile(path,result->error);result->success=bool(result->value.model);
                    }
                }else result->success=ImportLocalFaceFilter(path,library,result->value,result->error,budget,&result->cancel);
            }catch(const std::exception& e){result->error=e.what();}catch(...){result->error="Unexpected local model failure";}
            result->done.store(true,std::memory_order_release);
        });
        if(!localFaceFilterTimerBound){localFaceFilterTimer.SetOwner(this);Bind(wxEVT_TIMER,[this](wxTimerEvent&){PollLocalFaceFilter();},localFaceFilterTimer.GetId());localFaceFilterTimerBound=true;}
        if(!localFaceFilterTimer.Start(50)){result->cancel.store(true,std::memory_order_release);if(localFaceFilterWorker.joinable())localFaceFilterWorker.join();PollLocalFaceFilter();}
        RefreshLocalFaceFilterPresentation();
    }catch(const std::exception& e){if(localFaceFilterWorker.joinable())localFaceFilterWorker.join();localFaceFilterResult.reset();if(mainWindow)mainWindow->Notify("Local model could not start: "+wxString::FromUTF8(e.what()));RefreshLocalFaceFilterPresentation();}
}
void Application::PollLocalFaceFilter()
{
    if(!localFaceFilterResult||!localFaceFilterResult->done.load(std::memory_order_acquire))return;
    localFaceFilterTimer.Stop();if(localFaceFilterWorker.joinable())localFaceFilterWorker.join();auto result=std::move(localFaceFilterResult);
    if(closing||!mainWindow||result->epoch!=localFaceFilterEpoch||result->cancel.load(std::memory_order_acquire))return;
    if(result->success){
        if(result->remove){desktopStudio.faceFilterCustomModel.reset();desktopStudio.faceFilterCustomPath.clear();desktopStudio.faceFilterCustomName.clear();desktopStudio.faceFilterCustomFit={};if(desktopStudio.faceFilterStyle==4)desktopStudio.faceFilterStyle=0;}
        else{
            desktopStudio.faceFilterCustomModel=result->value.model;
            if(!result->restore){desktopStudio.faceFilterCustomPath=std::filesystem::path(result->value.nativePath).u8string();desktopStudio.faceFilterCustomName=result->value.displayName;desktopStudio.faceFilterCustomFit={};desktopStudio.faceFilterScale=1.f;desktopStudio.faceFilterVerticalOffset=0.f;desktopStudio.faceFilterStyle=4;activeEffectsPreset=0;}
        }
        SaveLocalFaceFilterIdentity();PublishDesktopStudio();
        if(!result->restore){wxString notice=result->remove?wxString("Local model removed. Your original GLB is unchanged."):wxString("Local model imported. Adjust its size, position and rotation to fit your face.");
            if(!result->remove&&!result->value.notice.empty())notice+=" "+wxString::FromUTF8(result->value.notice).Left(220);mainWindow->Notify(notice);}
    }else{
        if(result->restore&&desktopStudio.faceFilterStyle==4&&!desktopStudio.faceFilterCustomModel){desktopStudio.faceFilterStyle=0;PublishDesktopStudio();}
        mainWindow->Notify((result->restore?wxString("Saved local model unavailable. "):wxString("Local model was not changed. "))+wxString::FromUTF8(result->error).Left(220));
    }
    RefreshLocalFaceFilterPresentation();RefreshSegmentationStatus();
}
bool Application::VerifyLocalFaceFilterWorker()
{
    wxString fixture,library;
    if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)||!wxGetEnv("PRISMFORGE_UI_VERIFY_LOCAL_MODEL",&fixture))return true;
    if(!wxGetEnv("PRISMFORGE_UI_LOCAL_FILTER_LIBRARY",&library)||library.empty()||!std::filesystem::path(library.ToStdWstring()).is_absolute())return false;
    bool isolatedBuild=false;for(const auto& part:std::filesystem::path(library.ToStdWstring()))if(wxString(part.wstring()).CmpNoCase("build")==0)isolatedBuild=true;
    if(!isolatedBuild||!wxFileExists(fixture))return false;
    const auto held=desktopStudio;const int heldPreset=activeEffectsPreset;bool passed=true;
    auto check=[&](bool ok,const char* why){if(!ok){passed=false;logger<<"[UI-VERIFY] Local model worker failure: "<<why<<std::endl;}};
    auto wait=[&]{const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(localFaceFilterResult&&!localFaceFilterResult->done.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<limit)wxMilliSleep(5);
        if(localFaceFilterResult&&!localFaceFilterResult->done.load(std::memory_order_acquire)){localFaceFilterResult->cancel.store(true,std::memory_order_release);check(false,"bounded worker completed before deadline");}
        if(localFaceFilterWorker.joinable())localFaceFilterWorker.join();PollLocalFaceFilter();};
    const auto before=desktopStudio.faceFilterCustomModel;StartLocalFaceFilterOperation(fixture.ToStdWstring());
    check(localFaceFilterResult&&desktopStudio.faceFilterCustomModel==before,"starting import retains current immutable native snapshot");wait();
    check(desktopStudio.faceFilterStyle==4&&desktopStudio.faceFilterCustomModel&&!desktopStudio.faceFilterCustomPath.empty(),"actual GLB worker publishes native snapshot and local selection");
    const auto imported=desktopStudio.faceFilterCustomModel;const auto owned=desktopStudio.faceFilterCustomPath;
    StartLocalFaceFilterOperation((std::filesystem::path(library.ToStdWstring())/L"missing.glb").wstring());wait();
    check(desktopStudio.faceFilterCustomModel==imported&&desktopStudio.faceFilterCustomPath==owned&&desktopStudio.faceFilterStyle==4,"failed replacement preserves last successful selected model");
    desktopStudio.faceFilterCustomFit.xCm=3.2f;desktopStudio.faceFilterCustomFit.turnDegrees=-37;desktopStudio.faceFilterCustomFit.headOcclusion=false;
    desktopStudio.faceFilterCustomModel.reset();StartLocalFaceFilterOperation(wxString::FromUTF8(owned).ToStdWstring(),true);wait();
    check(desktopStudio.faceFilterCustomModel&&desktopStudio.faceFilterCustomFit.xCm==3.2f&&desktopStudio.faceFilterCustomFit.turnDegrees==-37&&!desktopStudio.faceFilterCustomFit.headOcclusion,"owned PFM restores immutable model and held fitting without reimport");
    const auto restored=desktopStudio.faceFilterCustomModel;StartLocalFaceFilterOperation(fixture.ToStdWstring());if(localFaceFilterResult)localFaceFilterResult->cancel.store(true,std::memory_order_release);wait();
    check(desktopStudio.faceFilterCustomModel==restored&&desktopStudio.faceFilterCustomFit.xCm==3.2f,"cancelled worker cannot publish late replacement");
    StartLocalFaceFilterOperation(wxString::FromUTF8(owned).ToStdWstring(),false,true);wait();
    check(desktopStudio.faceFilterStyle==0&&!desktopStudio.faceFilterCustomModel&&desktopStudio.faceFilterCustomPath.empty()&&!wxFileExists(wxString::FromUTF8(owned))&&wxFileExists(fixture),"owned removal clears active local model and retains original GLB");
    check(!localFaceFilterWorker.joinable()&&!localFaceFilterResult,"all owned workers joined and results released");
    desktopStudio=held;activeEffectsPreset=heldPreset;PublishDesktopStudio();
    logger<<(passed?"[UI-VERIFY] Local model real-worker import/restore/failure/cancel/remove PASS":"[UI-VERIFY] Local model real-worker FAIL")<<std::endl;return passed;
}
bool Application::VerifyFaceFilterSettings()
{
    if(!mainWindow)return false;
    const auto held=desktopStudio;const auto heldLook=desktopLook;const int heldPreset=activeEffectsPreset;
    const auto heldPerformance=modelPerformance;
    const int heldGeneralScale=Settings::Get("STUDIO_FACE_FILTER_SCALE"),heldGeneralHeight=Settings::Get("STUDIO_FACE_FILTER_HEIGHT"),heldCrownScale=Settings::Get("STUDIO_ROYAL_CROWN_SCALE"),heldCrownHeight=Settings::Get("STUDIO_ROYAL_CROWN_HEIGHT");
    bool passed=true;auto check=[&](bool ok,const char* why){if(!ok){passed=false;logger<<"[UI-VERIFY] 3D face filters settings failure: "<<why<<std::endl;}};
    auto close=[](float a,float b){return std::abs(a-b)<.00001f;};
    for(int value:{-1,6}){Settings::Set("STUDIO_FACE_FILTER",value);check(LoadFaceFilterStyle()==0,"invalid saved model defaults Off");}
    for(int value:{-1,74,151}){Settings::Set("STUDIO_FACE_FILTER_SCALE",value);check(LoadFaceFilterScale()==1.f,"invalid saved size defaults 100%");}
    for(int value:{-1,61}){Settings::Set("STUDIO_FACE_FILTER_HEIGHT",value);check(LoadFaceFilterHeight()==0.f,"invalid saved height defaults zero");}
    for(int value:{-1,74,151}){Settings::Set("STUDIO_ROYAL_CROWN_SCALE",value);check(LoadFaceFilterScale(5)==1.f,"missing or invalid crown size defaults to baked100percent");}
    for(int value:{-1,61}){Settings::Set("STUDIO_ROYAL_CROWN_HEIGHT",value);check(LoadFaceFilterHeight(5)==0.f,"missing or invalid crown height defaults to bakedzero");}
    desktopLook.bypassed=false;desktopStudio.faceFilterStyle=0;desktopStudio.glasses=true;desktopStudio.glassesStyle=3;desktopStudio.cap=true;desktopStudio.capStyle=2;
    desktopStudio.accessoryScale=1.37f;desktopStudio.capScale=1.23f;desktopStudio.capVerticalOffset=-.7f;PublishDesktopStudio();PublishDesktopLook();
    auto* choice=mainWindow->GetFaceFilterChoice();auto* size=mainWindow->GetFaceFilterScaleSlider();auto* height=mainWindow->GetFaceFilterHeightSlider();
    for(int style:{0,1,2,3}){
        choice->SetSelection(style);size->SetValue(123);height->SetValue(23);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);
        const auto state=effectsPipeline.GetStudioSettings();PrismForge::Effects::DesktopStudioSettings restored;LoadAccessorySettings(restored);
        check(state.faceFilterStyle==style&&restored.faceFilterStyle==style&&Settings::Get("STUDIO_FACE_FILTER")==style&&close(state.faceFilterScale,1.23f)&&close(state.faceFilterVerticalOffset,-.7f)&&close(restored.faceFilterScale,state.faceFilterScale)&&close(restored.faceFilterVerticalOffset,state.faceFilterVerticalOffset),"all model choices and fit survive restart");
        check(state.glasses&&state.cap&&state.glassesStyle==3&&close(state.accessoryScale,1.37f)&&close(state.capScale,1.23f)&&close(state.capVerticalOffset,-.7f),"filter selection preserves saved accessories");
    }
    for(int value:{75,100,150}){size->SetValue(value);mainWindow->Dispatch(Window::FACE_FILTER_SCALE);check(close(effectsPipeline.GetStudioSettings().faceFilterScale,value/100.f)&&Settings::Get("STUDIO_FACE_FILTER_SCALE")==value,"size dispatcher reaches processing");}
    for(int value:{0,30,60}){height->SetValue(value);mainWindow->Dispatch(Window::FACE_FILTER_HEIGHT);check(close(effectsPipeline.GetStudioSettings().faceFilterVerticalOffset,value/10.f-3.f)&&Settings::Get("STUDIO_FACE_FILTER_HEIGHT")==value,"height dispatcher reaches processing");}
    choice->SetSelection(2);size->SetValue(123);height->SetValue(23);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);
    mainWindow->Dispatch(Window::LOOK_BYPASS);choice->SetSelection(1);size->SetValue(99);height->SetValue(59);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);
    auto state=effectsPipeline.GetStudioSettings();check(desktopLook.bypassed&&state.faceFilterStyle==2&&close(state.faceFilterScale,1.23f)&&close(state.faceFilterVerticalOffset,-.7f)&&Settings::Get("STUDIO_FACE_FILTER")==2,"bypassed events retain the selected filter");
    mainWindow->Dispatch(Window::LOOK_BYPASS);check(!desktopLook.bypassed&&choice->GetSelection()==2&&size->GetValue()==123&&height->GetValue()==23,"restore retrieves held model and fit");
    mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==0&&close(state.faceFilterScale,1.23f)&&close(state.faceFilterVerticalOffset,-.7f)&&Settings::Get("STUDIO_FACE_FILTER")==0,"Original disables the model but keeps fit");
    auto invalid=state;invalid.faceFilterStyle=6;invalid.faceFilterScale=std::nanf("");invalid.faceFilterVerticalOffset=std::nanf("");effectsPipeline.ConfigureStudio(invalid);const auto bounded=effectsPipeline.GetStudioSettings();
    check(bounded.faceFilterStyle==0&&bounded.faceFilterScale==1.f&&bounded.faceFilterVerticalOffset==0.f,"processing configuration guards malformed values");
    desktopLook.bypassed=false;desktopStudio.faceFilterCustomModel=std::make_shared<PrismForge::Effects::Overlays::Realistic::NativeFaceFilter>();
    desktopStudio.faceFilterCustomPath=u8"C:/owned-library/模型.pfmodel";desktopStudio.faceFilterCustomName=u8"Local 模型";
    desktopStudio.faceFilterStyle=4;desktopStudio.faceFilterCustomFit.xCm=4.5f;desktopStudio.faceFilterCustomFit.yCm=-7.8f;desktopStudio.faceFilterCustomFit.zCm=12.3f;
    desktopStudio.faceFilterCustomFit.tiltDegrees=32;desktopStudio.faceFilterCustomFit.turnDegrees=-71;desktopStudio.faceFilterCustomFit.rollDegrees=17;desktopStudio.faceFilterCustomFit.headOcclusion=false;PublishDesktopStudio();
    mainWindow->Dispatch(Window::FACE_FILTER_CUSTOM_FIT);state=effectsPipeline.GetStudioSettings();PrismForge::Effects::DesktopStudioSettings saved;LoadAccessorySettings(saved);
    check(state.faceFilterStyle==4&&state.faceFilterCustomModel==desktopStudio.faceFilterCustomModel&&close(state.faceFilterCustomFit.xCm,4.5f)&&close(state.faceFilterCustomFit.yCm,-7.8f)&&close(state.faceFilterCustomFit.zCm,12.3f)&&state.faceFilterCustomFit.turnDegrees==-71&&!state.faceFilterCustomFit.headOcclusion,"local immutable snapshot and fit dispatcher reach native processing");
    check(close(saved.faceFilterCustomFit.zCm,12.3f)&&saved.faceFilterCustomFit.turnDegrees==-71&&!saved.faceFilterCustomFit.headOcclusion,"local fitting survives saved setting reload");
    mainWindow->Dispatch(Window::FACE_FILTER_CUSTOM_RESET);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==4&&state.faceFilterScale==1&&state.faceFilterVerticalOffset==0&&state.faceFilterCustomFit.xCm==0&&state.faceFilterCustomFit.zCm==0&&state.faceFilterCustomFit.turnDegrees==0&&state.faceFilterCustomFit.headOcclusion,"local reset keeps model and restores whole rigid fitting");
    const auto localBeforeCrown=desktopStudio.faceFilterCustomModel;desktopStudio.faceFilterCustomFit.xCm=3.2f;desktopStudio.faceFilterCustomFit.turnDegrees=-37;
    desktopStudio.faceFilterScale=1.26f;desktopStudio.faceFilterVerticalOffset=0;PublishDesktopStudio();
    Settings::Set("STUDIO_ROYAL_CROWN_SCALE",-1);Settings::Set("STUDIO_ROYAL_CROWN_HEIGHT",-1);
    choice->SetSelection(5);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==5&&Settings::Get("STUDIO_FACE_FILTER")==5&&state.faceFilterScale==1&&state.faceFilterVerticalOffset==0&&size->GetValue()==100&&height->GetValue()==30&&Settings::Get("STUDIO_FACE_FILTER_SCALE")==126&&Settings::Get("STUDIO_FACE_FILTER_HEIGHT")==30&&state.faceFilterCustomModel==localBeforeCrown&&close(state.faceFilterCustomFit.xCm,3.2f)&&state.faceFilterCustomFit.turnDegrees==-37,"fresh crown uses baked100percent zeroheight while preserving local126percent and rigid fit");
    size->SetValue(111);height->SetValue(36);mainWindow->Dispatch(Window::FACE_FILTER_SCALE);state=effectsPipeline.GetStudioSettings();LoadAccessorySettings(saved);
    check(state.faceFilterStyle==5&&close(state.faceFilterScale,1.11f)&&close(state.faceFilterVerticalOffset,.6f)&&saved.faceFilterStyle==5&&close(saved.faceFilterScale,1.11f)&&close(saved.faceFilterVerticalOffset,.6f)&&Settings::Get("STUDIO_ROYAL_CROWN_SCALE")==111&&Settings::Get("STUDIO_ROYAL_CROWN_HEIGHT")==36&&Settings::Get("STUDIO_FACE_FILTER_SCALE")==126,"crown adjustments persist independently of generic/local fit");
    choice->SetSelection(4);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==4&&close(state.faceFilterScale,1.26f)&&state.faceFilterVerticalOffset==0&&size->GetValue()==126&&height->GetValue()==30&&state.faceFilterCustomModel==localBeforeCrown&&close(state.faceFilterCustomFit.xCm,3.2f)&&state.faceFilterCustomFit.turnDegrees==-37,"leaving crown restores saved local scale and all rigid fitting");
    choice->SetSelection(5);mainWindow->Dispatch(Window::FACE_FILTER_STYLE);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==5&&close(state.faceFilterScale,1.11f)&&close(state.faceFilterVerticalOffset,.6f)&&size->GetValue()==111&&height->GetValue()==36,"crown selection restores its own saved adjustment");
    mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);state=effectsPipeline.GetStudioSettings();
    check(state.faceFilterStyle==0&&close(state.faceFilterScale,1.26f)&&state.faceFilterVerticalOffset==0&&Settings::Get("STUDIO_FACE_FILTER_SCALE")==126&&Settings::Get("STUDIO_ROYAL_CROWN_SCALE")==111,"Original exits crown and retains both fit profiles");
    logger<<(passed?"[UI-VERIFY] Royal crown default/independent fit/reload/local restore PASS":"[UI-VERIFY] Royal crown fit FAIL")<<std::endl;
    using namespace PrismForge::Effects::Overlays::Realistic;
    for(auto mode:{ModelPerformanceMode::Auto,ModelPerformanceMode::Performance,ModelPerformanceMode::Balanced,ModelPerformanceMode::Quality,ModelPerformanceMode::Custom}){
        ModelPerformanceSettings requested;requested.mode=mode;requested.customNodes=10000;requested.customVertices=310000;requested.customTriangles=420000;requested.customRenderEdge=1200;
        mainWindow->SetModelPerformancePresentation(requested,"Verification",false,false);mainWindow->Dispatch(Window::FACE_FILTER_PERFORMANCE);
        const auto expected=ResolveModelResourceBudget(requested,modelHardware);const auto reloaded=LoadModelPerformanceSettings();
        check(modelPerformance.mode==mode&&reloaded.mode==mode&&reloaded.customNodes==10000&&reloaded.customVertices==310000&&reloaded.customTriangles==420000&&reloaded.customRenderEdge==1200&&effectsPipeline.GetStudioSettings().faceFilterRenderEdge==expected.renderEdge,"all performance choices dispatch, persist and reach renderer");
    }
    auto large=std::make_shared<NativeFaceFilter>();large->vertices.resize(31000);large->triangles.resize(31000);
    desktopStudio.faceFilterCustomModel=large;desktopStudio.faceFilterStyle=4;desktopStudio.faceFilterCustomFit.xCm=3.2f;
    ModelPerformanceSettings budgetRequest;budgetRequest.mode=ModelPerformanceMode::Performance;
    mainWindow->SetModelPerformancePresentation(budgetRequest,"Verification",false,false);mainWindow->Dispatch(Window::FACE_FILTER_PERFORMANCE);
    check(localModelBudgetHeld&&effectsPipeline.GetStudioSettings().faceFilterStyle==0&&!effectsPipeline.GetStudioSettings().glasses&&!effectsPipeline.GetStudioSettings().cap&&desktopStudio.faceFilterStyle==4&&desktopStudio.faceFilterCustomModel==large&&desktopStudio.faceFilterCustomFit.xCm==3.2f,"smaller budget pauses oversized filter while retaining immutable model and fit");
    budgetRequest.mode=ModelPerformanceMode::Quality;desktopLook.bypassed=true;
    mainWindow->SetModelPerformancePresentation(budgetRequest,"Verification",false,true);mainWindow->Dispatch(Window::FACE_FILTER_PERFORMANCE);
    check(!localModelBudgetHeld&&modelPerformance.mode==ModelPerformanceMode::Quality&&effectsPipeline.GetStudioSettings().faceFilterStyle==4&&effectsPipeline.GetStudioSettings().faceFilterCustomModel==large&&desktopStudio.faceFilterCustomFit.xCm==3.2f,"higher budget restores model and fit and can be configured during bypass");
    localFaceFilterResult=std::make_shared<LocalFaceFilterResult>();budgetRequest.mode=ModelPerformanceMode::Performance;
    mainWindow->SetModelPerformancePresentation(budgetRequest,"Verification",false,false);mainWindow->Dispatch(Window::FACE_FILTER_PERFORMANCE);
    check(modelPerformance.mode==ModelPerformanceMode::Quality,"busy worker cannot change its captured import budget");localFaceFilterResult.reset();desktopLook.bypassed=false;
    for(int value:{-1,5}){Settings::Set("STUDIO_MODEL_MODE",value);check(LoadModelPerformanceSettings().mode==ModelPerformanceMode::Auto,"invalid saved mode defaults Auto");}
    for(int value:{-1,511,16385}){Settings::Set("STUDIO_MODEL_NODES",value);check(LoadModelPerformanceSettings().customNodes==4096,"invalid saved nodes defaults bounded");}
    for(int value:{-1,999,1000001}){Settings::Set("STUDIO_MODEL_VERTICES",value);Settings::Set("STUDIO_MODEL_TRIANGLES",value);const auto restored=LoadModelPerformanceSettings();check(restored.customVertices==200000&&restored.customTriangles==200000,"invalid saved geometry defaults bounded");}
    for(int value:{-1,255,1537}){Settings::Set("STUDIO_MODEL_RENDER_EDGE",value);check(LoadModelPerformanceSettings().customRenderEdge==1024,"invalid saved render detail defaults bounded");}
    // Keep the legacy worker fixture independent of the machine's Auto recommendation.
    modelPerformance={};modelPerformance.mode=ModelPerformanceMode::Balanced;desktopStudio.faceFilterCustomModel.reset();desktopStudio.faceFilterStyle=0;PublishDesktopStudio();
    check(VerifyLocalFaceFilterWorker(),"real local import/restore/remove worker checkpoint");
    logger<<(passed?"[UI-VERIFY] Model performance dispatch/persistence/busy/bypass/hold/restore PASS":"[UI-VERIFY] Model performance settings FAIL")<<std::endl;
    Settings::Set("STUDIO_FACE_FILTER_SCALE",heldGeneralScale);Settings::Set("STUDIO_FACE_FILTER_HEIGHT",heldGeneralHeight);Settings::Set("STUDIO_ROYAL_CROWN_SCALE",heldCrownScale);Settings::Set("STUDIO_ROYAL_CROWN_HEIGHT",heldCrownHeight);
    modelPerformance=heldPerformance;desktopStudio=held;desktopLook=heldLook;activeEffectsPreset=heldPreset;PublishDesktopStudio();PublishDesktopLook();
    return passed;
}

bool Application::VerifyAccessorySettings()
{
    if(!mainWindow)return false;
    const auto heldStudio=desktopStudio;const auto heldLook=desktopLook;const int heldPreset=activeEffectsPreset;
    bool passed=true;auto check=[&](bool ok,const char* reason){if(!ok){passed=false;logger<<"[UI-VERIFY] Face accessories settings failure: "<<reason<<std::endl;}};
    auto close=[](float a,float b){return std::abs(a-b)<.00001f;};
    for(int value:{-1,49,151}){Settings::Set("STUDIO_ACCESSORY_SCALE",value);check(LoadAccessoryScale()==1.f,"invalid saved glasses sizes default to 100%");}
    for(int value:{-1,74,151}){Settings::Set("STUDIO_CAP_SCALE",value);check(LoadCapScale()==1.f,"invalid saved cap sizes default to 100%");}
    for(int value:{-1,51}){Settings::Set("STUDIO_CAP_HEIGHT",value);check(LoadCapHeight()==0.f,"invalid saved cap heights default to zero");}
    for(int value:{-1,4}){Settings::Set("STUDIO_GLASSES_STYLE",value);check(LoadAccessoryGlassesStyle()==0,"missing or invalid glasses style defaults to Classic Wayfarer");}
    for(int value=0;value<4;++value){Settings::Set("STUDIO_GLASSES_STYLE",value);check(LoadAccessoryGlassesStyle()==value,"all reference glasses styles load");}
    for(int value:{-1,0,1,3}){
        Settings::Set("STUDIO_CAP",1);Settings::Set("STUDIO_CAP_STYLE",value);
        PrismForge::Effects::DesktopStudioSettings migrated;LoadAccessorySettings(migrated);
        check(!migrated.cap&&migrated.capStyle==2,"retired, missing and invalid saved caps migrate Off without selecting a replacement");
    }
    Settings::Set("STUDIO_CAP_STYLE",2);check(LoadAccessoryCapEnabled()&&LoadAccessoryCapStyle()==2,"accepted enabled cap survives restart");
    Settings::Set("STUDIO_GLASSES",1);Settings::Set("STUDIO_GLASSES_STYLE",-1);
    PrismForge::Effects::DesktopStudioSettings legacy;LoadAccessorySettings(legacy);
    check(legacy.glasses&&legacy.glassesStyle==0,"legacy enabled glasses keep Classic Wayfarer");
    desktopLook.bypassed=false;desktopStudio.glasses=false;desktopStudio.glassesStyle=0;desktopStudio.cap=false;desktopStudio.capStyle=2;
    desktopStudio.accessoryScale=1.f;desktopStudio.capScale=1.f;desktopStudio.capVerticalOffset=0.f;
    desktopStudio.touchupStrength=.45f;desktopStudio.touchupDetail=.6f;
    desktopStudio.light=PrismForge::Effects::DesktopLightMode::Custom;
    desktopStudio.background=PrismForge::Effects::DesktopBackgroundMode::Blur;
    PublishDesktopStudio();PublishDesktopLook();
    auto* glasses=mainWindow->GetAccessoryGlassesChoice();auto* cap=mainWindow->GetAccessoryCapChoice();
    auto* size=mainWindow->GetAccessoryScaleSlider();auto* capSize=mainWindow->GetCapScaleSlider();auto* height=mainWindow->GetCapHeightSlider();
    const auto layout=mainWindow->GetInspectorLayoutRevisionForTesting();
    auto restartMatches=[&](const PrismForge::Effects::DesktopStudioSettings& expected){
        PrismForge::Effects::DesktopStudioSettings restarted;LoadAccessorySettings(restarted);
        check(restarted.glasses==expected.glasses&&restarted.glassesStyle==expected.glassesStyle&&restarted.cap==expected.cap&&
            restarted.capStyle==expected.capStyle&&close(restarted.accessoryScale,expected.accessoryScale)&&
            close(restarted.capScale,expected.capScale)&&close(restarted.capVerticalOffset,expected.capVerticalOffset),"persisted accessory state reconstructs through the actual startup loaders");
    };
    for(int style=0;style<4;++style)for(int capSelection:{0,1}){
        glasses->SetSelection(style+1);cap->SetSelection(capSelection);
        mainWindow->Dispatch(Window::ACCESSORY_GLASSES);mainWindow->Dispatch(Window::ACCESSORY_CAP);
        auto configured=effectsPipeline.GetStudioSettings();
        check(configured.glasses&&configured.glassesStyle==style&&configured.cap==bool(capSelection)&&configured.capStyle==2&&
            Settings::Get("STUDIO_GLASSES")==1&&Settings::Get("STUDIO_GLASSES_STYLE")==style&&
            Settings::Get("STUDIO_CAP")==capSelection&&Settings::Get("STUDIO_CAP_STYLE")==2,"four glasses styles coexist independently with cap Off/on");
        for(int value:{50,137,150}){
            const auto before=effectsPipeline.GetStudioSettings();size->SetValue(value);mainWindow->Dispatch(Window::ACCESSORY_SCALE);
            configured=effectsPipeline.GetStudioSettings();
            check(close(configured.accessoryScale,value/100.f)&&Settings::Get("STUDIO_ACCESSORY_SCALE")==value&&
                configured.capScale==before.capScale&&configured.capVerticalOffset==before.capVerticalOffset,"glasses size changes leave cap fit unchanged");
        }
        for(int value:{75,113,150}){
            const auto before=effectsPipeline.GetStudioSettings();capSize->SetValue(value);mainWindow->Dispatch(Window::ACCESSORY_CAP_SCALE);
            configured=effectsPipeline.GetStudioSettings();
            check(close(configured.capScale,value/100.f)&&Settings::Get("STUDIO_CAP_SCALE")==value&&
                configured.accessoryScale==before.accessoryScale&&configured.capVerticalOffset==before.capVerticalOffset,"cap size changes leave glasses and cap height unchanged");
        }
        for(int value:{0,19,20,37,50}){
            const auto before=effectsPipeline.GetStudioSettings();height->SetValue(value);mainWindow->Dispatch(Window::ACCESSORY_CAP_HEIGHT);
            configured=effectsPipeline.GetStudioSettings();
            check(close(configured.capVerticalOffset,value/10.f-2.f)&&Settings::Get("STUDIO_CAP_HEIGHT")==value&&
                configured.accessoryScale==before.accessoryScale&&configured.capScale==before.capScale,"cap height uses signed tenth-centimetre mapping without changing either size");
            restartMatches(configured);
        }
        glasses->SetSelection(0);cap->SetSelection(0);mainWindow->Dispatch(Window::ACCESSORY_GLASSES);
        configured=effectsPipeline.GetStudioSettings();
        check(!configured.glasses&&!configured.cap&&configured.glassesStyle==style&&configured.capStyle==2&&
            close(configured.accessoryScale,1.5f)&&close(configured.capScale,1.5f)&&close(configured.capVerticalOffset,3.f),"Off retains both styles and all independent fit values");
        restartMatches(configured);
        size->SetValue(100);mainWindow->Dispatch(Window::ACCESSORY_SCALE);
        capSize->SetValue(100);mainWindow->Dispatch(Window::ACCESSORY_CAP_SCALE);
        height->SetValue(20);mainWindow->Dispatch(Window::ACCESSORY_CAP_HEIGHT);
        configured=effectsPipeline.GetStudioSettings();
        check(!configured.glasses&&!configured.cap&&configured.glassesStyle==style&&configured.accessoryScale==1.f&&
            configured.capScale==1.f&&configured.capVerticalOffset==0.f,"resetting each fit independently keeps Off and selected styles");
        check(configured.touchupStrength==.45f&&configured.touchupDetail==.6f&&configured.light==PrismForge::Effects::DesktopLightMode::Custom&&
            configured.background==PrismForge::Effects::DesktopBackgroundMode::Blur,"accessory edits preserve appearance, lighting and background settings");
        check(mainWindow->GetInspectorLayoutRevisionForTesting()==layout,"all accessory routes avoid inspector layout");
    }
    glasses->SetSelection(4);cap->SetSelection(1);size->SetValue(137);capSize->SetValue(123);height->SetValue(13);
    mainWindow->Dispatch(Window::ACCESSORY_GLASSES);desktopLook.bypassed=true;PublishDesktopLook();
    check(!glasses->IsEnabled()&&!cap->IsEnabled()&&!size->IsEnabled()&&!capSize->IsEnabled()&&!height->IsEnabled(),"bypass disables every accessory control");
    glasses->SetSelection(1);cap->SetSelection(0);size->SetValue(50);capSize->SetValue(75);height->SetValue(50);
    for(int id:{Window::ACCESSORY_GLASSES,Window::ACCESSORY_CAP,Window::ACCESSORY_SCALE,Window::ACCESSORY_CAP_SCALE,Window::ACCESSORY_CAP_HEIGHT})mainWindow->Dispatch(id);
    auto configured=effectsPipeline.GetStudioSettings();
    check(configured.glasses&&configured.glassesStyle==3&&configured.cap&&configured.capStyle==2&&close(configured.accessoryScale,1.37f)&&
        close(configured.capScale,1.23f)&&close(configured.capVerticalOffset,-.7f)&&Settings::Get("STUDIO_ACCESSORY_SCALE")==137&&
        Settings::Get("STUDIO_CAP_SCALE")==123&&Settings::Get("STUDIO_CAP_HEIGHT")==13,"disabled accessory events retain fitted settings during bypass");
    restartMatches(configured);desktopLook.bypassed=false;PublishDesktopLook();
    check(glasses->IsEnabled()&&cap->IsEnabled()&&size->IsEnabled()&&capSize->IsEnabled()&&height->IsEnabled()&&
        glasses->GetSelection()==4&&cap->GetSelection()==1&&size->GetValue()==137&&capSize->GetValue()==123&&height->GetValue()==13,"leaving bypass restores exact style and fitting controls");
    mainWindow->GetEffectsPresetChoice()->SetSelection(1);mainWindow->Dispatch(Window::EFFECT_PRESET_APPLY);
    configured=effectsPipeline.GetStudioSettings();
    check(!configured.glasses&&!configured.cap&&configured.glassesStyle==3&&configured.capStyle==2&&close(configured.accessoryScale,1.37f)&&
        close(configured.capScale,1.23f)&&close(configured.capVerticalOffset,-.7f)&&glasses->GetSelection()==0&&cap->GetSelection()==0,"Original turns accessories off while retaining styles and fit");
    restartMatches(configured);
    desktopStudio=heldStudio;desktopLook=heldLook;activeEffectsPreset=heldPreset;PublishDesktopStudio();PublishDesktopLook();
    return passed;
}

void Application::RefreshSegmentationStatus() {
    if(!mainWindow)return;
    using PrismForge::Effects::DesktopLightMode;
    const bool lightEnabled=desktopStudio.light!=DesktopLightMode::Off&&desktopStudio.strength>0&&
        (desktopStudio.light!=DesktopLightMode::Custom||(desktopStudio.keyLight.enabled&&desktopStudio.keyLight.intensity>0)||
            (desktopStudio.fillLight.enabled&&desktopStudio.fillLight.intensity>0));
    const bool faceLightingEnabled=desktopStudio.faceLighting&&lightEnabled&&!desktopLook.bypassed;
    const bool touchupEnabled=desktopStudio.touchupStrength>0&&desktopStudio.touchupDetail<1.f&&!desktopLook.bypassed;
    if(!faceLightingEnabled)mainWindow->SetFaceLightingStatus("Off",desktopLook.bypassed?
        "Bypass keeps your face lighting settings.":"Choose Face with shadows and enable a light to apply face shading.");
    if(!touchupEnabled)mainWindow->SetTouchupStatus("Off",desktopLook.bypassed?
        wxString("Bypass keeps your face touch-up settings."):desktopStudio.touchupDetail>=1.f?
        wxString("Detail preservation at 100% leaves the face unchanged by touch-up. Lower it to apply smoothing."):
        wxString("Increase Smoothing strength to enable face touch-up."));
    const bool faceFilterSelected=desktopStudio.faceFilterStyle>0&&!desktopLook.bypassed;
    const bool faceFilterEnabled=faceFilterSelected&&!localModelBudgetHeld;
    const bool accessoriesEnabled=!faceFilterSelected&&(desktopStudio.glasses||desktopStudio.cap)&&!desktopLook.bypassed;
    if(localModelBudgetHeld&&!desktopLook.bypassed)mainWindow->SetFaceFilterStatus("Paused","This model exceeds the selected geometry budget. Choose Quality or raise Custom limits to restore it. Model and fit are retained.");
    else if(!faceFilterEnabled)mainWindow->SetFaceFilterStatus("Off",desktopLook.bypassed?
        "Bypass keeps your 3D filter selection and fit.":"Choose a 3D face filter to use it on your preview and webcam.");
    if(faceFilterSelected)mainWindow->SetAccessoryStatus("Paused","Your 3D filter is active. Choose its Off option to restore your saved accessories.");
    if(!accessoriesEnabled&&!faceFilterSelected)mainWindow->SetAccessoryStatus("Off",desktopLook.bypassed?
        "Bypass keeps your accessory choices and size.":"Choose Classic Wayfarer or a cap to enable face accessories.");
    if(faceLightingEnabled||touchupEnabled||accessoriesEnabled||faceFilterEnabled){
        // All facial effects share one native service; each reports its own stage outcome.
        const auto face=effectsPipeline.GetFaceAnalysisState();const auto report=effectsPipeline.GetLastReport();
        using FacePhase=PrismForge::Providers::FaceAnalysisPhase;
        using StageStatus=PrismForge::Effects::StudioStack::StageStatus;
        auto statusFor=[&](int kind,StageStatus stage){
            auto update=[&](const wxString& status,const wxString& detail){
                if(kind==0)mainWindow->SetFaceLightingStatus(status,detail);
                else if(kind==1)mainWindow->SetTouchupStatus(status,detail);
                else if(kind==2)mainWindow->SetAccessoryStatus(status,detail);
                else mainWindow->SetFaceFilterStatus(status,detail);
            };
            const wxString effect=kind==0?wxString("Face lighting"):kind==1?wxString("Face touch-up"):kind==2?wxString("Face accessories"):wxString("3D face filter");
            if(face.phase==FacePhase::Failed||face.phase==FacePhase::Stopped){
                wxString detail=effect+" could not start. ";
                if(!face.lastError.empty())detail+=wxString::FromUTF8(face.lastError).Left(100)+" ";
                detail+="Turn all face effects Off, then enable an effect to retry.";update("Unavailable",detail);
            }else if(face.phase==FacePhase::Loading){
                update("Loading","Preparing face analysis. Your camera stays live.");
            }else if(sourceKind==PrismForge::Input::Kind::None||!face.ready||face.sourceGeneration!=inputGate.Current()){
                update("Waiting for current frame","Connect a camera and keep your face visible.");
            }else if(!face.lastError.empty()){
                update("Unavailable",effect+" skipped a frame. "+wxString::FromUTF8(face.lastError).Left(150));
            }else if(face.lastCompletedSequence&&!face.lastDetected){
                update("No face",kind>=2?wxString("Keep your face visible. The filter hides while no face is found."):
                    "Keep your face visible to apply "+(kind==0?wxString("face shading."):wxString("face touch-up.")));
            }else if(report.sourceGeneration==face.sourceGeneration&&stage==StageStatus::Applied){
                update("Tracking",kind==0?wxString("Face shading with approximate shadows is active."):
                    kind==1?wxString("Facial-area smoothing is active; eyes, lips and brows are preserved."):
                    kind==2?wxString("Your selected accessories are following your face."):wxString("Your 3D filter is following your face on preview and webcam output."));
            }else if(stage==StageStatus::Failed||stage==StageStatus::Unsupported){
                update("Unavailable",effect+" could not process this frame. "+wxString::FromUTF8(report.errorDescription).Left(150));
            }else{
                update("Waiting for current frame","Waiting for fresh face geometry. Your camera stays live.");
            }
        };
        if(faceLightingEnabled)statusFor(0,report.lightingStatus);
        if(touchupEnabled)statusFor(1,report.appearanceStatus);
        if(faceFilterEnabled){
            const int filter=report.overlayLayers.faceFilter;
            statusFor(3,report.overlayStatus==StageStatus::Unsupported?StageStatus::Unsupported:
                filter==3||report.overlayStatus==StageStatus::Failed?StageStatus::Failed:filter==0?StageStatus::Applied:StageStatus::NotReady);
        }
        if(accessoriesEnabled){
            // A frame border can report Overlay Applied while a tracked accessory is absent.
            const int glasses=desktopStudio.glasses?report.overlayLayers.glasses:0;
            const int cap=desktopStudio.cap?report.overlayLayers.hat:0;
            statusFor(2,report.overlayStatus==StageStatus::Unsupported?StageStatus::Unsupported:
                (glasses==3||cap==3||report.overlayStatus==StageStatus::Failed)?StageStatus::Failed:
                (glasses==0&&cap==0)?StageStatus::Applied:StageStatus::NotReady);
        }
    }
    if(desktopLook.bypassed){mainWindow->SetSegmentationStatus("Bypassed","Your background, lighting and touch-up settings are kept.");return;}
    const bool subjectLighting=desktopStudio.lightingSubjectOnly&&!desktopStudio.faceLighting&&desktopStudio.light!=DesktopLightMode::Off&&desktopStudio.strength>0&&
        (desktopStudio.light!=DesktopLightMode::Custom||(desktopStudio.keyLight.enabled&&desktopStudio.keyLight.intensity>0)||
            (desktopStudio.fillLight.enabled&&desktopStudio.fillLight.intensity>0));
    if(desktopStudio.background==PrismForge::Effects::DesktopBackgroundMode::Off&&!subjectLighting){
        mainWindow->SetSegmentationStatus("Off","Enable a background effect or Person only lighting to analyse the person.");return;
    }
    auto state=effectsPipeline.GetSegmentationState();
    using Phase=PrismForge::Providers::SegmentationPhase;
    wxString status=state.phase==Phase::Ready?"Ready":state.phase==Phase::Loading?"Starting":
        state.phase==Phase::Recovering?"Recovering":state.phase==Phase::Failed?"Unavailable":"Waiting for camera";
    wxString detail=wxString::FromUTF8(state.message);
    if(state.phase==Phase::Ready){
        if(state.consecutiveInferenceErrors)status="Retrying";else if(!state.currentCompletedFrames)status="Starting";
        detail=state.activeProvider==PrismForge::Providers::ExecutionProviderType::DirectML?"Person detection: GPU":"Person detection: CPU";
        detail+=state.highQuality?"; High quality":"; Balanced";
        if(state.fallbackOccurred)detail+=" (fallback)";
        if(!state.currentCompletedFrames)detail+="; waiting for first outline";
        else if(effectsPipeline.GetLastReport().backgroundStatus==PrismForge::Effects::StudioStack::StageStatus::NotReady||
            (subjectLighting&&effectsPipeline.GetLastReport().lightingStatus==PrismForge::Effects::StudioStack::StageStatus::NotReady))detail+="; waiting for a current outline";
        if(state.consecutiveInferenceErrors)detail+="; retrying person detection";
    }else if(state.phase==Phase::Recovering){
        detail=wxString::Format("Restarting person detection (%u of %u). Your camera stays live.",
            unsigned(state.recoveryAttempts),unsigned(state.maxRecoveryAttempts));
    }else if(state.phase==Phase::Failed&&state.recoveryAttempts){
        detail+=" Turn background and lighting Off, then enable the effect to try again.";
    }
    mainWindow->SetSegmentationStatus(status,detail);
}
bool Application::LoadBackgroundImage(const wxString& path) {
    // WIC queries dimensions before allocating pixels, bounding compressed-image expansion.
    wxFileName filename(path);const auto bytes=filename.GetSize();
    if(!filename.FileExists()||bytes==wxInvalidSize||bytes>64ULL*1024*1024)return false;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct ComGuard{bool release;~ComGuard(){if(release)CoUninitialize();}} guard{SUCCEEDED(com)};
    if(FAILED(com)&&com!=RPC_E_CHANGED_MODE)return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> image;ComPtr<IWICFormatConverter> converter;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||
       FAILED(factory->CreateDecoderFromFilename(path.wc_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))||
       FAILED(decoder->GetFrame(0,&image)))return false;
    UINT w=0,h=0;if(FAILED(image->GetSize(&w,&h))||!w||!h||w>8192||h>8192||uint64_t(w)*h>16ULL*1024*1024)return false;
    if(FAILED(factory->CreateFormatConverter(&converter))||FAILED(converter->Initialize(image.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
    AVFrame* raw=av_frame_alloc();if(!raw)return false;
    std::shared_ptr<AVFrame> owned(raw,[](AVFrame* p){av_frame_free(&p);});
    raw->format=AV_PIX_FMT_BGR24;raw->width=int(w);raw->height=int(h);raw->color_range=AVCOL_RANGE_JPEG;raw->colorspace=AVCOL_SPC_RGB;
    if(av_frame_get_buffer(raw,64)<0||FAILED(converter->CopyPixels(nullptr,raw->linesize[0],raw->linesize[0]*h,raw->data[0])))return false;
    desktopStudio.backgroundImage=std::move(owned);backgroundImagePath=filename.GetFullPath();backgroundImageName=filename.GetFullName();return true;
}
void Application::SaveBackgroundImagePath() {
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr))return;
    auto path=Settings::GetDocumentsDirectoryPath();path.SetFullName("desktop-studio.ini");
    wxFileConfig config("PrismForgeStudio",wxEmptyString,path.GetFullPath(),wxEmptyString,wxCONFIG_USE_LOCAL_FILE);
    config.Write("BackgroundImage",backgroundImagePath);config.Flush();
}
bool Application::HandleBackgroundAction(wxCommandEvent& event) {
    const int id=event.GetId();if(id<Window::BACKGROUND_MODE||id>Window::EFFECT_PRESET_APPLY)return false;
    if(closing||!mainWindow)return true;
    if(desktopLook.bypassed)return true;
    using namespace PrismForge::Effects;
    if(id==Window::BACKGROUND_IMPORT||id==Window::BACKGROUND_COLOUR) {
        if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)){mainWindow->Notify("Background files and colour dialogs are disabled in UI preview mode.");return true;}
        if(id==Window::BACKGROUND_IMPORT){wxFileDialog file(mainWindow,"Choose a background image",wxEmptyString,wxEmptyString,"Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(file.ShowModal()!=wxID_OK)return true;
            if(!LoadBackgroundImage(file.GetPath())){mainWindow->Notify("Image rejected: use a supported image under 64 MB and 16 megapixels.");return true;}
            desktopStudio.background=DesktopBackgroundMode::Replace;SaveBackgroundImagePath();}
        else{wxColourData colour;colour.SetColour(wxColour(desktopStudio.backgroundR,desktopStudio.backgroundG,desktopStudio.backgroundB));wxColourDialog dialog(mainWindow,&colour);if(dialog.ShowModal()!=wxID_OK)return true;
            const auto c=dialog.GetColourData().GetColour();desktopStudio.backgroundR=c.Red();desktopStudio.backgroundG=c.Green();desktopStudio.backgroundB=c.Blue();}
        activeEffectsPreset=0;
    }else if(id==Window::BACKGROUND_CLEAR){desktopStudio.backgroundImage.reset();backgroundImagePath.clear();backgroundImageName.clear();SaveBackgroundImagePath();activeEffectsPreset=0;}
    else if(id==Window::EFFECT_PRESET_APPLY){activeEffectsPreset=std::clamp(mainWindow->GetEffectsPresetChoice()->GetSelection(),0,3);
        if(activeEffectsPreset==1){desktopLook.selected=Look::Original;desktopLut.reset();desktopLutName.clear();desktopStudio.light=DesktopLightMode::Off;desktopStudio.studioFrame=false;desktopStudio.background=DesktopBackgroundMode::Off;desktopStudio.touchupStrength=0;desktopStudio.glasses=false;desktopStudio.cap=false;if(desktopStudio.faceFilterStyle==5){desktopStudio.faceFilterScale=LoadFaceFilterScale();desktopStudio.faceFilterVerticalOffset=LoadFaceFilterHeight();}desktopStudio.faceFilterStyle=0;}
        if(activeEffectsPreset==2){desktopStudio.background=DesktopBackgroundMode::Blur;desktopStudio.blurStrength=35;}
        if(activeEffectsPreset==3){desktopStudio.background=DesktopBackgroundMode::Replace;desktopStudio.light=DesktopLightMode::Warm;desktopStudio.strength=.5f;}
        desktopLook.bypassed=false;PublishDesktopLook();}
    else if(id==Window::BACKGROUND_QUALITY){desktopStudio.highQualityMask=mainWindow->GetBackgroundQualityChoice()->GetSelection()==1;}
    else{desktopStudio.background=static_cast<DesktopBackgroundMode>(std::clamp(mainWindow->GetBackgroundChoice()->GetSelection(),0,2));desktopStudio.blurStrength=std::clamp(mainWindow->GetBackgroundBlurSlider()->GetValue(),0,100);activeEffectsPreset=0;}
    PublishDesktopStudio();return true;
}

void Application::RefreshLookPresets()
{
    auto* choice = mainWindow->GetLookPresetChoice();
    const auto selected = choice->GetStringSelection();
    choice->Clear();
    choice->Append("Custom");
    for (const auto& [name, preset] : lookPresets.custom) choice->Append(name);
    if (!choice->SetStringSelection(selected)) choice->SetSelection(0);
    mainWindow->UpdateInspectorLayout();
}

bool Application::HandleDesktopLookAction(wxCommandEvent& event)
{
    const int command = event.GetId();
    if(command==Window::PACK_LIBRARY){if(!closing&&mainWindow)ShowPackLibrary();return true;}
    if (command < Window::LOOK_CHANGED || command > Window::LUT_INTENSITY) return false;
    if (closing || !mainWindow) return true;
    if (command == Window::LUT_INTENSITY) {
        activeEffectsPreset=0;
        desktopLutIntensity = std::clamp(mainWindow->GetLutIntensitySlider()->GetValue(), 0, 100);
        if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr))Settings::Set("DESKTOP_LUT_INTENSITY",desktopLutIntensity);
        PublishDesktopLook();
        return true;
    }
    if (command == Window::LUT_CLEAR) {
        ClearPackSelection();
        activeEffectsPreset=0;desktopLut.reset(); desktopLutName.clear(); desktopLutIntensity = 100;
        if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr))Settings::Set("DESKTOP_LUT_INTENSITY",100);
        PublishDesktopLook();
        mainWindow->Notify("Custom LUT cleared. The built-in look is kept.");
        return true;
    }
    if (command == Window::LOOK_CHANGED) {
        activeEffectsPreset=0;desktopLook.Select(mainWindow->GetLookChoice()->GetSelection());
        PublishDesktopLook();
        mainWindow->Notify(desktopLook.bypassed ? wxString("Look selected. Restore look to apply it.") :
            "Colour look: " + mainWindow->GetLookChoice()->GetStringSelection());
        return true;
    }
    if (command == Window::LOOK_BYPASS) {
        desktopLook.bypassed = !desktopLook.bypassed;
        PublishDesktopLook();
        mainWindow->Notify(desktopLook.bypassed ? "Desktop effects bypassed. Your lighting, touch-up, frame and colour choices are kept." : "Selected desktop effects restored.");
        return true;
    }
    if (command == Window::LOOK_PRESET_APPLY) {
        const auto found = lookPresets.custom.find(mainWindow->GetLookPresetChoice()->GetStringSelection());
        if (found != lookPresets.custom.end()) {
            activeEffectsPreset=0;desktopLook.Select(static_cast<int>(found->second.look));
            desktopLook.bypassed = false;
            PublishDesktopLook();
            mainWindow->Notify("Desktop look preset applied.");
        }
        return true;
    }
    // UI preview permits comparison controls, but never reads/writes profile files.
    if (wxGetEnv("PRISMFORGE_UI_PREVIEW", nullptr)) {
        mainWindow->Notify("Preset and LUT files are disabled in UI preview mode.");
        return true;
    }
    if (command == Window::LUT_IMPORT) {
        wxFileDialog file(mainWindow, "Load custom colour LUT", "", "",
            "3D colour LUT (*.cube)|*.cube", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return true;
        std::string error;
        auto lut = PrismForge::Effects::LoadCubeLutFile(std::filesystem::path(file.GetPath().ToStdWstring()), error);
        if (!lut) {
            mainWindow->Notify("LUT rejected: " + wxString::FromUTF8(error));
            return true;
        }
        ClearPackSelection();
        activeEffectsPreset=0;desktopLut = std::move(lut);
        desktopLutName = wxFileName(file.GetPath()).GetFullName();
        desktopLutIntensity = 100;
        Settings::Set("DESKTOP_LUT_INTENSITY",100);
        PublishDesktopLook();
        mainWindow->Notify(desktopLook.bypassed ? "LUT loaded. Restore look to apply it." : "Custom LUT loaded.");
        return true;
    }
    if (command == Window::LOOK_PRESET_SAVE || command == Window::LOOK_PRESET_IMPORT) {
        DesktopLookPreset preset{desktopLook.selected};
        if (command == Window::LOOK_PRESET_IMPORT) {
            wxFileDialog file(mainWindow, "Import desktop look preset", "", "",
                "PrismForge look (*.prismlook)|*.prismlook", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (file.ShowModal() != wxID_OK) return true;
            if (!DesktopLookPreset::ReadFile(file.GetPath(), preset)) {
                mainWindow->Notify("Look preset rejected: unsupported format or file larger than 32 KB.");
                return true;
            }
        }
        wxTextEntryDialog dialog(mainWindow, "Choose a unique name (up to 48 characters).", "Save desktop look preset");
        if (dialog.ShowModal() != wxID_OK) return true;
        const auto name = dialog.GetValue().Trim().Trim(false);
        if (!LookPresetStore::ValidName(name) || lookPresets.custom.count(name) || lookPresets.custom.size() >= 50) {
            mainWindow->Notify("Choose a unique name without slashes, brackets or control characters. Up to 50 presets are supported.");
            return true;
        }
        lookPresets.custom.emplace(name, preset);
        if (!lookPresets.Save()) {
            lookPresets.custom.erase(name);
            mainWindow->Notify("Could not save the desktop look preset.");
            return true;
        }
        RefreshLookPresets();
        if (command == Window::LOOK_PRESET_SAVE) mainWindow->GetLookPresetChoice()->SetStringSelection(name);
        mainWindow->Notify(command == Window::LOOK_PRESET_IMPORT ? "Look preset imported. Choose it to apply it." : "Desktop look preset saved.");
        return true;
    }
    if (command == Window::LOOK_PRESET_EXPORT) {
        wxFileDialog file(mainWindow, "Export desktop look preset", "", "Desktop.prismlook",
            "PrismForge look (*.prismlook)|*.prismlook", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (file.ShowModal() != wxID_OK) return true;
        mainWindow->Notify(DesktopLookPreset{desktopLook.selected}.Export(file.GetPath()) ?
            "Desktop look exported." : "Could not export the desktop look preset.");
        return true;
    }
    if (command == Window::LOOK_PRESET_DELETE) {
        const auto name = mainWindow->GetLookPresetChoice()->GetStringSelection();
        const auto found = lookPresets.custom.find(name);
        if (found == lookPresets.custom.end()) {
            mainWindow->Notify("Choose a saved desktop look preset to delete it.");
            return true;
        }
        if (wxMessageBox("Remove the saved look preset \"" + name + "\"?", "Remove look preset",
                wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, mainWindow) != wxYES) return true;
        const auto saved = found->second;
        lookPresets.custom.erase(found);
        if (!lookPresets.Save()) {
            lookPresets.custom.emplace(name, saved);
            mainWindow->Notify("Could not remove the desktop look preset.");
        } else {
            RefreshLookPresets();
            mainWindow->Notify("Look preset removed. The current look is kept.");
        }
        return true;
    }
    return false;
}

void Application::RefreshPresets()
{
    auto* choice = mainWindow->GetPresetChoice(); auto selected = choice->GetStringSelection();
    choice->Clear(); for (const auto& name : {"Custom", "Meeting", "Low bandwidth", "High quality"}) choice->Append(name);
    for (const auto& [name,preset] : presets.custom) choice->Append(name);
    if (!choice->SetStringSelection(selected)) choice->SetSelection(0);
}
void Application::RefreshStudio()
{
    if (!mainWindow || !rtspManager || closing) return;
    const auto& devices = rtspManager->GetDescriptors(); int id=rtspManager->GetStreamingDevice();
    if (sourceKind==PrismForge::Input::Kind::System && systemDevice) {
        int selected=-1;
        if (selectedSystemMode) for (size_t i=0;i<systemModes.size();++i)
            if (systemModes[i]==*selectedSystemMode) {selected=(int)i;break;}
        mainWindow->SetSystemCameraState(wxString::FromUTF8(systemDevice->displayName),systemModes,selected,systemStatus);
    } else if (id >= 0 && id < (int)devices.size()) {
        auto& state=GetCurrentDeviceStreamOptions(); mainWindow->SetDeviceState(&devices[id],&state);
    } else mainWindow->SetDeviceState(nullptr,nullptr);
    auto* sources=mainWindow->GetSourceChoice();
    for (size_t i=0;i<sourceChoices.size();++i) if (sourceChoices[i].kind==PrismForge::Input::Kind::Mobile) {
        for (const auto& d : devices) if (sourceChoices[i].key==PrismForge::Input::MobileKey(d.name(),d.url()))
            sources->SetString((int)i,(Settings::Get(FavoriteKey(d.name()))==1?"[Pinned] ":"")+wxString::FromUTF8(d.name())+
                (d.url().find("127.0.0.1")!=std::string::npos?"  |  USB":"  |  Wi-Fi"));
    }
    auto& manager = VCamDeviceManager::Instance();
    switch(manager.GetCurrentMode()) {
        case VCamMode::Dynamic: mainWindow->SetOutputStatus(manager.IsSessionCameraActive() ? "Windows camera available\nSelect PrismForge Virtual Camera in your meeting app." : "Windows camera unavailable\nRun the camera installer to enable it."); break;
        case VCamMode::Always: mainWindow->SetOutputStatus("Persistent Windows camera mode\nCheck your meeting app's camera picker."); break;
        case VCamMode::Disabled: mainWindow->SetOutputStatus("DirectShow only\nWindows camera visibility is turned off."); break;
    }
}
void Application::ApplyQuality(const QualityPreset& preset)
{
    int id = rtspManager->GetStreamingDevice(); const auto& devices = rtspManager->GetDescriptors();
    if (id < 0 || id >= (int)devices.size()) return;
    auto& state = GetCurrentDeviceStreamOptions(); const auto& desc = devices[id];
    auto resolution = PresetStore::Nearest(state.backCameraActive ? desc.backResolutions() : desc.frontResolutions(), preset.width, preset.height);
    if (resolution.first == 0) { mainWindow->Notify("This camera has not reported any supported resolutions."); return; }

    bool resChanged = (state.resolution != resolution);
    bool fpsChanged = (state.fps != preset.fps);
    bool bitChanged = (state.bitrate != preset.bitrate || state.adaptiveBitrate != preset.adaptive ||
                       state.minBitrate != preset.minBitrate || state.maxBitrate != preset.maxBitrate);

    state.resolution = resolution;
    state.fps = preset.fps;
    state.bitrate = preset.bitrate;
    state.adaptiveBitrate = preset.adaptive;
    state.minBitrate = preset.minBitrate;
    state.maxBitrate = preset.maxBitrate;

    if (resChanged) {
        rtspManager->SetResolution((uint16_t)resolution.first, (uint16_t)resolution.second);
    }
    if (fpsChanged) {
        rtspManager->SetFPS(preset.fps);
    }
    if (bitChanged) {
        if (preset.adaptive) {
            rtspManager->SetAdaptiveBitrate(preset.minBitrate, preset.maxBitrate);
        } else {
            rtspManager->SetBitrate(preset.bitrate);
        }
    }

    RefreshStudio();
    if (resolution != DeviceDescriptor::Resolution(preset.width, preset.height))
        mainWindow->Notify(wxString::Format("Using the closest supported resolution: %d x %d.", resolution.first, resolution.second));
    else
        mainWindow->Notify("Quality updated. Actual frame rate depends on the phone and receiving app.");
}
bool Application::HandleStudioAction(wxCommandEvent& event)
{
    const int command = event.GetId();
    if (command==Window::REFRESH_SOURCES) {
        if (!closing) { if (server) server->RetryUsb(); RefreshSystemCameras(); }
        return true;
    }
    if (command < Window::QUALITY_CHANGED || command > Window::THEME_GLASS) {
        if (command == Window::SNAPSHOTS) {
            auto path = Settings::GetDocumentsDirectoryPath(); path.AppendDir("Snapshots");
            if (!path.DirExists()) path.Mkdir(0755,wxPATH_MKDIR_FULL);
            wxLaunchDefaultApplication(path.GetPath()); return true;
        }
        if (command == Window::RECONNECT) {
            if (closing) return true;
            if (sourceKind==PrismForge::Input::Kind::System && systemDevice) {
                auto mode=selectedSystemMode;
                StartSystemCamera(*systemDevice,mode ? &*mode : nullptr);
            } else { wxCommandEvent e; OnSourceChanged(e); }
            return true;
        }
        return false;
    }
    if (closing || !rtspManager) return true;
    const int id = rtspManager->GetStreamingDevice(); const auto& devices = rtspManager->GetDescriptors();
    const bool active = id >= 0 && id < (int)devices.size();
    if (command >= Window::THEME_SYSTEM && command <= Window::THEME_GLASS) {
        int theme = command - Window::THEME_SYSTEM; Studio::appearance = theme; Settings::Set("STUDIO_THEME",theme);
        SetAppearance((theme == 1 || theme == 3) ? Appearance::Dark : theme == 2 ? Appearance::Light : Appearance::System);
        mainWindow->GetMenuBar()->Check(command,true); mainWindow->ApplyTheme(); return true;
    }
    if (command == Window::FAVORITE) {
        if (active) { auto key=FavoriteKey(devices[id].name()); Settings::Set(key,Settings::Get(key)==1?0:1); RefreshStudio(); }
        return true;
    }
    if (command == Window::QUALITY_CHANGED) {
        if (sourceKind==PrismForge::Input::Kind::System && systemDevice) {
            int index=mainWindow->GetResolutionChoice()->GetSelection()-1;
            if (index>=0 && index<(int)systemModes.size()) {
                auto mode=systemModes[index]; StartSystemCamera(*systemDevice,&mode);
            } else StartSystemCamera(*systemDevice);
            return true;
        }
        if (!active) return true;
        auto preset = QualityPreset::From(GetCurrentDeviceStreamOptions());
        int res = mainWindow->GetResolutionChoice()->GetSelection();
        if (res >= 0 && res < (int)mainWindow->resolutions.size()) { preset.width=mainWindow->resolutions[res].first; preset.height=mainWindow->resolutions[res].second; }
        const int rates[] = {15,24,30,60}; int index=mainWindow->GetFpsChoice()->GetSelection();
        if(index>=0 && index<4) preset.fps=rates[index];
        int bitrate=mainWindow->GetBitrateSlider()->GetValue();
        if (event.GetInt() == 1 || bitrate != preset.bitrate) { preset.bitrate=bitrate; preset.adaptive=false; }
        if (!(preset == QualityPreset::From(GetCurrentDeviceStreamOptions()))) { mainWindow->GetPresetChoice()->SetSelection(0); ApplyQuality(preset); }
        return true;
    }
    if (command == Window::PRESET_APPLY) {
        if (!active) return true;
        auto name=mainWindow->GetPresetChoice()->GetStringSelection(); QualityPreset preset;
        if(name=="Custom") return true;
        if(name=="Meeting") preset={1280,720,30,4000,false,1500,6000};
        else if(name=="Low bandwidth") preset={640,480,24,1500,true,500,2000};
        else if(name=="High quality") preset={1920,1080,30,8000,false,3000,10000};
        else if(presets.custom.count(name)) preset=presets.custom.at(name); else return true;
        ApplyQuality(preset); return true;
    }
    if (command == Window::PRESET_SAVE || command == Window::PRESET_IMPORT) {
        QualityPreset preset;
        if (command == Window::PRESET_SAVE) { if(!active)return true; preset=QualityPreset::From(GetCurrentDeviceStreamOptions()); }
        else {
            wxFileDialog file(mainWindow,"Import quality profile","","","PrismForge profile (*.vcamprofile)|*.vcamprofile",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(file.ShowModal()!=wxID_OK)return true;
            if(wxFileName(file.GetPath()).GetSize()>32768) { mainWindow->Notify("Profile rejected: maximum file size is 32 KB."); return true; }
            wxFileInputStream input(file.GetPath()); wxFileConfig config(input);
            if(!input.IsOk() || !QualityPreset::Read(config,preset)) { mainWindow->Notify("Profile rejected: missing, unsupported or out-of-range settings."); return true; }
        }
        wxTextEntryDialog dialog(mainWindow,"Choose a unique name (up to 48 characters).","Save quality profile");
        if(dialog.ShowModal()!=wxID_OK)return true;
        auto name=dialog.GetValue().Trim().Trim(false);
        if(!PresetStore::ValidName(name) || presets.custom.count(name) || presets.custom.size()>=50) { mainWindow->Notify("Use a unique profile name without slashes or brackets. Up to 50 profiles are supported."); return true; }
        presets.custom[name]=preset;
        if(!presets.Save()) { presets.custom.erase(name); mainWindow->Notify("Could not save the profile. Your existing profiles are unchanged."); return true; }
        RefreshPresets();
        // Import saves a profile without changing the active camera.
        if(command==Window::PRESET_SAVE) mainWindow->GetPresetChoice()->SetStringSelection(name);
        mainWindow->Notify("Profile saved. Choose it from Quality profile whenever you need it."); return true;
    }
    if(command==Window::PRESET_EXPORT) {
        if(!active)return true;
        wxFileDialog file(mainWindow,"Export quality profile","","Camera.vcamprofile","PrismForge profile (*.vcamprofile)|*.vcamprofile",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK)return true;
        wxMemoryInputStream empty("", 0); wxFileConfig config(empty);
        QualityPreset::From(GetCurrentDeviceStreamOptions()).Write(config);
        wxTempFileOutputStream output(file.GetPath()); bool ok=output.IsOk() && config.Save(output) && output.Commit();
        mainWindow->Notify(ok?"Profile exported without device identifiers or connection addresses.":"Could not export profile."); return true;
    }
    if(command==Window::PRESET_DELETE) {
        auto name=mainWindow->GetPresetChoice()->GetStringSelection(); auto it=presets.custom.find(name);
        if(it==presets.custom.end()) { mainWindow->Notify("Select a saved custom profile to delete it. Built-in profiles are kept."); return true; }
        if (wxMessageBox("Remove the saved profile \"" + name + "\"? Active camera settings will stay as they are.", "Remove profile", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, mainWindow) != wxYES) return true;
        // Removing a profile does not alter active camera settings.
        auto saved=it->second; presets.custom.erase(it);
        if(!presets.Save()) { presets.custom[name]=saved; mainWindow->Notify("Could not remove the profile."); }
        else { RefreshPresets(); mainWindow->GetPresetChoice()->SetSelection(0); mainWindow->Notify("Profile removed. Active camera settings are unchanged."); }
        return true;
    }
    if(command==Window::DIAGNOSTICS) {
        wxFileDialog file(mainWindow,"Export diagnostics","","PrismForge-diagnostics.txt","Text files (*.txt)|*.txt",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK)return true;
        wxString report="PrismForge Studio diagnostics\nNo device names, IP addresses, paths, or raw logs are included.\n";
        report+=wxString::Format("Connected devices: %zu\nSelected device: %s\nWindows camera mode: %d\nSession camera active: %s\n",devices.size(),active?"yes":"no",(int)VCamDeviceManager::Instance().GetCurrentMode(),VCamDeviceManager::Instance().IsSessionCameraActive()?"yes":"no");
        report+="Incoming video: "+mainWindow->GetStatsText()->GetLabel()+"\n";
        report += wxString::Format("Desktop look: %s\nLook bypassed: %s\n", DesktopLookPreset::Token(desktopLook.selected), desktopLook.bypassed ? "yes" : "no");
        if(active) { auto p=QualityPreset::From(GetCurrentDeviceStreamOptions()); report+=wxString::Format("Requested capture: %d x %d at %d fps\nRequested bitrate: %d kbps\n",p.width,p.height,p.fps,p.bitrate); }
        wxTempFile output(file.GetPath()); bool ok=output.IsOpened() && output.Write(report) && output.Commit();
        mainWindow->Notify(ok?"Diagnostics exported with identifying information omitted.":"Could not export diagnostics."); return true;
    }
    return false;
}

void Application::ClearPackSelection()
{
    ++packSelectionEpoch;
    activePackSelection={};
    if(!wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)&&!PackLibraryDialog::SaveSelection(activePackSelection)&&mainWindow)
        mainWindow->Notify("Effect changed, but the saved pack selection could not be cleared.");
}
void Application::ApplyPackLut(const EffectPackLut& value)
{
    if(closing||!mainWindow||!value.lut||value.selection.Empty())return;
    ++packSelectionEpoch;
    activePackSelection=value.selection;desktopLut=value.lut;desktopLutName=value.label;activeEffectsPreset=0;
    // Keep the customer's current strength and bypass choice. The common pipeline
    // owns the LUT independently of the pack files or subsequent install/remove.
    PublishDesktopLook();
    if(!PackLibraryDialog::SaveSelection(activePackSelection))mainWindow->Notify("Pack effect applied, but its selection could not be saved.");
    else mainWindow->Notify(desktopLook.bypassed?"Pack effect loaded. Restore effects to see it.":"Pack colour effect applied to preview and camera output.");
}
void Application::ShowPackLibrary()
{
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)){mainWindow->Notify("Pack file operations are disabled in UI preview mode.");return;}
    PackLibraryDialog dialog(mainWindow,[this](const EffectPackLut& value){ApplyPackLut(value);},
        [this](const std::string& id,const std::string& version){
            if(activePackSelection.packId==id&&activePackSelection.version==version){
                ClearPackSelection();desktopLut.reset();desktopLutName.clear();PublishDesktopLook();
                mainWindow->Notify("Active pack removed. The built-in look and other studio effects are kept.");
            }
        });
    dialog.ShowModal();
}
void Application::RestorePackSelection()
{
    if(wxGetEnv("PRISMFORGE_UI_PREVIEW",nullptr)||closing)return;
    activePackSelection=PackLibraryDialog::ReadSavedSelection();
    if(activePackSelection.Empty())return;
    packRestoreEpoch=packSelectionEpoch;
    packRestoreResult=std::make_shared<EffectPackRestoreResult>();
    auto result=packRestoreResult;auto selection=activePackSelection;auto root=PackLibraryDialog::LibraryRoot();
    try{
        packRestoreWorker=std::thread([result,selection,root]{
            try{result->value=PackLibraryDialog::LoadSelection(root,selection,result->error);}
            catch(const std::exception& e){result->error=e.what();}
            catch(...){result->error="Unexpected pack restore failure";}
            result->done.store(true,std::memory_order_release);
        });
        packRestoreTimer.SetOwner(this);
        Bind(wxEVT_TIMER,[this](wxTimerEvent&){PollPackRestore();},packRestoreTimer.GetId());
        if(!packRestoreTimer.Start(50)){
            logger<<"[PACK] Restore timer unavailable; waiting for owned verification"<<std::endl;
            if(packRestoreWorker.joinable())packRestoreWorker.join();
            PollPackRestore();
        }
    }catch(const std::exception& e){packRestoreResult.reset();mainWindow->Notify("Saved pack effect could not be restored: "+wxString::FromUTF8(e.what()));}
}
void Application::PollPackRestore()
{
    if(!packRestoreResult||!packRestoreResult->done.load(std::memory_order_acquire))return;
    packRestoreTimer.Stop();if(packRestoreWorker.joinable())packRestoreWorker.join();
    auto result=std::move(packRestoreResult);
    if(closing||!mainWindow||packRestoreEpoch!=packSelectionEpoch)return;
    if(result->value.lut){desktopLut=result->value.lut;desktopLutName=result->value.label;PublishDesktopLook();}
    else{activePackSelection={};mainWindow->Notify("Saved pack effect unavailable. Open Effects packs to repair or reinstall it. "+wxString::FromUTF8(result->error));}
}
