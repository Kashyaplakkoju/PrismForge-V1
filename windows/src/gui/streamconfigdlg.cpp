#include "gui/streamconfigdlg.h"
#include <wx/statline.h>
#include "gui/studio.h"

#include "rtsp/manager.h"

wxDEFINE_EVENT(EVT_STREAM_CONFIG_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_STREAM_RESOLUTION_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_STREAM_FPS_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_STREAM_BITRATE_CHANGED, wxCommandEvent);

StreamConfigDlg::StreamConfigDlg(wxWindow* parent,
    const DeviceDescriptor& device,
    bool isBackCamera,
    const Config& currentConfig)
    : Studio::GlassDialogBase(parent, wxID_ANY, "Camera settings", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---------------------------------------------------------
    // GROUP 1: QUALITY SETTINGS
    // ---------------------------------------------------------
    auto qualityBox = new wxStaticBoxSizer(wxVERTICAL, this, "Video Quality");
    // Increased row count for the expanded bitrate section
    auto qualityGrid = new wxFlexGridSizer(4, 2, 10, 15);
    qualityGrid->AddGrowableCol(1, 1);

    // 1. Resolution
    qualityGrid->Add(new wxStaticText(qualityBox->GetStaticBox(), wxID_ANY, "Resolution:"), 0, wxALIGN_CENTER_VERTICAL);
    resChoice = new wxChoice(qualityBox->GetStaticBox(), wxID_ANY);
    const auto& resolutions = isBackCamera ? device.backResolutions() : device.frontResolutions();
    for (const auto& res : resolutions) 
    {
        resChoice->Append(wxString::Format("%d x %d", res.first, res.second));
    }
    if (currentConfig.resIndex >= 0 && currentConfig.resIndex < (int)resChoice->GetCount())
        resChoice->SetSelection(currentConfig.resIndex);
    else if (resChoice->GetCount() > 0)
        resChoice->SetSelection(0);
    resChoice->Bind(wxEVT_CHOICE, &StreamConfigDlg::OnResolutionChanged, this);
    qualityGrid->Add(resChoice, 1, wxEXPAND);

    // 2. Framerate
    qualityGrid->Add(new wxStaticText(qualityBox->GetStaticBox(), wxID_ANY, "Framerate:"), 0, wxALIGN_CENTER_VERTICAL);
    fpsChoice = new wxChoice(qualityBox->GetStaticBox(), wxID_ANY);
    std::vector<int> fpsOptions = { 15, 24, 30, 60 };
    int fpsSel = 2;
    for (size_t i = 0; i < fpsOptions.size(); i++) 
    {
        fpsChoice->Append(std::to_string(fpsOptions[i]) + " FPS");
        if (fpsOptions[i] == currentConfig.fps) fpsSel = i;
    }
    fpsChoice->SetSelection(fpsSel);
    fpsChoice->Bind(wxEVT_CHOICE, &StreamConfigDlg::OnFpsChanged, this);
    qualityGrid->Add(fpsChoice, 1, wxEXPAND);


    // ---------------------------------------------------------
    // 3. BITRATE
    // ---------------------------------------------------------
    qualityGrid->Add(new wxStaticText(qualityBox->GetStaticBox(), wxID_ANY, "Bitrate Mode:"), 0, wxALIGN_TOP | wxTOP, 5);

    auto bitrateContainer = new wxBoxSizer(wxVERTICAL);

    // A. Radio Buttons
    auto radioSizer = new wxBoxSizer(wxHORIZONTAL);
    rbStatic = new wxRadioButton(qualityBox->GetStaticBox(), wxID_ANY, "Static (Constant)", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    rbAdaptive = new wxRadioButton(qualityBox->GetStaticBox(), wxID_ANY, "Adaptive (Variable)");

    rbStatic->SetValue(!currentConfig.adaptiveBitrate);
    rbAdaptive->SetValue(currentConfig.adaptiveBitrate);

    rbStatic->Bind(wxEVT_RADIOBUTTON, &StreamConfigDlg::OnBitrateModeChanged, this);
    rbAdaptive->Bind(wxEVT_RADIOBUTTON, &StreamConfigDlg::OnBitrateModeChanged, this);

    radioSizer->Add(rbStatic, 0, wxRIGHT, 15);
    radioSizer->Add(rbAdaptive, 0, wxRIGHT, 0);
    bitrateContainer->Add(radioSizer, 0, wxBOTTOM, 10);

    // B. Static Panel (Single Slider)
    staticBitratePanel = new wxPanel(qualityBox->GetStaticBox(), wxID_ANY);
    auto staticSizer = new wxBoxSizer(wxHORIZONTAL);

    int defStatic = currentConfig.bitrate > 0 ? currentConfig.bitrate : RTSP::DEFAULT_STATIC_BITRATE;

    staticLabel = new wxStaticText(staticBitratePanel, wxID_ANY, FormatBitrate(defStatic), wxDefaultPosition, wxSize(70, -1), wxALIGN_RIGHT);
    staticSlider = new wxSlider(staticBitratePanel, wxID_ANY, defStatic, RTSP::MIN_BITRATE, RTSP::MAX_BITRATE);
    staticSlider->Bind(wxEVT_SLIDER, &StreamConfigDlg::OnBitrateSliderChanged, this);

    staticSizer->Add(staticSlider, 1, wxEXPAND | wxRIGHT, 5);
    staticSizer->Add(staticLabel, 0, wxALIGN_CENTER_VERTICAL);
    staticBitratePanel->SetSizer(staticSizer);

    bitrateContainer->Add(staticBitratePanel, 1, wxEXPAND);

    // C. Adaptive Panel (Min/Max Sliders)
    adaptiveBitratePanel = new wxPanel(qualityBox->GetStaticBox(), wxID_ANY);
    auto adaptiveSizer = new wxFlexGridSizer(2, 3, 5, 5); // Label | Slider | Value
    adaptiveSizer->AddGrowableCol(1, 1);

    // Defaults: Min=1000, Max=5000
    int defMin = currentConfig.minBitrate > 0 ? currentConfig.minBitrate : RTSP::DEFAULT_MIN_BITRATE;
    int defMax = currentConfig.maxBitrate > 0 ? currentConfig.maxBitrate : RTSP::DEFAULT_MAX_BITRATE;

    // Min Row
    adaptiveSizer->Add(new wxStaticText(adaptiveBitratePanel, wxID_ANY, "Min:"), 0, wxALIGN_CENTER_VERTICAL);
    minSlider = new wxSlider(adaptiveBitratePanel, wxID_ANY, defMin, RTSP::MIN_BITRATE, RTSP::MAX_BITRATE);
    minLabel = new wxStaticText(adaptiveBitratePanel, wxID_ANY, FormatBitrate(defMin), wxDefaultPosition, wxSize(70, -1), wxALIGN_RIGHT);
    minSlider->Bind(wxEVT_SLIDER, &StreamConfigDlg::OnBitrateSliderChanged, this);
    adaptiveSizer->Add(minSlider, 1, wxEXPAND);
    adaptiveSizer->Add(minLabel, 0, wxALIGN_CENTER_VERTICAL);

    // Max Row
    adaptiveSizer->Add(new wxStaticText(adaptiveBitratePanel, wxID_ANY, "Max:"), 0, wxALIGN_CENTER_VERTICAL);
    maxSlider = new wxSlider(adaptiveBitratePanel, wxID_ANY, defMax, RTSP::MIN_BITRATE, RTSP::MAX_BITRATE);
    maxLabel = new wxStaticText(adaptiveBitratePanel, wxID_ANY, FormatBitrate(defMax), wxDefaultPosition, wxSize(70, -1), wxALIGN_RIGHT);
    maxSlider->Bind(wxEVT_SLIDER, &StreamConfigDlg::OnBitrateSliderChanged, this);
    adaptiveSizer->Add(maxSlider, 1, wxEXPAND);
    adaptiveSizer->Add(maxLabel, 0, wxALIGN_CENTER_VERTICAL);

    adaptiveBitratePanel->SetSizer(adaptiveSizer);
    bitrateContainer->Add(adaptiveBitratePanel, 1, wxEXPAND);

    qualityGrid->Add(bitrateContainer, 1, wxEXPAND);



    // 4. Codec
    qualityGrid->Add(new wxStaticText(qualityBox->GetStaticBox(), wxID_ANY, "Codec:"), 0, wxALIGN_CENTER_VERTICAL);
    codecChoice = new wxChoice(qualityBox->GetStaticBox(), wxID_ANY);
    codecChoice->Append("H.264 (Standard)");
    codecChoice->Append("H.265 (HEVC)");
    codecChoice->SetSelection(currentConfig.h265Enabled ? 1 : 0);
    codecChoice->Bind(wxEVT_CHOICE, &StreamConfigDlg::OnSimpleSettingChanged, this);
    qualityGrid->Add(codecChoice, 1, wxEXPAND);

    qualityBox->Add(qualityGrid, 1, wxEXPAND | wxALL, 10);
    mainSizer->Add(qualityBox, 0, wxEXPAND | wxALL, 10);


    // ---------------------------------------------------------
    // GROUP 2: CAMERA & HARDWARE
    // ---------------------------------------------------------
    auto hwBox = new wxStaticBoxSizer(wxVERTICAL, this, "Camera Settings");
    auto hwGrid = new wxFlexGridSizer(3, 2, 10, 15);
    hwGrid->AddGrowableCol(1, 1);

    // 1. Focus
    hwGrid->Add(new wxStaticText(hwBox->GetStaticBox(), wxID_ANY, "Focus Mode:"), 0, wxALIGN_CENTER_VERTICAL);
    focusChoice = new wxChoice(hwBox->GetStaticBox(), wxID_ANY);
    focusChoice->Append("Continuous Video (Auto)");
    focusChoice->Append("Fixed / Infinity");
    int focusSel = (currentConfig.focusMode == 1) ? 1 : 0;
    focusChoice->SetSelection(focusSel);
    focusChoice->Bind(wxEVT_CHOICE, &StreamConfigDlg::OnSimpleSettingChanged, this);
    hwGrid->Add(focusChoice, 1, wxEXPAND);

    // 2. Stabilization
    hwGrid->Add(new wxStaticText(hwBox->GetStaticBox(), wxID_ANY, "Stabilization:"), 0, wxALIGN_CENTER_VERTICAL);
    stabilizationCheck = new wxCheckBox(hwBox->GetStaticBox(), wxID_ANY, "Enable EIS");
    stabilizationCheck->SetValue(currentConfig.stabilizationEnabled);
    stabilizationCheck->Bind(wxEVT_CHECKBOX, &StreamConfigDlg::OnSimpleSettingChanged, this);
    hwGrid->Add(stabilizationCheck, 0, wxALIGN_CENTER_VERTICAL);

    // 3. Flash
    hwGrid->Add(new wxStaticText(hwBox->GetStaticBox(), wxID_ANY, "Flashlight:"), 0, wxALIGN_CENTER_VERTICAL);
    flashCheck = new wxCheckBox(hwBox->GetStaticBox(), wxID_ANY, "Enable Torch");
    flashCheck->SetValue(currentConfig.flashEnabled);
    flashCheck->Bind(wxEVT_CHECKBOX, &StreamConfigDlg::OnSimpleSettingChanged, this);
    hwGrid->Add(flashCheck, 0, wxALIGN_CENTER_VERTICAL);

    hwBox->Add(hwGrid, 1, wxEXPAND | wxALL, 10);
    mainSizer->Add(hwBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // ---------------------------------------------------------
    // BUTTONS
    // ---------------------------------------------------------
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto closeBtn = Studio::Button(this, "Close", "Close settings", true, wxID_OK);
    closeBtn->SetDefault();
    btnSizer->Add(closeBtn, 0);
    mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, FromDIP(14));

    SetSizerAndFit(mainSizer);
    Studio::Dialog(this);
    SetMinSize(GetSize());
    Studio::Help(resChoice, "Capture resolution", "Higher resolution needs more bandwidth and phone processing power.");
    Studio::Help(fpsChoice, "Capture frame rate", "Requested frame rate; performance depends on the phone and receiving app.");
    Studio::Help(codecChoice, "Video codec", "H.264 is widely supported. H.265 needs a compatible phone encoder.");
    Studio::Help(focusChoice, "Camera focus", "Use continuous autofocus for calls or fixed focus for a steady scene.");
    Studio::Help(stabilizationCheck, "Stabilization", "Reduce camera shake when supported by the phone.");
    Studio::Help(flashCheck, "Torch", "Illuminate the scene using the rear camera torch.");
    Studio::Help(staticSlider, "Constant bitrate", "More bandwidth improves detail. Changes apply immediately.");
    Studio::Help(minSlider, "Minimum adaptive bitrate", "Lower bound for bandwidth adaptation.");
    Studio::Help(maxSlider, "Maximum adaptive bitrate", "Upper bound for bandwidth adaptation.");
    CenterOnParent();
    UpdateBitrateUI();
}

StreamConfigDlg::~StreamConfigDlg() {}

wxString StreamConfigDlg::FormatBitrate(int kbps)
{
    if (kbps >= 1000) return wxString::Format("%.1f Mbps", kbps / 1000.0f);
    return wxString::Format("%d Kbps", kbps);
}

// --- Accessors for Parent ---
bool StreamConfigDlg::IsAdaptiveBitrate() const { return rbAdaptive->GetValue(); }
int StreamConfigDlg::GetStaticBitrate() const { return staticSlider->GetValue(); }
int StreamConfigDlg::GetMinBitrate() const { return minSlider->GetValue(); }
int StreamConfigDlg::GetMaxBitrate() const { return maxSlider->GetValue(); }

// --- Event Handlers ---

void StreamConfigDlg::UpdateBitrateUI()
{
    if (!rbAdaptive || !staticBitratePanel || !adaptiveBitratePanel)
        return;

    bool adaptive = rbAdaptive->GetValue();

    staticBitratePanel->Show(!adaptive);
    adaptiveBitratePanel->Show(adaptive);

    if (GetSizer()) {
        Layout();
        Fit();
    }
}

void StreamConfigDlg::OnBitrateModeChanged(wxCommandEvent& event)
{
    UpdateBitrateUI();

    // Notify parent that configuration structure changed
    wxCommandEvent evt(EVT_STREAM_BITRATE_CHANGED);
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

void StreamConfigDlg::OnBitrateSliderChanged(wxCommandEvent& event)
{
    // Update labels dynamically
    if (event.GetEventObject() == staticSlider) 
    {
        staticLabel->SetLabel(FormatBitrate(staticSlider->GetValue()));
    }
    else if (event.GetEventObject() == minSlider) 
    {
        // Validation: Min should not exceed Max
        if (minSlider->GetValue() > maxSlider->GetValue())
            maxSlider->SetValue(minSlider->GetValue());

        minLabel->SetLabel(FormatBitrate(minSlider->GetValue()));
        maxLabel->SetLabel(FormatBitrate(maxSlider->GetValue())); // Update Max too if forced
    }
    else if (event.GetEventObject() == maxSlider) 
    {
        // Validation: Max should not be lower than Min
        if (maxSlider->GetValue() < minSlider->GetValue())
            minSlider->SetValue(maxSlider->GetValue());

        maxLabel->SetLabel(FormatBitrate(maxSlider->GetValue()));
        minLabel->SetLabel(FormatBitrate(minSlider->GetValue())); // Update Min too if forced
    }

    // Fire generic event
    wxCommandEvent evt(EVT_STREAM_BITRATE_CHANGED);
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

void StreamConfigDlg::OnResolutionChanged(wxCommandEvent& event) 
{
    wxString sel = resChoice->GetStringSelection();
    if (sel.empty()) return;
    wxCommandEvent evt(EVT_STREAM_RESOLUTION_CHANGED);
    evt.SetEventObject(this);
    evt.SetString(sel);
    ProcessWindowEvent(evt);
}

void StreamConfigDlg::OnFpsChanged(wxCommandEvent& event) 
{
    wxString sel = fpsChoice->GetStringSelection();
    if (sel.empty()) return;
    long fps = 30;
    sel.BeforeFirst(' ').ToLong(&fps);
    if (fps <= 0) fps = 30;
    wxCommandEvent evt(EVT_STREAM_FPS_CHANGED);
    evt.SetEventObject(this);
    evt.SetInt((int)fps);
    ProcessWindowEvent(evt);
}

void StreamConfigDlg::OnSimpleSettingChanged(wxCommandEvent& event) 
{
    wxCommandEvent evt(EVT_STREAM_CONFIG_CHANGED);
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}