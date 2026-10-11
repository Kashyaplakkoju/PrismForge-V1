#pragma once

#include <wx/wx.h>

#include "gui/window.h"
#include "net/server.h"
#include "rtsp/manager.h"
#include "directshowsource.h"
#include "rtsp/streamoptions.h"
#include "snapshotmanager.h"
#include "gui/presets.h"
#include "gui/lookpresets.h"
#include "gui/packlibrarydialog.h"
#include "effects/desktopstudiopipeline.h"
#include "effects/overlays/local_face_filter_import.h"
#include "capture/inputsession.h"
#include "video/latestframedelivery.h"
#include "capture/systemcamera.h"
#include <optional>
#include <array>
#include <chrono>

class Application : public wxApp, public Server::ConnectionListener
{
public:
	Application();

	virtual bool OnInit();
	virtual int OnExit() override;
	
	virtual void OnDeviceConnected(DeviceDescriptor& descriptor) const override;
	virtual void OnDeviceDisconnected(DeviceDescriptor& descriptor) const override;
	virtual void OnDeviceErrorReported(DeviceDescriptor& descriptor, const Connection::ErrorReport& error) const override;

private:
	Window* mainWindow = nullptr;
    std::atomic<bool> closing{false};
    int uiVerificationExitCode = 0;
    PresetStore presets;
    LookPresetStore lookPresets;
    DesktopLookState desktopLook;
    std::shared_ptr<const PrismForge::Effects::CubeLut> desktopLut;
    wxString desktopLutName;
    int desktopLutIntensity = 100;
    EffectPackSelection activePackSelection;
    std::thread packRestoreWorker;
    wxTimer packRestoreTimer;
    std::shared_ptr<EffectPackRestoreResult> packRestoreResult;
    uint64_t packSelectionEpoch=0, packRestoreEpoch=0;
    void RestorePackSelection();
    void PollPackRestore();
    void ClearPackSelection();
    void ApplyPackLut(const EffectPackLut& value);
    void ShowPackLibrary();
    PrismForge::Effects::DesktopStudioSettings desktopStudio;
    wxString backgroundImageName,backgroundImagePath;
    int activeEffectsPreset=0;
    wxTimer segmentationTimer;
    void PublishDesktopStudio();
    void ConfigureDesktopStudio();
    PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings modelPerformance;
    PrismForge::Effects::Overlays::Realistic::ModelHardwareResources modelHardware;
    PrismForge::Effects::Overlays::Realistic::ModelResourceBudget modelBudget;
    bool localModelBudgetHeld=false;
    wxString ModelPerformanceSummary() const;
    void RefreshSegmentationStatus();
    bool HandleBackgroundAction(wxCommandEvent& event);
    bool LoadBackgroundImage(const wxString& path);
    void SaveBackgroundImagePath();
    bool HandleDesktopStudioAction(wxCommandEvent& event);
    bool HandleTouchupAction(wxCommandEvent& event);
    bool HandleAccessoryAction(wxCommandEvent& event);
    bool VerifyAccessorySettings();
    bool HandleFaceFilterAction(wxCommandEvent& event);
    bool VerifyFaceFilterSettings();
    bool VerifyLocalFaceFilterWorker();
    struct LocalFaceFilterResult {
        std::atomic<bool> done{false},cancel{false};
        PrismForge::Effects::Overlays::Realistic::LocalFaceFilterImportResult value;
        std::string error;
        bool success=false,restore=false,remove=false;
        uint64_t epoch=0;
    };
    std::thread localFaceFilterWorker;
    wxTimer localFaceFilterTimer;
    std::shared_ptr<LocalFaceFilterResult> localFaceFilterResult;
    uint64_t localFaceFilterEpoch=0;
    bool localFaceFilterTimerBound=false;
    void RestoreLocalFaceFilter();
    void StartLocalFaceFilterOperation(const std::wstring& path,bool restore=false,bool remove=false);
    void PollLocalFaceFilter();
    void SaveLocalFaceFilterIdentity();
    void RefreshLocalFaceFilterPresentation();

    bool HandleDesktopLookAction(wxCommandEvent& event);
    void PublishDesktopLook();
    void RefreshLookPresets();
    bool HandleStudioAction(wxCommandEvent& event);
    void RefreshStudio();
    void RefreshPresets();
    void ApplyQuality(const QualityPreset& preset);

	StreamOptions::Registry stateRegistry;

	std::unique_ptr<Server> server;
	std::unique_ptr<RTSP::Manager> rtspManager;
	std::unique_ptr<DirectShowSource> dsSource;
	SnapshotManager snapshotManager;
    PrismForge::Effects::DesktopStudioPipeline effectsPipeline;
    PrismForge::Input::SessionGate inputGate;
    PrismForge::Video::LatestFrameDelivery frameDelivery;
    std::atomic<int> deliveredFpsEstimate{0};
    // Serialized by inputGate; bounded per-source live processing diagnostics.
    uint64_t perfGeneration=0;
    std::vector<double> perfFrames;
    std::array<double,120> perfWaits{};
    double perfEffects=0,perfPreview=0,perfOutput=0,perfMf=0,perfDs=0;
    std::chrono::steady_clock::time_point perfWallStart;

    PrismForge::Input::Kind sourceKind = PrismForge::Input::Kind::None;
    std::string activeSourceKey;
    bool allowPhoneAutoSelect = true;
    std::vector<PrismForge::Input::Choice> sourceChoices;
    std::vector<PrismForge::Capture::CaptureDevice> systemDevices;
    std::optional<PrismForge::Capture::CaptureDevice> systemDevice;
    std::optional<PrismForge::Capture::CaptureMode> selectedSystemMode;
    std::vector<PrismForge::Capture::CaptureMode> systemModes;
    PrismForge::Capture::CaptureStatus systemStatus;
    std::unique_ptr<PrismForge::Capture::SystemCameraSource> systemCamera;
    std::thread cameraModesWorker;
    struct CameraFrameStats {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        uint64_t frames = 0;
    };
    void StopInputs();
    void DeliverInputFrame(uint64_t token, AVFrame* frame);
    void ProcessInputFrame(uint64_t token, AVFrame* frame, double queueWaitMs);
    void RefreshSystemCameras();
    void StartSystemCamera(const PrismForge::Capture::CaptureDevice& device,
        const PrismForge::Capture::CaptureMode* requested = nullptr);

	StreamOptions& GetCurrentDeviceStreamOptions();
	void BindEventListeners();

	void UpdateAvailableDevices();

	void OnSourceChanged(wxEvent& event);

	void ShowAdjustmentsDialog(wxCommandEvent& event);
	void ShowStreamConfigDialog(wxCommandEvent& event);
	
	void OnMenuEvent(wxCommandEvent& event);
	void OnWindowCloseEvent(wxCloseEvent& event);

	void EnsureStateInitialized(std::string name, const DeviceDescriptor& descriptor);
};