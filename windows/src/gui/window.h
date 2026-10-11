#pragma once

#include <wx/wx.h>
#include <wx/taskbar.h>
#include <map>
#include <wx/scrolwin.h>
#include <wx/timer.h>
#include "rtsp/streamoptions.h"
#include "capture/systemcamera.h"

#include "canvas.h"
#include "net/server.h"
#include "gui/studio.h"
#include "gui/lightpositionpad.h"
#include "effects/lighting/studiolighting.h"
#include "effects/overlays/face_filter_asset.h"
#include "effects/overlays/model_performance.h"
#include <array>

class Window;

class VCamTaskBarIcon : public wxTaskBarIcon
{
public:
	enum
	{
		ID_TRAY_OPEN = 10001,
		ID_TRAY_HIDE,
		ID_TRAY_ABOUT,
		ID_TRAY_EXIT
	};

	VCamTaskBarIcon(Window* window);
	virtual wxMenu* CreatePopupMenu() override;

private:
	Window* parentWindow;

	void OnLeftClick(wxTaskBarIconEvent& event);
	void OnMenuOpen(wxCommandEvent& event);
	void OnMenuHide(wxCommandEvent& event);
	void OnMenuAbout(wxCommandEvent& event);
	void OnMenuExit(wxCommandEvent& event);
};

class Window : public wxFrame
{
public:
	struct MenuIDs
	{
		static const int QR = 100;
		static const int DEVICES = 101;
		static const int HIDE2TRAY = 102;
		static const int SHOWSTATS = 103;
		static const int SAVESTATE = 104;
		static const int DS_SD = 105;
		static const int DS_HD = 106;
		static const int DS_FHD = 107;
		static const int DS_QHD = 108;
		static const int MINIMIZE_ON_CLOSE = 109;
		static const int VCAM_DYNAMIC = 110;
		static const int VCAM_ALWAYS = 111;
		static const int VCAM_DISABLED = 112;
	};

    enum StudioIDs { COMMANDS = 220, CONTROLS, SNAPSHOTS, RECONNECT, QUALITY_CHANGED, PRESET_APPLY,
        PRESET_SAVE, PRESET_IMPORT, PRESET_EXPORT, PRESET_DELETE, DIAGNOSTICS, FAVORITE,
        THEME_SYSTEM, THEME_DARK, THEME_LIGHT, THEME_GLASS, SNAPSHOT_ACTION, FOCUS_PREVIEW, DEBUG_FORCE_REPAINT, LOOK_CHANGED,
        LOOK_BYPASS, LOOK_PRESET_APPLY, LOOK_PRESET_SAVE, LOOK_PRESET_IMPORT, LOOK_PRESET_EXPORT, LOOK_PRESET_DELETE, LUT_IMPORT, LUT_CLEAR, LUT_INTENSITY, STUDIO_LIGHT_MODE, STUDIO_LIGHT_STRENGTH, STUDIO_LIGHT_DIRECTION, STUDIO_LIGHT_SCOPE, STUDIO_LIGHT_POSITION, STUDIO_KEY_INTENSITY, STUDIO_KEY_TEMPERATURE, STUDIO_FILL_INTENSITY, STUDIO_FILL_TEMPERATURE, STUDIO_FRAME_CHANGED,
        BACKGROUND_MODE, BACKGROUND_QUALITY, BACKGROUND_BLUR, BACKGROUND_IMPORT, BACKGROUND_CLEAR, BACKGROUND_COLOUR, EFFECT_PRESET_APPLY,
        TOUCHUP_STRENGTH, TOUCHUP_DETAIL, ACCESSORY_GLASSES, ACCESSORY_CAP, ACCESSORY_SCALE, ACCESSORY_CAP_SCALE, ACCESSORY_CAP_HEIGHT, PACK_LIBRARY, FACE_FILTER_STYLE, FACE_FILTER_SCALE, FACE_FILTER_HEIGHT, FACE_FILTER_IMPORT, FACE_FILTER_REMOVE, FACE_FILTER_CUSTOM_FIT, FACE_FILTER_CUSTOM_RESET, FACE_FILTER_PERFORMANCE };
    Window(Server::HostInfo hostInfo);
    void SetDeviceState(const DeviceDescriptor* device, const StreamOptions* options);
    void SetSystemCameraState(const wxString& name,
        const std::vector<PrismForge::Capture::CaptureMode>& modes, int selectedMode,
        const PrismForge::Capture::CaptureStatus& status);
    bool VerifySourceControls();
    void BeginStream();
    void SetStreamStats(int width, int height, int fps, double bitrate);
    void SetOutputStatus(const wxString& text);
    void Notify(const wxString& text);
    void SaveGeometry();
    void ShowCommands();
    void ShowAppearance();
    void BuildRail(wxBoxSizer* root);
    void ToggleInspector();
    void ApplyTheme();
    void UpdateInspectorLayout();
    enum { REFRESH_SOURCES = 24001 };
    bool VerifyInspectorScrolling();
    bool VerifyControlsLayout();
    bool VerifyBackgroundControls();
    Studio::GlassSelect* GetBackgroundChoice() { return backgroundChoice; }
    Studio::GlassSelect* GetBackgroundQualityChoice() { return backgroundQualityChoice; }
    Studio::GlassSlider* GetBackgroundBlurSlider() { return backgroundBlurSlider; }
    Studio::GlassSelect* GetEffectsPresetChoice() { return effectsPresetChoice; }
    wxColour GetBackgroundColour() const { return backgroundColour; }
    void SetBackgroundPresentation(int mode, int blur, const wxString& imageName,
        const wxColour& colour, bool bypassed, bool highQuality = true);
    void SetSegmentationStatus(const wxString& status, const wxString& detail = {});
    void SetEffectsPresetPresentation(int selection);
    void ForceFullRepaint();
    Studio::GlassSelect* GetLookChoice() { return lookChoice; }
    Studio::GlassSelect* GetLookPresetChoice() { return lookPresetChoice; }
    Studio::SmoothButton* GetLookBypassButton() { return lookBypassButton; }
    void SetLookPresentation(int selection, bool bypassed);
    Studio::GlassSlider* GetLutIntensitySlider() { return lutIntensitySlider; }
    Studio::SmoothButton* GetLutClearButton() { return lutClearButton; }
    void SetLutPresentation(const wxString& name, int intensity);
    Studio::GlassSelect* GetStudioLightChoice() { return studioLightChoice; }
    Studio::GlassSelect* GetStudioLightDirectionChoice() { return studioLightDirectionChoice; }
    Studio::GlassSlider* GetStudioLightStrengthSlider() { return studioLightStrengthSlider; }
    Studio::GlassSelect* GetStudioFrameChoice() { return studioFrameChoice; }
    void SetStudioPresentation(int mode,int direction,int strength,bool frame,bool bypassed);
    void SetCustomLightingPresentation(const PrismForge::Effects::Lighting::StudioLight& key,
        const PrismForge::Effects::Lighting::StudioLight& fill, bool personOnly, bool bypassed, bool faceLighting = false);
    bool VerifyCustomLightingControls();
    void SetFaceLightingStatus(const wxString& status, const wxString& detail = {});
    bool VerifyFaceLightingStatus();
    Studio::GlassSlider* GetTouchupStrengthSlider() { return touchupStrengthSlider; }
    Studio::GlassSlider* GetTouchupDetailSlider() { return touchupDetailSlider; }
    void SetTouchupPresentation(float strength, float detail, bool bypassed);
    void SetTouchupStatus(const wxString& status, const wxString& detail = {});
    bool VerifyTouchupControls();
    Studio::GlassSelect* GetAccessoryGlassesChoice() { return accessoryGlassesChoice; }
    Studio::GlassSelect* GetAccessoryCapChoice() { return accessoryCapChoice; }
    Studio::GlassSlider* GetAccessoryScaleSlider() { return accessoryScaleSlider; }
    Studio::GlassSlider* GetCapScaleSlider() { return accessoryCapScaleSlider; }
    Studio::GlassSlider* GetCapHeightSlider() { return accessoryCapHeightSlider; }
    void SetAccessoryPresentation(bool glasses, bool cap, float scale, bool bypassed, int capStyle = 2,
        int glassesStyle = 0, float capScale = 1.f, float capVerticalOffset = 0.f);
    void SetAccessoryStatus(const wxString& status, const wxString& detail = {});
    bool VerifyAccessoryControls();
    Studio::GlassSelect* GetFaceFilterChoice() { return faceFilterChoice; }
    Studio::GlassSlider* GetFaceFilterScaleSlider() { return faceFilterScaleSlider; }
    Studio::GlassSlider* GetFaceFilterHeightSlider() { return faceFilterHeightSlider; }
    void SetFaceFilterPresentation(int style, float scale, float verticalOffset, bool bypassed);
    void SetLocalFaceFilterPresentation(const wxString& name, const PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit& fit, bool available, bool busy, bool bypassed);
    PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit GetLocalFaceFilterFit() const;
    void SetModelPerformancePresentation(const PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings& settings,
        const wxString& summary, bool busy, bool bypassed);
    PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings GetModelPerformanceSettings() const;
    void SetFaceFilterStatus(const wxString& status, const wxString& detail = {});
    bool VerifyFaceFilterControls();
    uint64_t GetInspectorLayoutRevisionForTesting() const { return inspectorLayoutRevision; }
    Studio::GlassSelect* GetStudioLightScopeChoice() { return studioLightScopeChoice; }
    Studio::LightPositionPad* GetStudioLightPositionPad() { return studioLightPositionPad; }
    Studio::GlassSlider* GetStudioKeyIntensitySlider() { return studioKeyIntensitySlider; }
    Studio::GlassSlider* GetStudioFillIntensitySlider() { return studioFillIntensitySlider; }
    Studio::GlassSlider* GetStudioKeyTemperatureSlider() { return studioKeyTemperatureSlider; }
    Studio::GlassSlider* GetStudioFillTemperatureSlider() { return studioFillTemperatureSlider; }
    Studio::GlassSelect* GetPresetChoice() { return presetChoice; }
    Studio::GlassSelect* GetResolutionChoice() { return resolutionChoice; }
    Studio::GlassSelect* GetFpsChoice() { return fpsChoice; }
    Studio::GlassSlider* GetBitrateSlider() { return bitrateSlider; }
    std::vector<DeviceDescriptor::Resolution> resolutions;
    void Dispatch(int id);

	~Window();

	void HideToTray();
	void RestoreFromTray();

	Canvas* GetCanvas();
	Studio::GlassSelect* GetSourceChoice();
	wxButton* GetStreamOptionsButton();

	// Bottom Bar Controls
	wxButton* GetRotateLeftButton();
	wxButton* GetRotateRightButton();
	wxButton* GetFlipButton();
	wxButton* GetFlipVerticalButton();

	wxButton* GetZoomInButton();
	wxButton* GetZoomOutButton();
	Studio::GlassLabel* GetZoomLevelLabel();

	wxButton* GetTorchButton();
	wxButton* GetSwapButton();

	wxButton* GetAdjustmentsButton();
	wxButton* GetSnapshotButton();

	Studio::GlassLabel* GetStatsText();
	wxTaskBarIcon* GetTaskbarIcon();

private:
    wxPanel *shell = nullptr, *rail = nullptr;
    Studio::Card* previewPane = nullptr;
    Studio::GlassScrolledPanel* inspector = nullptr;
    wxBoxSizer* contentSizer = nullptr;
    Studio::GlassSelect *presetChoice = nullptr, *resolutionChoice = nullptr, *fpsChoice = nullptr, *outputChoice = nullptr;
    Studio::GlassSlider* bitrateSlider = nullptr;
    Studio::GlassSelect* lookChoice = nullptr;
    Studio::GlassSelect* lookPresetChoice = nullptr;
    Studio::SmoothButton* lookBypassButton = nullptr;
    Studio::Section* lookSection = nullptr;
    Studio::Section* studioEffectSection = nullptr;
    Studio::Section* backgroundSection = nullptr;
    Studio::Section* touchupSection = nullptr;
    Studio::GlassSlider *touchupStrengthSlider = nullptr, *touchupDetailSlider = nullptr;
    Studio::GlassLabel *touchupStrengthLabel = nullptr, *touchupDetailLabel = nullptr, *touchupStatusLabel = nullptr;
    wxString touchupStatus, touchupStatusDetail;
    Studio::Section* accessorySection = nullptr;
    Studio::GlassSelect *accessoryGlassesChoice = nullptr, *accessoryCapChoice = nullptr;
    int accessoryCapStyle = 2, accessoryGlassesStyle = 0;
    Studio::GlassSlider *accessoryScaleSlider = nullptr, *accessoryCapScaleSlider = nullptr, *accessoryCapHeightSlider = nullptr;
    Studio::GlassLabel *accessoryScaleLabel = nullptr, *accessoryCapScaleLabel = nullptr, *accessoryCapHeightLabel = nullptr,
        *accessoryStatusLabel = nullptr;
    wxString accessoryStatus, accessoryStatusDetail;
    Studio::Section* faceFilterSection = nullptr;
    Studio::GlassSelect* faceFilterChoice = nullptr;
    Studio::GlassSlider *faceFilterScaleSlider = nullptr, *faceFilterHeightSlider = nullptr;
    Studio::GlassLabel *faceFilterScaleLabel = nullptr, *faceFilterHeightLabel = nullptr, *faceFilterStatusLabel = nullptr;
    wxString faceFilterStatus, faceFilterStatusDetail;
    Studio::SmoothButton *faceFilterImportButton=nullptr,*faceFilterRemoveButton=nullptr,*faceFilterResetButton=nullptr;
    Studio::GlassLabel* faceFilterLocalName=nullptr;
    wxPanel* faceFilterCustomPanel=nullptr;
    std::array<Studio::GlassSlider*,6> faceFilterCustomSliders{};
    std::array<Studio::GlassLabel*,6> faceFilterCustomLabels{};
    Studio::GlassSelect* faceFilterOcclusionChoice=nullptr;
    bool faceFilterLocalAvailable=false,faceFilterImportBusy=false;
    Studio::GlassSelect* modelPerformanceChoice=nullptr;
    Studio::GlassLabel* modelPerformanceSummary=nullptr;
    wxPanel* modelPerformanceCustomPanel=nullptr;
    std::array<Studio::GlassSlider*,4> modelPerformanceSliders{};
    std::array<Studio::GlassLabel*,4> modelPerformanceLabels{};
    bool modelPerformanceBusy=false;

    Studio::GlassSelect *backgroundChoice = nullptr, *backgroundQualityChoice = nullptr, *effectsPresetChoice = nullptr;
    Studio::GlassSlider* backgroundBlurSlider = nullptr;
    Studio::GlassLabel *backgroundBlurLabel = nullptr, *backgroundImageLabel = nullptr,
        *segmentationStatusLabel = nullptr, *segmentationDetailLabel = nullptr;
    Studio::SmoothButton *backgroundLoadButton = nullptr, *backgroundClearButton = nullptr,
        *backgroundColourButton = nullptr;
    wxColour backgroundColour = wxColour(36, 48, 75);
    wxString backgroundImageName, segmentationStatus, segmentationDetail;
    bool backgroundBypassed = false;
    Studio::GlassSelect *studioLightChoice = nullptr, *studioLightDirectionChoice = nullptr, *studioFrameChoice = nullptr;
    Studio::GlassSlider* studioLightStrengthSlider = nullptr;
    Studio::GlassSelect* studioLightScopeChoice = nullptr;
    wxPanel* customLightPanel = nullptr;
    Studio::LightPositionPad* studioLightPositionPad = nullptr;
    Studio::GlassSlider *studioKeyIntensitySlider = nullptr, *studioFillIntensitySlider = nullptr,
        *studioKeyTemperatureSlider = nullptr, *studioFillTemperatureSlider = nullptr;
    Studio::GlassLabel *studioKeyIntensityLabel = nullptr, *studioFillIntensityLabel = nullptr,
        *studioKeyTemperatureLabel = nullptr, *studioFillTemperatureLabel = nullptr;
    Studio::GlassLabel* studioLightStrengthLabel = nullptr;
    Studio::GlassLabel* faceLightingStatusLabel = nullptr;
    wxString faceLightingStatus, faceLightingDetail;
    Studio::GlassSlider* lutIntensitySlider = nullptr;
    Studio::GlassLabel *lutNameLabel = nullptr, *lutIntensityLabel = nullptr;
    Studio::SmoothButton* lutClearButton = nullptr;
    Studio::GlassLabel *connectionLabel = nullptr, *deviceLabel = nullptr, *outputLabel = nullptr, *noticeLabel = nullptr, *bitrateLabel = nullptr;
    wxButton *controlsButton = nullptr, *favoriteButton = nullptr;
    Studio::LiveChip* liveChip = nullptr;
    Studio::StatusDot* statusDot = nullptr;
    Studio::SmoothButton* connectButton = nullptr;
    Studio::SmoothButton* themeButton = nullptr;
    Studio::Section *profileSection = nullptr, *captureSection = nullptr, *bandwidthSection = nullptr, *framingSection = nullptr, *outputSection = nullptr;
    Studio::SegmentedControl *resolutionSeg = nullptr, *fpsSeg = nullptr;
    Studio::GlassSlider *blurSlider = nullptr, *opacitySlider = nullptr;
    Studio::GlassLabel *blurValueLabel = nullptr, *opacityValueLabel = nullptr;
    uint64_t inspectorLayoutRevision = 0;
    wxTimer statusTimer;
    wxLongLong lastFrameTime = 0;
    bool systemInput = false;
    bool connected = false, inspectorRequested = true, compact = false;
    std::vector<wxWindow*> cameraControls;
    void BuildInspector();
    void AdaptLayout();
    void Tick(wxTimerEvent&);
    VCamTaskBarIcon* taskbarIcon = nullptr;
	Canvas* canvas;

	Studio::GlassSelect* sourceChoice = nullptr;
	wxButton* streamOptionsButton;

	// Bottom Bar
	wxButton* rotateLeftButton;
	wxButton* rotateRightButton;
	wxButton* flipButton;
	wxButton* flipVerticalButton;

	wxButton* zoomInButton;
	wxButton* zoomOutButton;
	Studio::GlassLabel* zoomLevelLabel;

	wxButton* torchButton;
	wxButton* swapButton;

	wxButton* adjustmentsButton;
	wxButton* snapshotButton;

	Studio::GlassLabel* statsText;

	void InitializeMenu(Server::HostInfo hostinfo);
	void InitializeTopBar(wxPanel* parent, wxBoxSizer* topsizer);
	void InitializeCanvasPanel(wxPanel* parent, wxBoxSizer* topsizer);
	void InitializeBottomBar(wxPanel* parent, wxBoxSizer* topsizer);

	void MinimizeToTaskbar(wxIconizeEvent& evt);
	void MaximizeFromTaskbar(wxTaskBarIconEvent& evt);
};