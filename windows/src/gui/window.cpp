#include "gui/window.h"
#include "gui/studio.h"
#include "gui/appicons.h"
#include "settings.h"
#include "logger.h"

#include <wx/display.h>
#include <wx/srchctrl.h>
#include <wx/listbox.h>
#include <wx/wrapsizer.h>
#include <wx/statline.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/evtloop.h>
#include <cmath>

Window::Window(Server::HostInfo hostinfo)
    : wxFrame(nullptr, wxID_ANY, "PrismForge Studio", wxDefaultPosition, wxDefaultSize), statusTimer(this)
{
    const auto appIcons = PrismForge::AppIcons::Load();
    SetIcons(appIcons);
    const auto appIcon = PrismForge::AppIcons::ForTray(appIcons, this);
    const auto shellResult = PrismForge::AppIcons::InitializeWindowIdentity(static_cast<HWND>(GetHandle()));
    if (FAILED(shellResult)) logger << "[UI] Window shell identity failed: " << shellResult << std::endl;
    SetMinClientSize(FromDIP(wxSize(640, 480)));
    SetClientSize(FromDIP(wxSize(1100, 740)));
    auto savedW = Settings::Get("WINDOW_WIDTH"), savedH = Settings::Get("WINDOW_HEIGHT");
    if (savedW >= 640 && savedW <= 3840 && savedH >= 480 && savedH <= 2160)
        SetClientSize(FromDIP(wxSize(savedW, savedH)));
    auto area = wxDisplay((unsigned)0).GetClientArea();
    if (GetSize().x > area.width || GetSize().y > area.height) SetSize(wxSize(std::min(GetSize().x, area.width), std::min(GetSize().y, area.height)));
    Center();
    if (Settings::Get("WINDOW_MAXIMIZED") == 1) Maximize();
    taskbarIcon = new VCamTaskBarIcon(this);
    taskbarIcon->SetIcon(appIcon, "PrismForge Studio");
    InitializeMenu(hostinfo);
    shell = new wxPanel(this);
    Studio::s_rootShell = shell;
    Studio::Style(shell);
    shell->SetBackgroundStyle(wxBG_STYLE_PAINT);
    shell->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(shell);
        auto s = shell->GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        Studio::RenderAmbientBackground(s.x, s.y);
        if (Studio::s_ambientBg.IsOk()) {
            dc.DrawBitmap(Studio::s_ambientBg, 0, 0, false);
        } else {
            dc.SetBackground(wxBrush(Studio::Background()));
            dc.Clear();
        }
    });
    auto* root = new wxBoxSizer(wxVERTICAL);
    logger << "[UI] Building Studio controls" << std::endl;
    InitializeTopBar(shell, root);
    contentSizer = new wxBoxSizer(wxHORIZONTAL);
    previewPane = new Studio::Card(shell);
    previewPane->customRadius = FromDIP(24);
    Studio::Style(previewPane, true);
    auto* previewSizer = new wxBoxSizer(wxVERTICAL);
    auto* previewHeader = new wxBoxSizer(wxHORIZONTAL);
    previewHeader->Add(Studio::Label(previewPane, "Camera preview", 10, false, true), 0, wxALIGN_CENTER_VERTICAL);
    previewHeader->AddStretchSpacer();
    liveChip = new Studio::LiveChip(previewPane);
    previewHeader->Add(liveChip, 0, wxALIGN_CENTER_VERTICAL);
    previewSizer->Add(previewHeader, 0, wxEXPAND | wxALL, FromDIP(16));
    InitializeCanvasPanel(previewPane, previewSizer);
    InitializeBottomBar(previewPane, previewSizer);
    previewPane->SetSizer(previewSizer);
    contentSizer->Add(previewPane, 1, wxEXPAND);
    logger << "[UI] Building inspector" << std::endl;
    BuildInspector();
    contentSizer->Add(inspector, 0, wxEXPAND | wxLEFT, FromDIP(20));
    root->Add(contentSizer, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    noticeLabel = Studio::Label(shell, "Choose a camera above, or connect your phone to begin.", 10, true);
    noticeLabel->SetMinSize(wxSize(0, -1));
    footer->Add(noticeLabel, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    statsText = Studio::Label(shell, "No video", 9, true, false, wxALIGN_RIGHT);
    statsText->SetMinSize(FromDIP(wxSize(200, -1)));
    Studio::Help(statsText, "Incoming stream statistics", "Measured incoming video. A meeting app may negotiate a different output frame rate.");
    footer->Add(statsText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    root->Add(footer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(24));

    auto* shellSizer = new wxBoxSizer(wxHORIZONTAL);
    BuildRail(shellSizer);
    shellSizer->Add(root, 1, wxEXPAND);
    shell->SetSizer(shellSizer);

    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(shell, 1, wxEXPAND);
    SetSizer(frameSizer);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        Studio::InvalidateBackground(); // re-cache ambient bg at new size
        if (inspector) AdaptLayout();
        e.Skip();
    });
    Bind(wxEVT_ICONIZE, &Window::MinimizeToTaskbar, this);
    Bind(wxEVT_TIMER, &Window::Tick, this);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowCommands(); }, COMMANDS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleInspector(); }, CONTROLS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        ShowFullScreen(!IsFullScreen(), wxFULLSCREEN_NOMENUBAR | wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION);
    }, FOCUS_PREVIEW);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (snapshotButton->IsEnabled()) { wxCommandEvent e(wxEVT_BUTTON, snapshotButton->GetId()); snapshotButton->GetEventHandler()->ProcessEvent(e); }
    }, SNAPSHOT_ACTION);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ForceFullRepaint(); }, DEBUG_FORCE_REPAINT);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE && IsFullScreen()) ShowFullScreen(false);
        else if (e.GetKeyCode() == WXK_ESCAPE && compact && inspectorRequested) ToggleInspector();
        else e.Skip();
    });
    logger << "[UI] Applying initial state" << std::endl;
    SetDeviceState(nullptr, nullptr);
    ApplyTheme();
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) { CallAfter([this] { ApplyTheme(); }); e.Skip(); });
    statusTimer.Start(1000);
    AdaptLayout();
    CallAfter([this] { UpdateInspectorLayout(); });
    logger << "[UI] Studio layout ready" << std::endl;
}
Window::~Window() {
    statusTimer.Stop();
    PrismForge::AppIcons::ClearWindowIdentity(static_cast<HWND>(GetHandle()));
    delete taskbarIcon;
}
void Window::HideToTray() { Hide(); }
void Window::RestoreFromTray() { Show(); Iconize(false); Raise(); SetFocus(); }
void Window::Dispatch(int id) { wxCommandEvent event(wxEVT_MENU, id); GetEventHandler()->ProcessEvent(event); }
void Window::InitializeMenu(Server::HostInfo)
{
    auto* bar = new wxMenuBar;
    auto* file = new wxMenu;
    file->Append(COMMANDS, "Search actions\tCtrl+K");
    file->Append(MenuIDs::QR, "Connect a phone\tCtrl+N");
    file->Append(SNAPSHOTS, "Open snapshots folder");
    file->Append(DIAGNOSTICS, "Export diagnostics...");
    file->AppendSeparator();
    file->Append(MenuIDs::HIDE2TRAY, "Hide to tray\tCtrl+H");
    file->AppendCheckItem(MenuIDs::MINIMIZE_ON_CLOSE, "Keep running when window closes")->Check(Settings::Get("MINIMIZE_TASKBAR") == 1);
    file->AppendSeparator(); file->Append(wxID_EXIT, "Quit PrismForge\tCtrl+Q");
    bar->Append(file, "&Studio");
    auto* view = new wxMenu;
    view->Append(CONTROLS, "Show / hide controls\tCtrl+,");
    view->Append(FOCUS_PREVIEW, "Full screen\tF11");
    view->Append(SNAPSHOT_ACTION, "Take snapshot\tCtrl+Shift+S");
    view->AppendSeparator();
    view->Append(DEBUG_FORCE_REPAINT, "Force full repaint\tF5");
    auto* themes = new wxMenu;
    themes->AppendRadioItem(THEME_SYSTEM, "System"); themes->AppendRadioItem(THEME_DARK, "Dark"); themes->AppendRadioItem(THEME_LIGHT, "Light"); themes->AppendRadioItem(THEME_GLASS, "Glass");
    int theme = std::clamp(Settings::Get("STUDIO_THEME"), 0, 3); themes->Check(THEME_SYSTEM + theme, true);
    view->AppendSubMenu(themes, "Appearance"); bar->Append(view, "&View");
    auto* device = new wxMenu;
    device->Append(MenuIDs::DEVICES, "Connected devices");
    device->Append(RECONNECT, "Reconnect selected phone\tCtrl+R");
    device->AppendCheckItem(MenuIDs::SAVESTATE, "Remember device settings")->Check(Settings::Get("SAVE_DEVICE_STATES") == 1);
    auto* modes = new wxMenu;
    modes->AppendRadioItem(MenuIDs::VCAM_DYNAMIC, "Available while app is open");
    modes->AppendRadioItem(MenuIDs::VCAM_ALWAYS, "Always available in Windows");
    modes->AppendRadioItem(MenuIDs::VCAM_DISABLED, "DirectShow only");
    modes->Check(MenuIDs::VCAM_DYNAMIC + std::clamp(Settings::Get("VIRTUAL_CAMERA_MODE"), 0, 2), true);
    device->AppendSubMenu(modes, "Windows camera visibility");
    auto* resolutions = new wxMenu;
    resolutions->AppendRadioItem(MenuIDs::DS_SD, "640 x 480"); resolutions->AppendRadioItem(MenuIDs::DS_HD, "1280 x 720");
    resolutions->AppendRadioItem(MenuIDs::DS_FHD, "1920 x 1080"); resolutions->AppendRadioItem(MenuIDs::DS_QHD, "3840 x 2160 (4K)");
    int res = Settings::Get("DIRECTSHOW_RESOLUTION"); resolutions->Check(MenuIDs::DS_SD + (res < 0 ? 2 : std::clamp(res, 0, 3)), true);
    device->AppendSubMenu(resolutions, "DirectShow output resolution"); bar->Append(device, "&Device");
    auto* help = new wxMenu; help->Append(wxID_ABOUT, "About PrismForge"); bar->Append(help, "&Help"); SetMenuBar(bar);
}
void Window::InitializeTopBar(wxPanel* parent, wxBoxSizer* root)
{
    auto* top = new wxBoxSizer(wxHORIZONTAL);

    // Title
    auto* titleSizer = new wxBoxSizer(wxHORIZONTAL);
    titleSizer->Add(Studio::Label(parent, "PrismForge", 15, false, true), 0, wxALIGN_CENTER_VERTICAL);
    titleSizer->Add(Studio::Label(parent, " Studio", 9, true, false), 0, wxALIGN_BOTTOM | wxBOTTOM, FromDIP(2));
    top->Add(titleSizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));

    // Source Capsule Card: [ StatusDot | Source Choice | Pin | Controls ]
    auto* sourceCard = new Studio::Card(parent);
    auto* sourceSizer = new wxBoxSizer(wxHORIZONTAL);

    statusDot = new Studio::StatusDot(sourceCard);
    sourceSizer->Add(statusDot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    sourceChoice = new Studio::GlassSelect(sourceCard, wxID_ANY);
    sourceChoice->Append("Select a camera");
    sourceChoice->SetSelection(0);
    sourceChoice->SetMinSize(FromDIP(wxSize(200, 32)));
    Studio::Help(sourceChoice, "Camera source", "Choose a mobile or system camera. Refresh to discover capture devices.");
    sourceSizer->Add(sourceChoice, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));

    auto* refreshSources = Studio::Button(sourceCard, "Refresh", "Refresh connected camera inputs.");
    refreshSources->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(REFRESH_SOURCES); });
    sourceSizer->Add(refreshSources, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    favoriteButton = Studio::Button(sourceCard, "Pin", "Pin the selected phone for easy identification in the source list.");
    favoriteButton->SetMinSize(FromDIP(wxSize(44, 30)));
    favoriteButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(FAVORITE); });
    sourceSizer->Add(favoriteButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    controlsButton = Studio::Button(sourceCard, "Controls", "Show camera, profile and output controls (Ctrl+,)");
    controlsButton->SetMinSize(FromDIP(wxSize(68, 30)));
    controlsButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleInspector(); });
    sourceSizer->Add(controlsButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    sourceCard->SetSizerAndFit(sourceSizer);
    top->Add(sourceCard, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

    // Quick Actions
    auto* commands = Studio::Button(parent, "Search actions", "Find controls and keyboard shortcuts (Ctrl+K)");
    commands->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowCommands(); });
    top->Add(commands, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    connectButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(parent, "+ Connect phone", "Pair with a QR code, or connect an Android phone using USB.", true));
    connectButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(MenuIDs::QR); });
    top->Add(connectButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    themeButton = Studio::IconButton(parent, "theme", "Appearance", "Toggle System, Dark, Light, or Glass theme");
    themeButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int next = (Studio::appearance + 1) % 4;
        Dispatch(THEME_SYSTEM + next);
    });
    top->Add(themeButton, 0, wxALIGN_CENTER_VERTICAL);

    root->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
}
void Window::InitializeCanvasPanel(wxPanel* parent, wxBoxSizer* root)
{
    canvas = new Canvas(parent, wxDefaultPosition, FromDIP(wxSize(400, 300)));
    canvas->SetMinSize(FromDIP(wxSize(160, 100)));
    root->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    // The empty state offers the same connection action as the header.
    canvas->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { if (!connected) Dispatch(MenuIDs::QR); });
}
void Window::InitializeBottomBar(wxPanel*, wxBoxSizer*)
{
    auto* dock = new Studio::Card(canvas, false, true);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto add = [&](const wxString& icon, const wxString& label, const wxString& tip) {
        auto* button = Studio::IconButton(dock, icon, label, tip);
        row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(2)); cameraControls.push_back(button); return button;
    };
    swapButton = add("switch", "Flip camera", "Switch between front and rear cameras.");
    torchButton = add("flash", "Flashlight", "Toggle camera flashlight / torch on or off.");
    flipButton = add("mirror", "Mirror", "Mirror the outgoing video horizontally.");
    flipVerticalButton = add("flip-v", "Flip vertically", "Flip outgoing video vertically.");
    rotateLeftButton = add("left", "Rotate left", "Rotate video 90 degrees counterclockwise.");
    rotateRightButton = add("right", "Rotate right", "Rotate video 90 degrees clockwise.");
    row->Add(new Studio::Separator(dock), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    zoomOutButton = add("minus", "Zoom out", "Zoom out by 0.5x.");
    zoomLevelLabel = Studio::Label(dock, "1.0x", 10, true);
    row->Add(zoomLevelLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
    zoomInButton = add("plus", "Zoom in", "Zoom in by 0.5x; maximum depends on your phone.");
    row->Add(new Studio::Separator(dock), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    snapshotButton = add("snapshot", "Snapshot", "Save a photo (Ctrl+Shift+S). Open snapshots from the navigation rail.");
    auto* padding = new wxBoxSizer(wxVERTICAL); padding->Add(row, 0, wxALL, FromDIP(4)); dock->SetSizerAndFit(padding);
    auto* overlay = new wxBoxSizer(wxVERTICAL); overlay->AddStretchSpacer(); overlay->Add(dock, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(16)); canvas->SetSizer(overlay);
}
void Window::BuildRail(wxBoxSizer* root)
{
    rail = new Studio::Card(shell, true); rail->SetMinSize(FromDIP(wxSize(64,-1)));
    auto* stack = new wxBoxSizer(wxVERTICAL);
    auto* logo = Studio::Button(rail, "P", "PrismForge Studio", true); logo->SetMinSize(FromDIP(wxSize(40,40)));
    logo->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){ if(compact && inspectorRequested) ToggleInspector(); }); stack->Add(logo,0,wxALIGN_CENTER|wxTOP|wxBOTTOM,FromDIP(14));
    auto entry = [&](const wxString& icon,const wxString& label,int action) {
        auto* b=Studio::IconButton(rail,icon,label,label); b->Bind(wxEVT_BUTTON,[this,action](wxCommandEvent&){Dispatch(action);});stack->Add(b,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(8));return b;
    };
    auto* studio=Studio::IconButton(rail,"camera","Studio","Return to the camera preview");studio->selected=true;studio->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){if(compact&&inspectorRequested)ToggleInspector();});stack->Add(studio,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(8));
    entry("phone","Connected devices",MenuIDs::DEVICES);
    entry("sliders","Camera controls",CONTROLS);
    entry("folder","Open snapshots",SNAPSHOTS);
    stack->AddStretchSpacer();
    auto* theme=Studio::IconButton(rail,"theme","Appearance","System, Dark, Light or Glass appearance");theme->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){ShowAppearance();});stack->Add(theme,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(8));
    entry("help","About PrismForge",wxID_ABOUT);rail->SetSizer(stack);root->Add(rail,0,wxEXPAND);
}
void Window::ShowAppearance()
{
    wxMenu menu;
    for(int i=0;i<4;++i) { const char* names[]={"System","Dark","Light","Glass"};menu.AppendRadioItem(THEME_SYSTEM+i,names[i]); }
    menu.Check(THEME_SYSTEM+Studio::appearance,true);PopupMenu(&menu);
}
void Window::BuildInspector()
{
    inspector = new Studio::GlassInspectorPanel(shell, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(310, -1)));
    inspector->SetMinSize(FromDIP(wxSize(310, 80)));

    auto* note = new Studio::Card(inspector);
    auto* ns = new wxBoxSizer(wxVERTICAL);
    ns->Add(Studio::Label(note, "Make it yours", 12, false, true), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    deviceLabel = Studio::Label(note, "Connect a phone to unlock camera controls.", 10, true);
    deviceLabel->Wrap(FromDIP(252));
    ns->Add(deviceLabel, 0, wxALL, FromDIP(14));
    note->SetSizer(ns);
    inspector->AddCard(note);

    auto choice = [&](wxPanel* parent, wxBoxSizer* s, const wxString& name, const wxString& help) {
        auto* c = new Studio::GlassSelect(parent, wxID_ANY);
        Studio::Help(c, name, help);
        s->Add(c, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
        return c;
    };

    // Quality Profile Section
    profileSection = new Studio::Section(inspector, "Quality profile", true);
    profileSection->SetValue("Custom");
    profileSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(profileSection);
    auto* profile = profileSection->body;
    auto* ps = new wxBoxSizer(wxVERTICAL);
    presetChoice = choice(profile, ps, "Quality profile", "Reusable quality settings checked against your camera's available resolutions.");
    for (const auto& text : {"Custom", "Meeting", "Low bandwidth", "High quality"}) presetChoice->Append(text);
    presetChoice->SetSelection(0);
    presetChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        profileSection->SetValue(presetChoice->GetStringSelection());
        Dispatch(PRESET_APPLY);
    });
    auto* actions = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    for (const auto& action : std::vector<std::pair<wxString, int>>{{"Save", PRESET_SAVE}, {"Import", PRESET_IMPORT}, {"Export", PRESET_EXPORT}, {"Delete", PRESET_DELETE}}) {
        auto* b = Studio::Button(profile, action.first, action.first + " a quality profile.");
        b->SetFont(wxFontInfo(9).FaceName("Segoe UI"));
        b->Bind(wxEVT_BUTTON, [this, id = action.second](wxCommandEvent&) { Dispatch(id); });
        actions->Add(b, 0, wxRIGHT | wxBOTTOM, FromDIP(3));
        if (action.second == PRESET_SAVE || action.second == PRESET_EXPORT) cameraControls.push_back(b);
    }
    ps->Add(actions, 0, wxEXPAND);
    profile->SetSizer(ps);

    // Capture Quality Section
    captureSection = new Studio::Section(inspector, "Capture quality", true);
    captureSection->SetValue(wxString("1080p ") + wxUniChar(0x00B7) + " 30 fps");
    captureSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(captureSection);
    auto* capture = captureSection->body;
    auto* cs = new wxBoxSizer(wxVERTICAL);

    cs->Add(Studio::Label(capture, "Resolution", 9, true), 0, wxBOTTOM, FromDIP(6));
    resolutionSeg = new Studio::SegmentedControl(capture, {"720p", "1080p", "1440p"});
    resolutionSeg->SetSelection(1);
    resolutionSeg->onSelect = [this](int idx, const wxString& label) {
        int targetH = (idx == 0) ? 720 : (idx == 1) ? 1080 : 1440;
        for (size_t i = 0; i < resolutions.size(); ++i) {
            if (resolutions[i].second == targetH) {
                resolutionChoice->SetSelection((int)i);
                captureSection->SetValue(wxString::Format("%dp ", targetH) + wxUniChar(0x00B7) + wxString::Format(" %s fps", fpsSeg->buttons[fpsSeg->selectedIndex]->GetLabel()));
                Dispatch(QUALITY_CHANGED);
                return;
            }
        }
        captureSection->SetValue(label + " " + wxUniChar(0x00B7) + " " + fpsSeg->buttons[fpsSeg->selectedIndex]->GetLabel() + " fps");
    };
    cs->Add(resolutionSeg, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    // Hidden backing choice for driver compatibility
    resolutionChoice = new Studio::GlassSelect(capture, wxID_ANY);
    cs->Add(resolutionChoice, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    resolutionChoice->Hide();
    resolutionChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Dispatch(QUALITY_CHANGED); });
    cameraControls.push_back(resolutionChoice);

    cs->Add(Studio::Label(capture, "Frame rate", 9, true), 0, wxBOTTOM, FromDIP(6));
    fpsSeg = new Studio::SegmentedControl(capture, {"24", "30", "60"});
    fpsSeg->SetSelection(1);
    fpsSeg->onSelect = [this](int idx, const wxString& label) {
        int choiceIdx = (idx == 0) ? 1 : (idx == 1) ? 2 : 3;
        fpsChoice->SetSelection(choiceIdx);
        captureSection->SetValue(resolutionSeg->buttons[resolutionSeg->selectedIndex]->GetLabel() + " " + wxUniChar(0x00B7) + " " + label + " fps");
        Dispatch(QUALITY_CHANGED);
    };
    cs->Add(fpsSeg, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    // Hidden backing choice for driver compatibility
    fpsChoice = new Studio::GlassSelect(capture, wxID_ANY);
    fpsChoice->Hide();
    for (const auto& fps : {"15 fps", "24 fps", "30 fps", "60 fps"}) fpsChoice->Append(fps);
    fpsChoice->SetSelection(2);
    cameraControls.push_back(fpsChoice);
    capture->SetSizer(cs);

    // Bandwidth Section
    bandwidthSection = new Studio::Section(inspector, "Bandwidth", true);
    bandwidthSection->SetValue("6.0 Mbps");
    bandwidthSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(bandwidthSection);
    auto* quality = bandwidthSection->body;
    auto* qs = new wxBoxSizer(wxVERTICAL);
    bitrateLabel = Studio::Label(quality, "6.0 Mbps", 10, true);
    qs->Add(bitrateLabel, 0, wxBOTTOM, FromDIP(6));
    bitrateSlider = new Studio::GlassSlider(quality, wxID_ANY, 6000, RTSP::MIN_BITRATE, RTSP::MAX_BITRATE);
    Studio::Help(bitrateSlider, "Capture bitrate", "Higher bitrate improves detail but needs more bandwidth. Release to apply constant bitrate.");
    bitrateSlider->Bind(wxEVT_SCROLL_CHANGED, [this](wxScrollEvent&) {
        wxCommandEvent e(wxEVT_MENU, QUALITY_CHANGED); e.SetInt(1); GetEventHandler()->ProcessEvent(e);
    });
    bitrateSlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        wxString txt = wxString::Format("%.1f Mbps", bitrateSlider->GetValue() / 1000.0);
        bitrateLabel->SetLabel(txt);
        bandwidthSection->SetValue(txt);
    });
    qs->Add(bitrateSlider, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    cameraControls.push_back(bitrateSlider);
    streamOptionsButton = Studio::Button(quality, "Advanced settings", "Adaptive bitrate, codec, focus, stabilization and torch.");
    qs->Add(streamOptionsButton, 0, wxEXPAND);
    cameraControls.push_back(streamOptionsButton);
    quality->SetSizer(qs);

    // Image & Framing Section
    framingSection = new Studio::Section(inspector, "Image && framing", false);
    framingSection->SetValue("Standard");
    framingSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(framingSection);
    auto* framing = framingSection->body;
    auto* fs = new wxBoxSizer(wxVERTICAL);
    adjustmentsButton = Studio::Button(framing, "Color, exposure && effects", "Adjust brightness, contrast, saturation, and filters.");
    fs->Add(adjustmentsButton, 0, wxEXPAND);
    cameraControls.push_back(adjustmentsButton);
    framing->SetSizer(fs);

    // Shared desktop colour look: one processed frame feeds all outputs.
    lookSection = new Studio::Section(inspector, "Colour look", true);
    lookSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(lookSection);
    auto* looks = new wxBoxSizer(wxVERTICAL);
    lookChoice = choice(lookSection->body, looks, "Colour look",
        "Built-in look for preview, snapshots and virtual camera. A loaded LUT applies after this look.");
    for (const auto& name : {"Original", "Warm", "Cool", "Mono"}) lookChoice->Append(name);
    lookChoice->SetSelection(std::clamp(Settings::Get("DESKTOP_LOOK"), 0, 3));
    lookSection->SetValue(lookChoice->GetStringSelection());
    lookChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        lookPresetChoice->SetSelection(0);
        Dispatch(LOOK_CHANGED);
    });
    lookBypassButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(lookSection->body,
        "Bypass effects", "Compare the original image, then restore your colour, lighting, touch-up and frame choices."));
    lookBypassButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(LOOK_BYPASS); });
    looks->Add(lookBypassButton, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    looks->Add(Studio::Label(lookSection->body, "Custom LUT", 9, true), 0, wxBOTTOM, FromDIP(6));
    lutNameLabel = Studio::Label(lookSection->body, "No LUT loaded", 9, true);
    lutNameLabel->Wrap(FromDIP(240));
    looks->Add(lutNameLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* lutActions = new wxBoxSizer(wxHORIZONTAL);
    auto* loadLut = Studio::Button(lookSection->body, "Load .cube", "Load a local 3D colour LUT. Applies after the built-in look.");
    loadLut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(LUT_IMPORT); });
    lutActions->Add(loadLut, 1, wxRIGHT, FromDIP(6));
    lutClearButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(lookSection->body, "Clear", "Remove the loaded LUT; keep the built-in look."));
    lutClearButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(LUT_CLEAR); });
    lutActions->Add(lutClearButton, 0);
    looks->Add(lutActions, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    auto* packsButton=Studio::Button(lookSection->body,"Effects packs","Import downloaded packs, choose a colour effect, or remove installed versions.");
    packsButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Dispatch(PACK_LIBRARY);});
    looks->Add(packsButton,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    auto* lutStrength = new wxBoxSizer(wxHORIZONTAL);
    lutStrength->Add(Studio::Label(lookSection->body, "LUT strength", 9, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    lutIntensitySlider = new Studio::GlassSlider(lookSection->body, wxID_ANY, 100, 0, 100);
    Studio::Help(lutIntensitySlider, "LUT strength", "Blend the loaded LUT from 0% to 100%.");
    lutIntensitySlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { Dispatch(LUT_INTENSITY); });
    lutStrength->Add(lutIntensitySlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    lutIntensityLabel = Studio::Label(lookSection->body, "100%", 9, true);
    lutIntensityLabel->SetMinSize(FromDIP(wxSize(36, -1)));
    lutStrength->Add(lutIntensityLabel, 0, wxALIGN_CENTER_VERTICAL);
    looks->Add(lutStrength, 0, wxEXPAND | wxBOTTOM, FromDIP(14));
    lutClearButton->Enable(false);
    lutIntensitySlider->Enable(false);
    looks->Add(Studio::Label(lookSection->body, "Saved built-in looks", 9, true), 0, wxBOTTOM, FromDIP(6));
    lookPresetChoice = choice(lookSection->body, looks, "Look preset", "Saves the built-in look. Custom LUT selection and strength are separate.");
    lookPresetChoice->Append("Custom");
    lookPresetChoice->SetSelection(0);
    lookPresetChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Dispatch(LOOK_PRESET_APPLY); });
    auto* lookActions = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    for (const auto& action : std::vector<std::pair<wxString, int>>{{"Save", LOOK_PRESET_SAVE}, {"Import", LOOK_PRESET_IMPORT},
            {"Export", LOOK_PRESET_EXPORT}, {"Delete", LOOK_PRESET_DELETE}}) {
        auto* button = Studio::Button(lookSection->body, action.first, action.first + " a desktop look preset.");
        button->SetFont(wxFontInfo(9).FaceName("Segoe UI"));
        button->Bind(wxEVT_BUTTON, [this, id = action.second](wxCommandEvent&) { Dispatch(id); });
        lookActions->Add(button, 0, wxRIGHT | wxBOTTOM, FromDIP(3));
    }
    looks->Add(lookActions, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* lookNote = Studio::Label(lookSection->body, "Preview, snapshots and virtual camera", 9, true);
    lookNote->Wrap(FromDIP(240));
    looks->Add(lookNote, 0, wxEXPAND);
    lookSection->body->SetSizer(looks);

    // Source-independent lighting controls and original frame overlay.
    studioEffectSection=new Studio::Section(inspector,"Studio effects",true);
    studioEffectSection->onToggle=[this]{UpdateInspectorLayout();};inspector->AddCard(studioEffectSection);
    auto* studio=new wxBoxSizer(wxVERTICAL);auto* body=studioEffectSection->body;
    studio->Add(Studio::Label(body,"Lighting mode",9,true),0,wxBOTTOM,FromDIP(6));
    studioLightChoice=choice(body,studio,"Lighting","Warm/cool lighting for mobile and system cameras. Choose the lighting area below.");
    for(const auto& name:{"Off","Warm","Cool","Warm key + cool fill","Custom two lights"})studioLightChoice->Append(name);
    studioLightChoice->SetSelection(std::clamp(Settings::Get("STUDIO_LIGHT_MODE"),0,4));
    studioLightChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(STUDIO_LIGHT_MODE);});
    studio->Add(Studio::Label(body,"Preset light direction",9,true),0,wxBOTTOM,FromDIP(6));
    studioLightDirectionChoice=choice(body,studio,"Direction","Position the key light on the left, front or right.");
    for(const auto& name:{"Left","Front","Right"})studioLightDirectionChoice->Append(name);
    studioLightDirectionChoice->SetSelection(std::clamp(Settings::Get("STUDIO_LIGHT_DIRECTION"),0,2));
    studioLightDirectionChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(STUDIO_LIGHT_DIRECTION);});
    auto* strengthRow=new wxBoxSizer(wxHORIZONTAL);
    strengthRow->Add(Studio::Label(body,"Light strength",9,true),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
    int strength=Settings::Get("STUDIO_LIGHT_STRENGTH");if(strength<0||strength>100)strength=50;
    studioLightStrengthSlider=new Studio::GlassSlider(body,wxID_ANY,strength,0,100);
    Studio::Help(studioLightStrengthSlider,"Light strength","Blend studio lighting from 0% to 100%.");
    studioLightStrengthSlider->Bind(wxEVT_SLIDER,[this](wxCommandEvent&){Dispatch(STUDIO_LIGHT_STRENGTH);});
    strengthRow->Add(studioLightStrengthSlider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
    studioLightStrengthLabel=Studio::Label(body,wxString::Format("%d%%",strength),9,true);
    studioLightStrengthLabel->SetMinSize(FromDIP(wxSize(36,-1)));strengthRow->Add(studioLightStrengthLabel,0,wxALIGN_CENTER_VERTICAL);
    studio->Add(strengthRow,0,wxEXPAND|wxBOTTOM,FromDIP(14));
    studio->Add(Studio::Label(body,"Lighting area",9,true),0,wxBOTTOM,FromDIP(6));
    studioLightScopeChoice=choice(body,studio,"Lighting area","Choose the whole frame, the detected person or face shading with approximate shadows. Face with shadows requires a visible face.");
    for(const auto& name:{"Whole frame","Person only","Face with shadows"})studioLightScopeChoice->Append(name);
    studioLightScopeChoice->SetSelection(Settings::Get("STUDIO_FACE_LIGHTING")==1?2:Settings::Get("STUDIO_LIGHT_SUBJECT_ONLY")==1?1:0);
    studioLightScopeChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(STUDIO_LIGHT_SCOPE);});
    faceLightingStatusLabel=Studio::Label(body,"Face lighting: Off",9,true);
    faceLightingStatusLabel->SetName("Studio face lighting status");
    faceLightingStatusLabel->SetMinSize(FromDIP(wxSize(240,18)));
    faceLightingStatusLabel->SetMaxSize(FromDIP(wxSize(-1,18)));
    studio->Add(faceLightingStatusLabel,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    SetFaceLightingStatus("Off","Choose Face with shadows and enable a light to apply face shading.");
    customLightPanel=new wxPanel(body);
    customLightPanel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    customLightPanel->Bind(wxEVT_ERASE_BACKGROUND,[](wxEraseEvent&){});
    customLightPanel->Bind(wxEVT_PAINT,[this](wxPaintEvent&){
        wxAutoBufferedPaintDC dc(customLightPanel);const auto size=customLightPanel->GetClientSize();
        if(size.x<=0||size.y<=0)return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if(gc)Studio::PaintBackdrop(gc.get(),customLightPanel,wxRect(0,0,size.x,size.y));
    });
    auto* customLights=new wxBoxSizer(wxVERTICAL);
    customLights->Add(Studio::Label(customLightPanel,"Light positions",9,true),0,wxBOTTOM,FromDIP(6));
    studioLightPositionPad=new Studio::LightPositionPad(customLightPanel);
    studioLightPositionPad->Bind(wxEVT_SLIDER,[this](wxCommandEvent&){Dispatch(STUDIO_LIGHT_POSITION);});
    customLights->Add(studioLightPositionPad,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    auto lightSlider=[&](const wxString& name,const wxString& help,int value,int low,int high,int command,
        Studio::GlassSlider*& slider,Studio::GlassLabel*& label){
        auto* title=Studio::Label(customLightPanel,name,9,true);title->SetName("Studio label: "+name);
        customLights->Add(title,0,wxBOTTOM,FromDIP(5));
        auto* row=new wxBoxSizer(wxHORIZONTAL);
        slider=new Studio::GlassSlider(customLightPanel,wxID_ANY,value,low,high);Studio::Help(slider,name,help);
        slider->Bind(wxEVT_SLIDER,[this,command](wxCommandEvent&){Dispatch(command);});
        row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        label=Studio::Label(customLightPanel,"",9,true);label->SetMinSize(FromDIP(wxSize(54,-1)));
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);customLights->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    };
    lightSlider("Key light (1) brightness","Brightness of the primary key light; 0% turns this light off.",100,0,200,STUDIO_KEY_INTENSITY,studioKeyIntensitySlider,studioKeyIntensityLabel);
    lightSlider("Key light (1) temperature","Lower temperature is warmer; higher temperature is cooler.",3200,1500,15000,STUDIO_KEY_TEMPERATURE,studioKeyTemperatureSlider,studioKeyTemperatureLabel);
    lightSlider("Fill light (2) brightness","Brightness of the secondary fill light; 0% turns this light off.",50,0,200,STUDIO_FILL_INTENSITY,studioFillIntensitySlider,studioFillIntensityLabel);
    lightSlider("Fill light (2) temperature","Choose the fill light's colour temperature independently.",6500,1500,15000,STUDIO_FILL_TEMPERATURE,studioFillTemperatureSlider,studioFillTemperatureLabel);
    customLightPanel->SetSizer(customLights);studio->Add(customLightPanel,0,wxEXPAND|wxBOTTOM,FromDIP(4));
    studio->Add(Studio::Label(body,"Frame overlay",9,true),0,wxBOTTOM,FromDIP(6));
    studioFrameChoice=choice(body,studio,"Frame","Original studio border on preview, snapshots and virtual camera.");
    for(const auto& name:{"Off","Studio border"})studioFrameChoice->Append(name);
    studioFrameChoice->SetSelection(Settings::Get("STUDIO_FRAME")==1?1:0);
    studioFrameChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(STUDIO_FRAME_CHANGED);});
    auto* studioNote=Studio::Label(body,"Face shading with approximate shadows. Person only and Face with shadows keep the background unchanged.",9,true);
    studioNote->Wrap(FromDIP(240));studio->Add(studioNote,0,wxEXPAND);body->SetSizer(studio);
    SetStudioPresentation(studioLightChoice->GetSelection(),studioLightDirectionChoice->GetSelection(),strength,studioFrameChoice->GetSelection()==1,false);

    // Facial-area smoothing is independent of window theme appearance and studio lighting.
    touchupSection=new Studio::Section(inspector,"Face touch-up",false);
    touchupSection->onToggle=[this]{UpdateInspectorLayout();};inspector->AddCard(touchupSection);
    auto* touchupBody=touchupSection->body;auto* touchupControls=new wxBoxSizer(wxVERTICAL);
    auto touchupSlider=[&](const wxString& title,const wxString& help,int value,int command,
        Studio::GlassSlider*& slider,Studio::GlassLabel*& label){
        auto* caption=Studio::Label(touchupBody,title,9,true);caption->SetName("Touch-up label: "+title);
        touchupControls->Add(caption,0,wxBOTTOM,FromDIP(5));
        auto* row=new wxBoxSizer(wxHORIZONTAL);
        slider=new Studio::GlassSlider(touchupBody,wxID_ANY,value,0,100);Studio::Help(slider,title,help);
        slider->Bind(wxEVT_SLIDER,[this,command](wxCommandEvent&){Dispatch(command);});
        row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        label=Studio::Label(touchupBody,wxString::Format("%d%%",value),9,true);
        label->SetMinSize(FromDIP(wxSize(44,18)));label->SetMaxSize(FromDIP(wxSize(44,18)));
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);touchupControls->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    };
    const int savedTouchupStrength=Settings::Get("STUDIO_TOUCHUP_STRENGTH");
    const int savedTouchupDetail=Settings::Get("STUDIO_TOUCHUP_DETAIL");
    const int touchupStrength=savedTouchupStrength>=0&&savedTouchupStrength<=100?savedTouchupStrength:0;
    const int touchupDetail=savedTouchupDetail>=0&&savedTouchupDetail<=100?savedTouchupDetail:75;
    touchupSlider("Smoothing strength","0% turns touch-up off. 1-50% stays subtle; 51-100% progressively applies stronger, broader smoothing.",touchupStrength,
        TOUCHUP_STRENGTH,touchupStrengthSlider,touchupStrengthLabel);
    touchupSlider("Detail preservation","Higher values retain more texture and fine contrast. 100% leaves the face unchanged by touch-up.",touchupDetail,
        TOUCHUP_DETAIL,touchupDetailSlider,touchupDetailLabel);
    touchupStatusLabel=Studio::Label(touchupBody,"Touch-up: Off",9,true);
    touchupStatusLabel->SetName("Studio face touch-up status");
    touchupStatusLabel->SetMinSize(FromDIP(wxSize(240,18)));touchupStatusLabel->SetMaxSize(FromDIP(wxSize(-1,18)));
    touchupControls->Add(touchupStatusLabel,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    auto* touchupNote=Studio::Label(touchupBody,
        "Smooths the detected facial area while preserving eyes, lips and brows. This is facial-area smoothing, not semantic skin segmentation.",9,true);
    touchupNote->Wrap(FromDIP(240));touchupControls->Add(touchupNote,0,wxEXPAND);
    touchupBody->SetSizer(touchupControls);
    SetTouchupPresentation(touchupStrength/100.f,touchupDetail/100.f,false);
    SetTouchupStatus("Off","Increase Smoothing strength to enable face touch-up.");

    // Glasses and the selected cap use the same face tracker and processed output.
    accessorySection=new Studio::Section(inspector,"Face accessories",false);
    accessorySection->onToggle=[this]{UpdateInspectorLayout();};inspector->AddCard(accessorySection);
    auto* accessoryBody=accessorySection->body;auto* accessoryControls=new wxBoxSizer(wxVERTICAL);
    auto accessoryCaption=[&](const wxString& title){
        auto* caption=Studio::Label(accessoryBody,title,9,true);caption->SetName("Accessory label: "+title);
        accessoryControls->Add(caption,0,wxBOTTOM,FromDIP(5));
    };
    accessoryCaption("Glasses");
    accessoryGlassesChoice=choice(accessoryBody,accessoryControls,"Glasses","Choose glasses that turn with your face.");
    for(const auto& name:{"Off","Classic Wayfarer","Gold Teardrop Aviator","Vintage Round Tortoise","Chic Cat-Eye Emerald"})
        accessoryGlassesChoice->Append(name);
    const int savedGlassesStyle=Settings::Get("STUDIO_GLASSES_STYLE");
    accessoryGlassesStyle=savedGlassesStyle>=0&&savedGlassesStyle<=3?savedGlassesStyle:0;
    accessoryGlassesChoice->SetSelection(Settings::Get("STUDIO_GLASSES")==1?accessoryGlassesStyle+1:0);
    accessoryGlassesChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(ACCESSORY_GLASSES);});
    accessoryCaption("Cap");
    accessoryCapChoice=choice(accessoryBody,accessoryControls,"Cap","Choose the Pokemon Masters cap that turns with your face.");
    for(const auto& name:{"Off","Pokemon Masters cap"})accessoryCapChoice->Append(name);
    const int savedCapStyle=Settings::Get("STUDIO_CAP_STYLE");
    accessoryCapStyle=2;
    // Retired visor and legacy unnamed caps stay off until the owner chooses a supported cap.
    accessoryCapChoice->SetSelection(Settings::Get("STUDIO_CAP")==1&&savedCapStyle==2?1:0);
    accessoryCapChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(ACCESSORY_CAP);});
    auto savedFit=[](const char* key,int minimum,int maximum,int fallback){
        const int value=Settings::Get(key);return value>=minimum&&value<=maximum?value:fallback;
    };
    const int accessoryScale=savedFit("STUDIO_ACCESSORY_SCALE",50,150,100);
    const int capScale=savedFit("STUDIO_CAP_SCALE",75,150,100);
    const int capHeight=savedFit("STUDIO_CAP_HEIGHT",0,50,20);
    auto accessoryFit=[&](const wxString& title,const wxString& help,int value,int minimum,int maximum,
        int command,Studio::GlassSlider*& slider,Studio::GlassLabel*& label,bool height=false){
        accessoryCaption(title);auto* row=new wxBoxSizer(wxHORIZONTAL);
        slider=new Studio::GlassSlider(accessoryBody,wxID_ANY,value,minimum,maximum);
        Studio::Help(slider,title,help);slider->Bind(wxEVT_SLIDER,[this,command](wxCommandEvent&){Dispatch(command);});
        row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        label=Studio::Label(accessoryBody,height?wxString::Format("%.1f cm",value/10.f-2.f):wxString::Format("%d%%",value),9,true);
        const auto bounds=FromDIP(wxSize(height?64:44,18));label->SetMinSize(bounds);label->SetMaxSize(bounds);
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);accessoryControls->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    };
    accessoryFit("Glasses size","Adjust only glasses size from 50% to 150%.",accessoryScale,50,150,
        ACCESSORY_SCALE,accessoryScaleSlider,accessoryScaleLabel);
    accessoryFit("Cap size","Adjust only cap size from 75% to 150%.",capScale,75,150,
        ACCESSORY_CAP_SCALE,accessoryCapScaleSlider,accessoryCapScaleLabel);
    accessoryFit("Cap height","Move the cap down or up from -2.0 to 3.0 cm.",capHeight,0,50,
        ACCESSORY_CAP_HEIGHT,accessoryCapHeightSlider,accessoryCapHeightLabel,true);
    accessoryStatusLabel=Studio::Label(accessoryBody,"Accessories: Off",9,true);
    accessoryStatusLabel->SetName("Studio face accessories status");
    accessoryStatusLabel->SetMinSize(FromDIP(wxSize(240,18)));accessoryStatusLabel->SetMaxSize(FromDIP(wxSize(-1,18)));
    accessoryControls->Add(accessoryStatusLabel,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    auto* accessoryNote=Studio::Label(accessoryBody,"Glasses and your selected cap turn with your face. Adjust each fit separately or wear both together.",9,true);
    accessoryNote->Wrap(FromDIP(240));accessoryControls->Add(accessoryNote,0,wxEXPAND);
    accessoryBody->SetSizer(accessoryControls);
    SetAccessoryPresentation(accessoryGlassesChoice->GetSelection()>0,accessoryCapChoice->GetSelection()>0,
        accessoryScale/100.f,false,accessoryCapStyle,accessoryGlassesStyle,capScale/100.f,capHeight/10.f-2.f);
    SetAccessoryStatus("Off","Choose glasses or a cap to enable face accessories.");


    // Original solid 3D masks and helmets run on the same processed webcam output.
    faceFilterSection=new Studio::Section(inspector,"3D face filters",false);
    faceFilterSection->onToggle=[this]{UpdateInspectorLayout();};inspector->AddCard(faceFilterSection);
    auto* filterBody=faceFilterSection->body;auto* filterControls=new wxBoxSizer(wxVERTICAL);
    filterControls->Add(Studio::Label(filterBody,"3D filter",9,true),0,wxBOTTOM,FromDIP(5));
    faceFilterChoice=choice(filterBody,filterControls,"3D face filter","Choose a 3D filter that turns with your face.");
    for(const auto& name:{"Off","Aurora fox mask","Nocturne owl mask","Atlas explorer helmet","Local model","Royal crown"})faceFilterChoice->Append(name);
    faceFilterChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(FACE_FILTER_STYLE);});
    auto filterFit=[&](const wxString& title,const wxString& help,int value,int minimum,int maximum,int command,
        Studio::GlassSlider*& slider,Studio::GlassLabel*& label){
        auto* caption=Studio::Label(filterBody,title,9,true);caption->SetName("Face filter label: "+title);
        filterControls->Add(caption,0,wxBOTTOM,FromDIP(5));auto* row=new wxBoxSizer(wxHORIZONTAL);
        slider=new Studio::GlassSlider(filterBody,wxID_ANY,value,minimum,maximum);Studio::Help(slider,title,help);
        slider->Bind(wxEVT_SLIDER,[this,command](wxCommandEvent&){Dispatch(command);});
        row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));label=Studio::Label(filterBody,"",9,true);
        const auto bounds=FromDIP(wxSize(64,18));label->SetMinSize(bounds);label->SetMaxSize(bounds);
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);filterControls->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    };
    const int filterStyle=savedFit("STUDIO_FACE_FILTER",0,5,0);
    const int filterScale=savedFit(filterStyle==5?"STUDIO_ROYAL_CROWN_SCALE":"STUDIO_FACE_FILTER_SCALE",75,150,100);
    const int filterHeight=savedFit(filterStyle==5?"STUDIO_ROYAL_CROWN_HEIGHT":"STUDIO_FACE_FILTER_HEIGHT",0,60,30);
    filterFit("Filter size","Resize the whole filter from 75% to 150%, keeping its shape.",filterScale,75,150,FACE_FILTER_SCALE,
        faceFilterScaleSlider,faceFilterScaleLabel);
    filterFit("Filter height","Move the filter down or up from -3.0 to 3.0 cm.",filterHeight,0,60,FACE_FILTER_HEIGHT,
        faceFilterHeightSlider,faceFilterHeightLabel);
    faceFilterLocalName=Studio::Label(filterBody,"No local model imported",9,true);
    faceFilterLocalName->Wrap(FromDIP(240));filterControls->Add(faceFilterLocalName,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    auto* importRow=new wxBoxSizer(wxHORIZONTAL);
    faceFilterImportButton=new Studio::SmoothButton(filterBody,wxID_ANY,"Import .glb");
    faceFilterImportButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Dispatch(FACE_FILTER_IMPORT);});
    faceFilterRemoveButton=new Studio::SmoothButton(filterBody,wxID_ANY,"Remove");
    faceFilterRemoveButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Dispatch(FACE_FILTER_REMOVE);});
    Studio::Help(faceFilterImportButton,"Import local 3D model","Prepare a supported GLB as a static rest-pose copy with embedded textures. Import notices explain material changes. The original file stays untouched.");
    Studio::Help(faceFilterRemoveButton,"Remove local model","Remove its converted library copy; your original GLB stays untouched.");
    importRow->Add(faceFilterImportButton,1,wxRIGHT,FromDIP(6));importRow->Add(faceFilterRemoveButton,0);
    filterControls->Add(importRow,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    filterControls->Add(Studio::Label(filterBody,"Model performance",9,true),0,wxBOTTOM,FromDIP(5));
    modelPerformanceChoice=choice(filterBody,filterControls,"Model performance","Auto recommends a budget from your hardware. Custom lets you choose import geometry limits and render detail.");
    for(const auto& name:{"Auto","Performance","Balanced","Quality","Custom"})modelPerformanceChoice->Append(name);
    modelPerformanceChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){
        SetModelPerformancePresentation(GetModelPerformanceSettings(),modelPerformanceSummary->GetLabel(),modelPerformanceBusy,false);
        Dispatch(FACE_FILTER_PERFORMANCE);
    });
    modelPerformanceSummary=Studio::Label(filterBody,"Auto: checking available hardware...",9,true);
    modelPerformanceSummary->SetName("Model performance hardware recommendation");
    modelPerformanceSummary->SetMinSize(FromDIP(wxSize(240,120)));modelPerformanceSummary->SetMaxSize(FromDIP(wxSize(-1,120)));
    modelPerformanceSummary->Wrap(FromDIP(240));filterControls->Add(modelPerformanceSummary,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    modelPerformanceCustomPanel=new wxPanel(filterBody);modelPerformanceCustomPanel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    modelPerformanceCustomPanel->Bind(wxEVT_ERASE_BACKGROUND,[](wxEraseEvent&){});
    modelPerformanceCustomPanel->Bind(wxEVT_PAINT,[this](wxPaintEvent&){
        wxAutoBufferedPaintDC dc(modelPerformanceCustomPanel);const auto size=modelPerformanceCustomPanel->GetClientSize();if(size.x<=0||size.y<=0)return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(gc)Studio::PaintBackdrop(gc.get(),modelPerformanceCustomPanel,wxRect(0,0,size.x,size.y));
    });
    auto* performanceControls=new wxBoxSizer(wxVERTICAL);
    const char* performanceNames[]={"Maximum nodes","Maximum vertices","Maximum triangles","Render detail"};
    const char* performanceHelp[]={"Import scene limit from 512 to 16384 nodes.","Import geometry limit from 1000 to 1000000 vertices, in steps of 1000.","Import geometry limit from 1000 to 1000000 triangles, in steps of 1000.","Maximum render detail edge from 256 to 1536 pixels. Applies immediately."};
    const int performanceMinimum[]={512,1,1,256},performanceMaximum[]={16384,1000,1000,1536},performanceDefault[]={4096,200,200,1024};
    for(size_t field=0;field<modelPerformanceSliders.size();++field){
        auto* caption=Studio::Label(modelPerformanceCustomPanel,performanceNames[field],9,true);caption->SetName("Model performance label: "+wxString(performanceNames[field]));
        performanceControls->Add(caption,0,wxBOTTOM,FromDIP(4));auto* row=new wxBoxSizer(wxHORIZONTAL);
        auto* slider=new Studio::GlassSlider(modelPerformanceCustomPanel,wxID_ANY,performanceDefault[field],performanceMinimum[field],performanceMaximum[field]);
        Studio::Help(slider,performanceNames[field],performanceHelp[field]);slider->SetName("Model performance: "+wxString(performanceNames[field]));
        slider->Bind(wxEVT_SLIDER,[this](wxCommandEvent&){
            SetModelPerformancePresentation(GetModelPerformanceSettings(),modelPerformanceSummary->GetLabel(),modelPerformanceBusy,false);
            Dispatch(FACE_FILTER_PERFORMANCE);
        });
        modelPerformanceSliders[field]=slider;row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        auto* label=Studio::Label(modelPerformanceCustomPanel,"",9,true);label->SetName("Model performance value: "+wxString(performanceNames[field]));
        label->SetMinSize(FromDIP(wxSize(88,18)));label->SetMaxSize(FromDIP(wxSize(88,18)));modelPerformanceLabels[field]=label;
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);performanceControls->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    }
    auto* performanceNote=Studio::Label(modelPerformanceCustomPanel,"Render detail applies immediately. Geometry limits check imports and pause oversized models. Meshes are not simplified.",9,true);
    performanceNote->Wrap(FromDIP(240));performanceControls->Add(performanceNote,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    modelPerformanceCustomPanel->SetSizer(performanceControls);modelPerformanceCustomPanel->Hide();filterControls->Add(modelPerformanceCustomPanel,0,wxEXPAND);
    faceFilterCustomPanel=new wxPanel(filterBody);faceFilterCustomPanel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    faceFilterCustomPanel->Bind(wxEVT_ERASE_BACKGROUND,[](wxEraseEvent&){});
    faceFilterCustomPanel->Bind(wxEVT_PAINT,[this](wxPaintEvent&){
        wxAutoBufferedPaintDC dc(faceFilterCustomPanel);const auto size=faceFilterCustomPanel->GetClientSize();if(size.x<=0||size.y<=0)return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(gc)Studio::PaintBackdrop(gc.get(),faceFilterCustomPanel,wxRect(0,0,size.x,size.y));
    });
    auto* localControls=new wxBoxSizer(wxVERTICAL);
    const char* customNames[]={"Left / right","Up / down","Forward / back","Tilt","Turn","Roll"};
    for(size_t axis=0;axis<faceFilterCustomSliders.size();++axis){
        localControls->Add(Studio::Label(faceFilterCustomPanel,customNames[axis],9,true),0,wxBOTTOM,FromDIP(4));
        auto* row=new wxBoxSizer(wxHORIZONTAL);const int limit=axis<3?200:180;
        auto* slider=new Studio::GlassSlider(faceFilterCustomPanel,wxID_ANY,limit,0,limit*2);
        Studio::Help(slider,customNames[axis],axis<3?"Move in head-local coordinates from -20 to +20 cm.":"Rotate the whole model from -180 to +180 degrees.");
        slider->Bind(wxEVT_SLIDER,[this](wxCommandEvent&){Dispatch(FACE_FILTER_CUSTOM_FIT);});
        faceFilterCustomSliders[axis]=slider;row->Add(slider,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        auto* label=Studio::Label(faceFilterCustomPanel,"",9,true);label->SetMinSize(FromDIP(wxSize(64,18)));faceFilterCustomLabels[axis]=label;
        row->Add(label,0,wxALIGN_CENTER_VERTICAL);localControls->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    }
    localControls->Add(Studio::Label(faceFilterCustomPanel,"Head occlusion",9,true),0,wxBOTTOM,FromDIP(4));
    faceFilterOcclusionChoice=new Studio::GlassSelect(faceFilterCustomPanel,wxID_ANY);
    faceFilterOcclusionChoice->Append("Hide surfaces behind head");faceFilterOcclusionChoice->Append("Show whole model");
    faceFilterOcclusionChoice->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){Dispatch(FACE_FILTER_CUSTOM_FIT);});
    localControls->Add(faceFilterOcclusionChoice,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    faceFilterResetButton=new Studio::SmoothButton(faceFilterCustomPanel,wxID_ANY,"Reset local fit");
    faceFilterResetButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Dispatch(FACE_FILTER_CUSTOM_RESET);});
    localControls->Add(faceFilterResetButton,0,wxEXPAND|wxBOTTOM,FromDIP(10));faceFilterCustomPanel->SetSizer(localControls);
    filterControls->Add(faceFilterCustomPanel,0,wxEXPAND);
    faceFilterStatusLabel=Studio::Label(filterBody,"3D filter: Off",9,true);faceFilterStatusLabel->SetName("Studio 3D face filter status");
    faceFilterStatusLabel->SetMinSize(FromDIP(wxSize(240,18)));faceFilterStatusLabel->SetMaxSize(FromDIP(wxSize(-1,18)));
    filterControls->Add(faceFilterStatusLabel,0,wxEXPAND|wxBOTTOM,FromDIP(10));
    auto* filterNote=Studio::Label(filterBody,"3D filters replace glasses and caps while selected. Choose Off to restore your saved accessories.",9,true);
    filterNote->Wrap(FromDIP(240));filterControls->Add(filterNote,0,wxEXPAND);filterBody->SetSizer(filterControls);
    SetFaceFilterPresentation(filterStyle,filterScale/100.f,filterHeight/10.f-3.f,false);
    SetLocalFaceFilterPresentation({}, {}, false, false, false);
    SetModelPerformancePresentation({},"Auto: checking available hardware...",false,false);
    SetFaceFilterStatus("Off","Choose a 3D face filter to use it on your preview and webcam.");

    // Background effects share the same processed frame as the live preview and webcam.
    backgroundSection = new Studio::Section(inspector, "Background", true);
    backgroundSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(backgroundSection);
    auto* background = backgroundSection->body;
    auto* backgrounds = new wxBoxSizer(wxVERTICAL);
    backgrounds->Add(Studio::Label(background, "Effects preset", 9, true), 0, wxBOTTOM, FromDIP(6));
    effectsPresetChoice = choice(background, backgrounds, "Effects preset",
        "Apply Original, Portrait blur or Studio background to the desktop effects pipeline.");
    for (const auto& name : {"Custom", "Original", "Portrait blur", "Studio background"})
        effectsPresetChoice->Append(name);
    effectsPresetChoice->SetSelection(0);
    effectsPresetChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Dispatch(EFFECT_PRESET_APPLY); });
    backgrounds->Add(Studio::Label(background, "Background mode", 9, true), 0, wxBOTTOM, FromDIP(6));
    backgroundChoice = choice(background, backgrounds, "Background mode",
        "Keep the original background, blur it, or replace it with a colour or image.");
    for (const auto& name : {"Off", "Blur", "Replace"}) backgroundChoice->Append(name);
    backgroundChoice->SetSelection(0);
    backgroundChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        SetEffectsPresetPresentation(0);
        Dispatch(BACKGROUND_MODE);
    });
    backgrounds->Add(Studio::Label(background, "Background quality", 9, true), 0, wxBOTTOM, FromDIP(6));
    backgroundQualityChoice = choice(background, backgrounds, "Background quality",
        "Balanced uses less processing. High quality updates the person outline more often with finer detail.");
    for (const auto& name : {"Balanced", "High quality"}) backgroundQualityChoice->Append(name);
    backgroundQualityChoice->SetSelection(1);
    backgroundQualityChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Dispatch(BACKGROUND_QUALITY); });
    auto* backgroundBlurRow = new wxBoxSizer(wxHORIZONTAL);
    backgroundBlurRow->Add(Studio::Label(background, "Blur strength", 9, true),
        0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    backgroundBlurSlider = new Studio::GlassSlider(background, wxID_ANY, 50, 0, 100);
    Studio::Help(backgroundBlurSlider, "Background blur strength", "Adjust blur behind the person from 0% to 100%.");
    backgroundBlurSlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        SetEffectsPresetPresentation(0);
        Dispatch(BACKGROUND_BLUR);
    });
    backgroundBlurRow->Add(backgroundBlurSlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    backgroundBlurLabel = Studio::Label(background, "50%", 9, true);
    backgroundBlurLabel->SetMinSize(FromDIP(wxSize(36, -1)));
    backgroundBlurRow->Add(backgroundBlurLabel, 0, wxALIGN_CENTER_VERTICAL);
    backgrounds->Add(backgroundBlurRow, 0, wxEXPAND | wxBOTTOM, FromDIP(14));
    backgrounds->Add(Studio::Label(background, "Replacement image", 9, true), 0, wxBOTTOM, FromDIP(6));
    backgroundImageLabel = Studio::Label(background, "No image selected; using colour", 9, true);
    backgroundImageLabel->Wrap(FromDIP(240));
    backgrounds->Add(backgroundImageLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* backgroundActions = new wxBoxSizer(wxHORIZONTAL);
    backgroundLoadButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(background,
        "Load image", "Choose a local background image for preview, snapshots and virtual camera."));
    backgroundLoadButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(BACKGROUND_IMPORT); });
    backgroundActions->Add(backgroundLoadButton, 1, wxRIGHT, FromDIP(6));
    backgroundClearButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(background,
        "Clear", "Remove the replacement image and use the selected background colour."));
    backgroundClearButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(BACKGROUND_CLEAR); });
    backgroundActions->Add(backgroundClearButton, 0);
    backgrounds->Add(backgroundActions, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    backgroundColourButton = dynamic_cast<Studio::SmoothButton*>(Studio::Button(background,
        "Choose background colour", "Choose the replacement colour used when no image is selected."));
    backgroundColourButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Dispatch(BACKGROUND_COLOUR); });
    backgrounds->Add(backgroundColourButton, 0, wxEXPAND | wxBOTTOM, FromDIP(14));
    segmentationStatusLabel = Studio::Label(background, "Person detection: Not started", 9, false, true);
    backgrounds->Add(segmentationStatusLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    segmentationDetailLabel = Studio::Label(background, "Select Blur or Replace to start person detection.", 9, true);
    segmentationDetailLabel->Wrap(FromDIP(240));
    backgrounds->Add(segmentationDetailLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    auto* backgroundNote = Studio::Label(background,
        "Without a valid person mask, the original background is kept.", 9, true);
    backgroundNote->Wrap(FromDIP(240));
    backgrounds->Add(backgroundNote, 0, wxEXPAND);
    background->SetSizer(backgrounds);
    SetBackgroundPresentation(0, 50, {}, backgroundColour, false);
    SetSegmentationStatus("Not started", "Select Blur or Replace to start person detection.");

    // Windows Output Section
    outputSection = new Studio::Section(inspector, "Windows output", false);
    outputSection->SetValue("DirectShow + VCam");
    outputSection->onToggle = [this] { UpdateInspectorLayout(); };
    inspector->AddCard(outputSection);
    auto* output = outputSection->body;
    auto* os = new wxBoxSizer(wxVERTICAL);
    outputLabel = Studio::Label(output, "Checking camera availability...", 10, true);
    outputLabel->Wrap(FromDIP(250));
    os->Add(outputLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    outputChoice = choice(output, os, "DirectShow output resolution", "DirectShow output format. Windows Camera apps negotiate their own format.");
    for (const auto& r : {"640 x 480", "1280 x 720", "1920 x 1080", "3840 x 2160"}) outputChoice->Append(r);
    int res = Settings::Get("DIRECTSHOW_RESOLUTION");
    outputChoice->SetSelection(res < 0 ? 2 : std::clamp(res, 0, 3));
    outputChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        Dispatch(MenuIDs::DS_SD + outputChoice->GetSelection());
    });
    output->SetSizer(os);

    // Tune the Glass Section
    auto* tuneCard = new Studio::Card(inspector);
    auto* ts = new wxBoxSizer(wxVERTICAL);
    ts->Add(Studio::Label(tuneCard, "Tune the glass", 11, false, true), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    // Blur row
    auto* blurRow = new wxBoxSizer(wxHORIZONTAL);
    blurRow->Add(Studio::Label(tuneCard, "Blur", 9, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    blurSlider = new Studio::GlassSlider(tuneCard, wxID_ANY, Studio::glassBlur, 0, 50);
    Studio::Help(blurSlider, "Glass blur", "Adjust background orb diffusion and blur intensity.");
    blurRow->Add(blurSlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    blurValueLabel = Studio::Label(tuneCard, wxString::Format("%d", Studio::glassBlur), 9, true);
    blurValueLabel->SetMinSize(FromDIP(wxSize(28, -1)));
    blurRow->Add(blurValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    ts->Add(blurRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    blurSlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        Studio::glassBlur = blurSlider->GetValue();
        blurValueLabel->SetLabel(wxString::Format("%d", Studio::glassBlur));
        Studio::InvalidateBackground();
        shell->Refresh();
        UpdateInspectorLayout();
    });

    // Opacity row
    auto* opacityRow = new wxBoxSizer(wxHORIZONTAL);
    opacityRow->Add(Studio::Label(tuneCard, "Opacity", 9, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    int initOpacity = int(std::round(Studio::glassFill * 100.0f));
    opacitySlider = new Studio::GlassSlider(tuneCard, wxID_ANY, initOpacity, 2, 30);
    Studio::Help(opacitySlider, "Glass opacity", "Adjust translucent panel fill opacity (2% to 30%).");
    opacityRow->Add(opacitySlider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    opacityValueLabel = Studio::Label(tuneCard, wxString::Format("%d%%", initOpacity), 9, true);
    opacityValueLabel->SetMinSize(FromDIP(wxSize(28, -1)));
    opacityRow->Add(opacityValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    ts->Add(opacityRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(14));

    opacitySlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        Studio::glassFill = opacitySlider->GetValue() / 100.0f;
        opacityValueLabel->SetLabel(wxString::Format("%d%%", opacitySlider->GetValue()));
        shell->Refresh();
        UpdateInspectorLayout();
    });

    tuneCard->SetSizer(ts);
    inspector->AddCard(tuneCard);

    inspector->FinalizeCards();

    CallAfter([this] {
        UpdateInspectorLayout();
    });
}
void Window::AdaptLayout()
{
    const auto oldPreviewRect = previewPane->GetRect();
    const auto oldInspectorRect = inspector->GetRect();
    const bool wasPreviewShown = previewPane->IsShown();
    const bool wasInspectorShown = inspector->IsShown();
    bool narrow = GetClientSize().x < FromDIP(1060);
    if (narrow != compact) { compact = narrow; inspectorRequested = !compact; }
    inspector->Show(inspectorRequested);
    previewPane->Show(!compact || !inspectorRequested);
    auto* item = contentSizer->GetItem(inspector); item->SetProportion(compact ? 1 : 0); item->SetBorder(compact ? 0 : FromDIP(20));
    controlsButton->SetLabel(compact && inspectorRequested ? "Back to preview" : "Controls");
    shell->Layout();
    UpdateInspectorLayout();
    const bool changed = oldPreviewRect != previewPane->GetRect() || oldInspectorRect != inspector->GetRect() ||
        wasPreviewShown != previewPane->IsShown() || wasInspectorShown != inspector->IsShown();
    if (changed) {
        // Resizing a painted glass card only invalidates its exposed strip on MSW.
        // Repaint the old rounded edge and moved child backdrops after layout,
        // once per geometry/visibility change, outside the streaming frame path.
        wxRect dirty = oldPreviewRect;
        dirty.Union(oldInspectorRect);
        dirty.Union(previewPane->GetRect());
        dirty.Union(inspector->GetRect());
        dirty.Intersect(shell->GetClientRect());
        if (!dirty.IsEmpty()) shell->RefreshRect(dirty, true);
        if (previewPane->IsShown()) Studio::RefreshAll(previewPane);
#ifdef __WXMSW__
        if (!dirty.IsEmpty()) {
            const RECT bounds{dirty.x, dirty.y, dirty.GetRight() + 1, dirty.GetBottom() + 1};
            ::RedrawWindow(static_cast<HWND>(shell->GetHandle()), &bounds, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
#else
        shell->Update();
#endif
    }
}
void Window::ToggleInspector() { inspectorRequested = !inspectorRequested; AdaptLayout(); if (inspectorRequested) presetChoice->SetFocus(); else canvas->SetFocus(); }
void Window::SetDeviceState(const DeviceDescriptor* device, const StreamOptions* options)
{
    systemInput = false;
    resolutionChoice->Hide(); resolutionSeg->Show();
    resolutionSeg->Enable(device && options); fpsSeg->Enable(device && options);
    connected = device && options;
    for (auto* c : cameraControls) c->Enable(connected);
    presetChoice->Enable(connected);
    favoriteButton->Enable(connected);
    resolutionChoice->Clear();
    resolutions.clear();
    if (!connected) {
        if (liveChip) liveChip->SetLive(false, "Not connected");
        if (statusDot) statusDot->SetLive(false);
        if (connectButton) {
            connectButton->SetLabel("+ Connect phone");
            connectButton->primary = true;
            connectButton->Refresh();
        }
        deviceLabel->SetLabel("Select a system camera or connect your phone.");
        noticeLabel->SetLabel("Choose a camera above, or connect your phone to begin.");
        statsText->SetLabel("No video");
        canvas->Clear();
        canvas->SetHudStats("", "");
        canvas->SetEmptyState("Choose your camera", "Select a system camera, or connect an Android phone\nover USB or Wi-Fi.");
        resolutionChoice->Append("No camera connected");
        resolutionChoice->SetSelection(0);
        lastFrameTime = 0;
        if (profileSection) profileSection->SetValue("Custom");
        if (captureSection) captureSection->SetValue("No camera");
        if (bandwidthSection) bandwidthSection->SetValue(wxString::Format("%.1f Mbps", bitrateSlider ? (bitrateSlider->GetValue() / 1000.0) : 6.0));
    } else {
        if (liveChip) liveChip->SetLive(true, "Live");
        if (statusDot) statusDot->SetLive(true);
        if (connectButton) {
            connectButton->SetLabel("Disconnect");
            connectButton->primary = true;
            connectButton->Refresh();
        }
        const bool usb = device->url().find("127.0.0.1") != std::string::npos || device->url().find("localhost") != std::string::npos;
        deviceLabel->SetLabel(wxString::FromUTF8(device->name()) + (usb ? "\nUSB connection" : "\nWi-Fi connection"));
        noticeLabel->SetLabel(wxString::Format("Streaming from %s over %s.", wxString::FromUTF8(device->name()), usb ? "USB" : "Wi-Fi"));
        resolutions = options->backCameraActive ? device->backResolutions() : device->frontResolutions();
        for (size_t i = 0; i < resolutions.size(); ++i) {
            resolutionChoice->Append(wxString::Format("%d x %d", resolutions[i].first, resolutions[i].second));
            if (resolutions[i] == options->resolution) resolutionChoice->SetSelection((int)i);
        }
        const int rates[] = {15, 24, 30, 60};
        for (int i = 0; i < 4; ++i) if (rates[i] == options->fps) fpsChoice->SetSelection(i);
        bitrateSlider->SetValue(options->bitrate);
        bitrateLabel->SetLabel(options->adaptiveBitrate ? wxString("Adaptive bitrate") : wxString::Format("%.1f Mbps", options->bitrate / 1000.0));
        if (torchButton) {
            auto* b = dynamic_cast<Studio::SmoothButton*>(torchButton);
            if (b) {
                b->selected = options->flashEnabled;
                b->SetToolTip(options->flashEnabled ? "Flashlight is on (click to turn off)" : "Flashlight is off (click to turn on)");
                b->Refresh();
            }
        }
        if (swapButton) {
            auto* b = dynamic_cast<Studio::SmoothButton*>(swapButton);
            if (b) {
                b->selected = !options->backCameraActive;
                b->SetToolTip(options->backCameraActive ? "Rear camera active (click to flip to front)" : "Front camera active (click to flip to rear)");
                b->Refresh();
            }
        }
        zoomLevelLabel->SetLabel(wxString::Format("%.1fx", options->zoom));

        if (resolutionSeg) {
            if (options->resolution.second == 720) resolutionSeg->SetSelection(0);
            else if (options->resolution.second == 1080) resolutionSeg->SetSelection(1);
            else if (options->resolution.second == 1440) resolutionSeg->SetSelection(2);
        }
        if (fpsSeg) {
            if (options->fps == 24) fpsSeg->SetSelection(0);
            else if (options->fps == 30) fpsSeg->SetSelection(1);
            else if (options->fps == 60) fpsSeg->SetSelection(2);
        }

        wxString resText = wxString::Format("%dp", options->resolution.second);
        wxString fpsText = wxString::Format("%d fps", options->fps);
        canvas->SetHudStats(resText, fpsText);
        if (captureSection) captureSection->SetValue(wxString::Format("%dp ", options->resolution.second) + wxUniChar(0x00B7) + wxString::Format(" %d fps", options->fps));
        if (bandwidthSection) bandwidthSection->SetValue(options->adaptiveBitrate ? wxString("Adaptive") : wxString::Format("%.1f Mbps", options->bitrate / 1000.0));
        if (profileSection) profileSection->SetValue(presetChoice->GetStringSelection());
    }
    deviceLabel->Wrap(FromDIP(240));
    UpdateInspectorLayout();
}
void Window::SetSystemCameraState(const wxString& name,
    const std::vector<PrismForge::Capture::CaptureMode>& modes, int selectedMode,
    const PrismForge::Capture::CaptureStatus& status)
{
    using namespace PrismForge::Capture;
    systemInput = true;
    connected = status.state == CaptureState::Starting || status.state == CaptureState::Running;
    for (auto* c : cameraControls) c->Enable(false);
    presetChoice->Enable(false); favoriteButton->Enable(false);
    resolutionSeg->Hide(); fpsSeg->Enable(false); resolutionChoice->Show();
    resolutionChoice->Clear(); resolutions.clear();
    resolutionChoice->Append("Automatic capture mode");
    for (const auto& mode : modes) resolutionChoice->Append(wxString::FromUTF8(mode.ToString()));
    resolutionChoice->SetSelection(selectedMode >= 0 && selectedMode < (int)modes.size() ? selectedMode+1 : 0);
    resolutionChoice->Enable(true); snapshotButton->Enable(connected);
    deviceLabel->SetLabel(name+"\nSystem capture device"); deviceLabel->Wrap(FromDIP(240));
    if (liveChip) liveChip->SetLive(status.state == CaptureState::Running,
        status.state == CaptureState::Running ? "Live" : connected ? "Connecting..." : "Camera unavailable");
    if (statusDot) statusDot->SetLive(status.state == CaptureState::Running);
    if (connectButton) { connectButton->SetLabel("+ Connect phone"); connectButton->Refresh(); }
    if (profileSection) profileSection->SetValue("Phone profiles");
    if (bandwidthSection) bandwidthSection->SetValue("Not used for system cameras");
    if (captureSection) captureSection->SetValue(status.deliveredMode.width > 0
        ? wxString::FromUTF8(status.deliveredMode.ToString()) : wxString("Automatic capture mode"));
    if (!status.errorMessage.empty()) {
        canvas->Clear(); statsText->SetLabel("Camera unavailable");
        canvas->SetEmptyState("Camera unavailable", wxString::FromUTF8(status.errorMessage));
        Notify(wxString::FromUTF8(status.errorMessage));
    } else if (status.state == CaptureState::Disconnected) {
        canvas->Clear(); statsText->SetLabel("Camera disconnected");
        canvas->SetEmptyState("Camera disconnected", "Reconnect the camera, then use Refresh or Ctrl+R.");
    }
    UpdateInspectorLayout();
}
bool Window::VerifySourceControls() {
    sourceChoice->Clear(); sourceChoice->Append("Select a camera");
    int mobile=sourceChoice->AppendHeading("Mobile cameras"); sourceChoice->Append("Phone");
    int system=sourceChoice->AppendHeading("System cameras"); int camera=sourceChoice->Append("Webcam");
    sourceChoice->SetSelection(2); sourceChoice->SetSelection(system);
    bool ok=sourceChoice->GetSelection()==2 && !sourceChoice->IsSelectable(mobile);
    wxKeyEvent key(wxEVT_KEY_DOWN); key.m_keyCode=WXK_DOWN;
    sourceChoice->GetEventHandler()->ProcessEvent(key);
    ok=ok && sourceChoice->GetSelection()==camera;
    PrismForge::Capture::CaptureStatus status; status.state=PrismForge::Capture::CaptureState::Starting;
    SetSystemCameraState("Fixture webcam", {{1920,1080,30000,1001,"NV12",false}},0,status);
    ok=ok && resolutionChoice->IsShown() && resolutionChoice->IsEnabled() &&
        !fpsSeg->IsEnabled() && !streamOptionsButton->IsEnabled() && !torchButton->IsEnabled() && snapshotButton->IsEnabled();
    SetStreamStats(1920,1080,30,0);
    ok=ok && statsText->GetLabel().Contains("System camera");
    DeviceDescriptor phone("Fixture phone","rtsp://127.0.0.1:8554/test","tcp",{{1280,720},{1920,1080}},{{1280,720},{1920,1080}},{});
    StreamOptions options; options.fps=60;
    SetDeviceState(&phone,&options);
    ok=ok && fpsSeg->IsEnabled() && fpsSeg->GetSelection()==2 && fpsChoice->GetSelection()==3;
    for(int index=0;index<3;++index) {
        wxCommandEvent click(wxEVT_BUTTON,fpsSeg->buttons[index]->GetId());
        click.SetEventObject(fpsSeg->buttons[index]);fpsSeg->buttons[index]->GetEventHandler()->ProcessEvent(click);
        ok=ok && fpsChoice->GetSelection()==index+1;
    }
    SetStreamStats(1920,1080,60,24);
    ok=ok && statsText->GetLabel().Contains("1920 x 1080") && statsText->GetLabel().Contains("60 fps");
    SetDeviceState(nullptr,nullptr);
    return ok && !resolutionChoice->IsShown() && !snapshotButton->IsEnabled();
}
void Window::SetStreamStats(int w, int h, int fps, double bitrate)
{
    if (!connected) return;
    lastFrameTime = wxGetUTCTimeMillis();
    if (liveChip) liveChip->SetLive(true, "Live");
    if (statusDot) statusDot->SetLive(true);
    statsText->SetLabel(systemInput
        ? wxString::Format("%d x %d  |  %d fps  |  System camera", w, h, fps)
        : wxString::Format("%d x %d  |  %d fps  |  %.1f Mbps", w, h, fps, bitrate));
    canvas->SetHudStats(wxString::Format("%dp", h), wxString::Format("%d fps", fps));
    shell->Layout();
}
void Window::BeginStream() {
    lastFrameTime = 0;
    if (liveChip) liveChip->SetLive(false, "Connecting...");
    statsText->SetLabel("Waiting for frames");
}
void Window::SetOutputStatus(const wxString& text) {
    int resolution = Settings::Get("DIRECTSHOW_RESOLUTION"); if (resolution < 0 || resolution > 3) resolution = 2;
    outputChoice->SetSelection(resolution); GetMenuBar()->Check(MenuIDs::DS_SD + resolution, true); outputLabel->SetLabel(text); outputLabel->Wrap(FromDIP(240)); UpdateInspectorLayout(); }
void Window::Notify(const wxString& text) { noticeLabel->SetLabel(text); noticeLabel->SetToolTip(text); shell->Layout(); }
void Window::Tick(wxTimerEvent&)
{
    if (connected && (lastFrameTime == 0 || (wxGetUTCTimeMillis() - lastFrameTime).GetValue() > 5000)) {
        if (liveChip) liveChip->SetLive(false, lastFrameTime == 0 ? "Waiting..." : "Interrupted");
        if (statusDot) statusDot->SetLive(false);
        statsText->SetLabel("Waiting for frames");
        canvas->Clear();
        canvas->SetHudStats("", "");
        canvas->SetEmptyState("Waiting for your camera", "Check your camera connection.\nUse Ctrl+R to reconnect.");
    }
}
void Window::SaveGeometry()
{
    if (!IsMaximized() && !IsFullScreen() && !IsIconized()) {
        auto size = ToDIP(GetClientSize()); Settings::Set("WINDOW_WIDTH",size.x); Settings::Set("WINDOW_HEIGHT",size.y);
    }
    Settings::Set("WINDOW_MAXIMIZED", IsMaximized() ? 1 : 0);
}
void Window::ApplyTheme()
{
    Studio::InvalidateBackground();
    // Pre-render the ambient background at the current shell size
    if (shell) {
        auto s = shell->GetClientSize();
        Studio::RenderAmbientBackground(s.x, s.y);
    }
    Studio::Dialog(this);
    Studio::Style(shell);
    Studio::Style(previewPane, true);
    if (inspector) inspector->SetBackgroundColour(Studio::Glass() ? Studio::GlassBg1() : Studio::Background());
    if (rail) rail->SetBackgroundColour(Studio::Rail());
    if (canvas) canvas->SetBackgroundColour(Studio::Stage());
    Studio::SyncLabels(this);
    const char* themeNames[] = {"Follow system", "Dark theme", "Light theme", "Glass theme"};
    if (themeButton) {
        themeButton->SetToolTip(wxString::Format("Current: %s. Click to switch.", themeNames[Studio::appearance]));
    }
    // Cascade refresh to every child so glass controls repaint with the new theme
    Studio::RefreshAll(shell);
    UpdateInspectorLayout();
    Refresh(true);
}
void Window::UpdateInspectorLayout()
{
    ++inspectorLayoutRevision;
    if (inspector) inspector->UpdateInspectorLayout();
}
void Window::ForceFullRepaint()
{
    Studio::InvalidateBackground();
    shell->Refresh(true);
    if (inspector) {
        inspector->UpdateInspectorLayout();
        inspector->Refresh(true);
    }
    Studio::RefreshAll(shell);
    Update();
}
bool Window::VerifyControlsLayout()
{
    // Observe real native paint damage, rather than just comparing sizer values.
    class PaintCoverage final : public wxEvtHandler {
    public:
        explicit PaintCoverage(wxWindow* target) : target(target) {
            Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
                const auto box = this->target->GetUpdateRegion().GetBox();
                const auto client = this->target->GetClientSize();
                ++paintCount;
                if (paintBoxes.size() < 16) { paintBoxes.push_back(box); paintClients.push_back(client); }
                if (box.x == 0 && box.width >= client.x && client.x > 0) fullWidth = true;
                event.Skip();
            });
        }
        void Reset() { fullWidth = false; paintCount = 0; paintBoxes.clear(); paintClients.clear(); }
        wxWindow* target;
        bool fullWidth = false;
        int paintCount = 0;
        std::vector<wxRect> paintBoxes;
        std::vector<wxSize> paintClients;
    } coverage(previewPane);
    previewPane->PushEventHandler(&coverage);
    const auto originalSize = GetClientSize();
    const bool originalMaximized = IsMaximized();
    const bool originalRequested = inspectorRequested;
    const int originalTheme = Studio::appearance;
    bool passed = true;
    auto check = [&](bool ok, const char* reason) {
        if (!ok) { passed = false; logger << "[UI-VERIFY] Controls layout failure: " << reason << std::endl; }
    };
    // This verifier runs inside one long CallAfter batch. Native theme/size events
    // can still be queued even when the sizer already reports its new geometry.
    // Drain only UI/paint messages, never input or timer events. Update() services
    // existing damage only: it does not manufacture a successful invalidation.
    auto settleNativePaint = [&] {
        if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_UI);
        shell->Update();
        previewPane->Update();
    };
    auto paintTrace = [&](int theme, const char* phase) {
        const auto client = previewPane->GetClientSize();
        logger << "[UI-VERIFY] Controls paint theme=" << theme << " phase=" << phase
            << " client=" << client.x << "x" << client.y << " events=" << coverage.paintCount
            << " fullWidth=" << coverage.fullWidth << " shown=" << previewPane->IsShownOnScreen()
            << " frozen=" << previewPane->IsFrozen();
#ifdef __WXMSW__
        RECT nativePending{};
        const auto hwnd = static_cast<HWND>(previewPane->GetHandle());
        const bool damaged = ::GetUpdateRect(hwnd, &nativePending, FALSE) != FALSE;
        logger << " nativeVisible=" << (::IsWindowVisible(hwnd) != FALSE) << " nativeDamage=" << damaged
            << ":" << nativePending.left << "," << nativePending.top << "," << nativePending.right << "," << nativePending.bottom;
#endif
        logger << " boxes=";
        for (size_t index = 0; index < coverage.paintBoxes.size(); ++index) {
            const auto& box = coverage.paintBoxes[index]; const auto& size = coverage.paintClients[index];
            logger << "[" << box.x << "," << box.y << "," << box.width << "," << box.height
                << ";client=" << size.x << "x" << size.y << "]";
        }
        logger << std::endl;
    };
    Maximize(false);
    settleNativePaint();
    for (int theme = 0; theme != 4; ++theme) {
        Studio::appearance = theme;
        ApplyTheme();
        SetClientSize(FromDIP(wxSize(1200, 700)));
        AdaptLayout();
        if (!inspectorRequested) ToggleInspector();
        settleNativePaint();
        check(IsShownOnScreen() && !IsIconized() && !previewPane->IsFrozen(), "native fixture ready for paint coverage");
        check(!compact && inspector->IsShown() && previewPane->IsShown(), "wide controls visible");
        const int withControls = previewPane->GetClientSize().x;
        coverage.Reset();
        ToggleInspector();
        settleNativePaint();
        paintTrace(theme, "expanded");
        check(!inspector->IsShown() && previewPane->IsShown(), "wide controls hidden");
        check(previewPane->GetClientSize().x > withControls, "preview expands into controls space");
        check(coverage.fullWidth, "expanded card repaints its full width including the former edge");
        check(liveChip->GetRect().GetRight() < previewPane->GetClientSize().x, "live badge follows expanded edge");
        check(canvas->GetRect().GetRight() < previewPane->GetClientSize().x, "canvas stays inside preview card");
        coverage.Reset();
        ToggleInspector();
        settleNativePaint();
        paintTrace(theme, "contracted");
        check(inspector->IsShown() && previewPane->GetClientSize().x == withControls, "controls restore original preview width");
        check(coverage.fullWidth, "contracted card repaints its full width");
        SetClientSize(FromDIP(wxSize(800, 600)));
        AdaptLayout();
        check(compact && previewPane->IsShown() && !inspector->IsShown(), "compact preview mode");
        ToggleInspector();
        check(inspector->IsShown() && !previewPane->IsShown() && controlsButton->GetLabel() == "Back to preview", "compact controls replace preview");
        ToggleInspector();
        check(!inspector->IsShown() && previewPane->IsShown(), "compact returns to preview");
    }
#ifdef __WXMSW__
    check(::SendMessageW(static_cast<HWND>(GetHandle()), WM_GETICON, ICON_BIG, 0) != 0, "native taskbar icon assigned");
    check(PrismForge::AppIcons::VerifyWindowIdentity(static_cast<HWND>(GetHandle())), "native shell icon/relaunch identity matches module-relative assets");
    PWSTR currentAppId = nullptr;
    const auto processIdResult = ::GetCurrentProcessExplicitAppUserModelID(&currentAppId);
    check(SUCCEEDED(processIdResult) && currentAppId && std::wcscmp(currentAppId, PrismForge::AppIcons::AppId) == 0, "wx initialization preserves PrismForge process identity");
    ::CoTaskMemFree(currentAppId);
    check(::SendMessageW(static_cast<HWND>(GetHandle()), WM_GETICON, ICON_SMALL, 0) != 0, "native caption icon assigned");
#endif
    previewPane->PopEventHandler(false);
    Studio::appearance = originalTheme;
    SetClientSize(originalSize);
    AdaptLayout();
    inspectorRequested = originalRequested;
    AdaptLayout();
    ApplyTheme();
    if (originalMaximized) Maximize();
    logger << (passed ? "[UI-VERIFY] Controls layout/paint/icons PASS" : "[UI-VERIFY] Controls layout/paint/icons FAIL") << std::endl;
    return passed;
}
bool Window::VerifyInspectorScrolling()
{
    bool passed = true;
    auto check = [&](bool condition, const char* name) {
        if (!condition) { passed = false; logger << "[UI-VERIFY] Scroll failure: " << name << std::endl; }
    };
    // Hidden process startup can defer the frame's first native size event.
    // Establish real child bounds before exercising mouse capture/hit testing.
    Layout();
    AdaptLayout();
    inspector->UpdateInspectorLayout();
    const int viewport = inspector->GetClientSize().y;
    check(viewport > 0 && inspector->GetClientSize().x > 0, "fixture has a real viewport");
    check(inspector->GetContentHeight() > viewport, "fixture must overflow viewport");
    auto* first = inspector->GetChildren().GetFirst()->GetData();
    inspector->ScrollTo(0);
    inspector->ScrollTo(1);
    check(first->GetPosition().y == -1, "minus-one child position");
    inspector->ScrollTo(0);
    for (int i = 0; i < 6; ++i) {
        wxMouseEvent wheel(wxEVT_MOUSEWHEEL);
        wheel.m_wheelRotation = -20;
        wheel.m_wheelDelta = 120;
        wheel.m_wheelAxis = wxMOUSE_WHEEL_VERTICAL;
        inspector->GetEventHandler()->ProcessEvent(wheel);
    }
    check(std::abs(inspector->GetScrollOffset() - FromDIP(44)) <= 1, "precision wheel proportional movement");
    const int beforeZero = inspector->GetScrollOffset();
    wxMouseEvent zero(wxEVT_MOUSEWHEEL);
    zero.m_wheelRotation = 0;
    zero.m_wheelDelta = 120;
    zero.m_wheelAxis = wxMOUSE_WHEEL_VERTICAL;
    inspector->GetEventHandler()->ProcessEvent(zero);
    check(inspector->GetScrollOffset() == beforeZero, "zero wheel does not scroll");
    for (int cycle = 0; cycle < 10; ++cycle) {
        inspector->ScrollTo(inspector->GetContentHeight());
        const int bottom = inspector->GetContentHeight() - inspector->GetClientSize().y;
        check(inspector->GetScrollOffset() == bottom, "bottom reachable");
        inspector->UpdateInspectorLayout();
        check(inspector->GetScrollOffset() == bottom, "layout preserves bottom offset");
        check(!inspector->IsFrozen(), "inspector remains paintable after layout");
        std::function<void(wxWindow*)> checkNativeVisibility=[&](wxWindow* window) {
            if(!window->IsShown())return;
            check(::IsWindowVisible(static_cast<HWND>(window->GetHandle()))!=FALSE,"shown descendants keep native visibility");
            for(auto* child:window->GetChildren())checkNativeVisibility(child);
        };
        checkNativeVisibility(inspector);
        inspector->ScrollTo(0);
        check(first->GetPosition().y == 0, "top restored after round trip");
    }
    inspector->ScrollTo(150);
    const bool open = lookSection->header->expanded;
    lookSection->SetExpanded(!open);
    check(inspector->GetScrollOffset() <= std::max(0, inspector->GetContentHeight() - inspector->GetClientSize().y), "collapse clamps scroll range");
    lookSection->SetExpanded(open);
    inspector->ScrollTo(0);
    logger << "[UI-VERIFY] Thumb fixture: viewport " << inspector->GetClientSize().x << "x" << inspector->GetClientSize().y
        << ", content " << inspector->GetContentHeight() << ", offset " << inspector->GetScrollOffset() << std::endl;
    wxMouseEvent down(wxEVT_LEFT_DOWN);
    down.SetPosition(wxPoint(inspector->GetClientSize().x - 2, 2));
    check(inspector->GetEventHandler()->ProcessEvent(down), "thumb press handled");
    check(inspector->HasCapture(), "thumb acquires mouse capture");
    wxMouseEvent motion(wxEVT_MOTION);
    motion.SetPosition(wxPoint(inspector->GetClientSize().x - 2, inspector->GetClientSize().y + FromDIP(100)));
    inspector->GetEventHandler()->ProcessEvent(motion);
    check(inspector->GetScrollOffset() == inspector->GetContentHeight() - inspector->GetClientSize().y, "thumb dragging reaches bottom");
    wxMouseEvent up(wxEVT_LEFT_UP);
    inspector->GetEventHandler()->ProcessEvent(up);
    check(!inspector->HasCapture(), "thumb releases mouse capture");
    wxMouseCaptureLostEvent lost(inspector->GetId());
    check(inspector->GetEventHandler()->ProcessEvent(lost), "capture loss handled");
    inspector->ScrollTo(0);
    logger << (passed ? "[UI-VERIFY] Inspector scrolling PASS" : "[UI-VERIFY] Inspector scrolling FAIL") << std::endl;
    return passed;
}

void Window::SetBackgroundPresentation(int mode, int blur, const wxString& imageName,
    const wxColour& colour, bool bypassed, bool highQuality)
{
    if (!backgroundChoice) return;
    mode = std::clamp(mode, 0, 2);
    blur = std::clamp(blur, 0, 100);
    backgroundChoice->SetSelection(mode);
    backgroundBlurSlider->SetValue(blur);
    backgroundBlurLabel->SetLabel(wxString::Format("%d%%", blur));
    backgroundChoice->Enable(!bypassed);
    backgroundQualityChoice->SetSelection(highQuality ? 1 : 0);
    backgroundQualityChoice->Enable(!bypassed);
    effectsPresetChoice->Enable(!bypassed);
    backgroundBlurSlider->Enable(mode == 1 && !bypassed);
    backgroundImageName = imageName;
    backgroundBypassed = bypassed;
    if (colour.IsOk()) backgroundColour = colour;
    const wxString imageLabel = imageName.empty()
        ? wxString("No image selected; using colour") : imageName.Left(100);
    if (backgroundImageLabel->GetLabel() != imageLabel) {
        backgroundImageLabel->SetLabel(imageLabel);
        backgroundImageLabel->Wrap(FromDIP(240));
    }
    backgroundImageLabel->SetToolTip(imageName);
    backgroundLoadButton->Enable(mode == 2 && !bypassed);
    backgroundClearButton->Enable(mode == 2 && !imageName.empty() && !bypassed);
    backgroundColourButton->Enable(mode == 2 && imageName.empty() && !bypassed);
    backgroundColourButton->SetLabel(wxString::Format("Colour #%02X%02X%02X",
        unsigned(backgroundColour.Red()), unsigned(backgroundColour.Green()), unsigned(backgroundColour.Blue())));
    backgroundSection->SetValue(bypassed ? wxString("Bypassed") : backgroundChoice->GetStringSelection());
    UpdateInspectorLayout();
}

void Window::SetSegmentationStatus(const wxString& status, const wxString& detail)
{
    if (!segmentationStatusLabel || (segmentationStatus == status && segmentationDetail == detail)) return;
    segmentationStatus = status;
    segmentationDetail = detail;
    segmentationStatusLabel->SetLabel("Person detection: " + status.Left(48));
    segmentationStatusLabel->Wrap(FromDIP(240));
    segmentationDetailLabel->SetLabel(detail.Left(240));
    segmentationDetailLabel->SetToolTip(detail);
    segmentationDetailLabel->Wrap(FromDIP(240));
    segmentationDetailLabel->Show(!detail.empty());
    UpdateInspectorLayout();
}

void Window::SetEffectsPresetPresentation(int selection)
{
    if (effectsPresetChoice) effectsPresetChoice->SetSelection(std::clamp(selection, 0, 3));
}

bool Window::VerifyBackgroundControls()
{
    const int mode = backgroundChoice->GetSelection();
    const int blur = backgroundBlurSlider->GetValue();
    const bool highQuality = backgroundQualityChoice->GetSelection() == 1;
    const int preset = effectsPresetChoice->GetSelection();
    const auto image = backgroundImageName;
    const auto colour = backgroundColour;
    const auto status = segmentationStatus;
    const auto detail = segmentationDetail;
    const bool bypassed = backgroundBypassed;
    bool passed = true;
    auto check = [&](bool ok, const char* reason) {
        if (!ok) { passed = false; logger << "[UI-VERIFY] Background failure: " << reason << std::endl; }
    };
    // Intercept the real dispatched menu events so this UI-only check never opens
    // dialogs, changes saved settings, or starts an inference worker.
    wxEvtHandler route;
    int routed = wxID_NONE;
    route.Bind(wxEVT_MENU, [&](wxCommandEvent& event) { routed = event.GetId(); },
        BACKGROUND_MODE, EFFECT_PRESET_APPLY);
    PushEventHandler(&route);
    auto fire = [&](wxWindow* control, wxEventType type, int expected) {
        routed = wxID_NONE;
        wxCommandEvent event(type, control->GetId());
        event.SetEventObject(control);
        control->GetEventHandler()->ProcessEvent(event);
        check(routed == expected, "native background control dispatches its assigned event");
    };
    SetBackgroundPresentation(0, 50, {}, wxColour(36, 48, 75), false);
    check(!backgroundBlurSlider->IsEnabled() && !backgroundLoadButton->IsEnabled()
        && !backgroundClearButton->IsEnabled() && !backgroundColourButton->IsEnabled(), "Off disables mode-specific controls");
    SetBackgroundPresentation(1, 150, {}, wxColour(20, 40, 60), false);
    check(backgroundBlurSlider->IsEnabled() && backgroundBlurSlider->GetValue() == 100
        && !backgroundLoadButton->IsEnabled(), "Blur enables and clamps its strength");
    fire(backgroundChoice, wxEVT_CHOICE, BACKGROUND_MODE);
    fire(backgroundBlurSlider, wxEVT_SLIDER, BACKGROUND_BLUR);
    SetBackgroundPresentation(1, 43, {}, wxColour(20, 40, 60), false, false);
    check(backgroundQualityChoice->IsEnabled() && backgroundQualityChoice->GetSelection() == 0,
        "Balanced quality is displayed independently of background mode");
    fire(backgroundQualityChoice, wxEVT_CHOICE, BACKGROUND_QUALITY);
    SetBackgroundPresentation(2, 43, {}, wxColour(20, 40, 60), false);
    check(backgroundLoadButton->IsEnabled() && backgroundColourButton->IsEnabled()
        && !backgroundClearButton->IsEnabled(), "Replace without image enables colour fallback");
    fire(backgroundLoadButton, wxEVT_BUTTON, BACKGROUND_IMPORT);
    fire(backgroundColourButton, wxEVT_BUTTON, BACKGROUND_COLOUR);
    SetBackgroundPresentation(2, 43, "sample-background.png", wxColour(20, 40, 60), false);
    check(backgroundClearButton->IsEnabled(), "Loaded image enables clear action");
    fire(backgroundClearButton, wxEVT_BUTTON, BACKGROUND_CLEAR);
    SetBackgroundPresentation(2, 43, "sample-background.png", wxColour(20, 40, 60), true, false);
    check(!backgroundChoice->IsEnabled() && !effectsPresetChoice->IsEnabled()
        && !backgroundClearButton->IsEnabled() && !backgroundColourButton->IsEnabled()
        && !backgroundQualityChoice->IsEnabled() && backgroundQualityChoice->GetSelection() == 0
        && backgroundChoice->GetSelection() == 2 && backgroundBlurSlider->GetValue() == 43
        && backgroundImageName == "sample-background.png" && backgroundBypassed, "Bypass retains mode, image, colour and strength");
    SetBackgroundPresentation(2, 43, "sample-background.png", wxColour(20, 40, 60), false);
    for (int choice = 1; choice <= 3; ++choice) {
        SetEffectsPresetPresentation(choice);
        fire(effectsPresetChoice, wxEVT_CHOICE, EFFECT_PRESET_APPLY);
        check(effectsPresetChoice->GetSelection() == choice, "effect preset selection survives dispatch");
    }
    for (const auto& state : {"Starting", "Ready", "Recovering", "Missing", "Failed"}) {
        SetSegmentationStatus(state, "Service-supplied detail");
        check(segmentationStatus == state && segmentationDetail == "Service-supplied detail",
            "person detection presentation reflects its supplied service state");
    }
    PopEventHandler(false);
    SetBackgroundPresentation(mode, blur, image, colour, bypassed, highQuality);
    SetEffectsPresetPresentation(preset);
    SetSegmentationStatus(status, detail);
    logger << (passed ? "[UI-VERIFY] Background controls PASS" : "[UI-VERIFY] Background controls FAIL") << std::endl;
    return passed;
}

void Window::SetStudioPresentation(int mode,int direction,int strength,bool frame,bool bypassed)
{
    if(!studioLightChoice)return;
    mode=std::clamp(mode,0,4);direction=std::clamp(direction,0,2);strength=std::clamp(strength,0,100);
    studioLightChoice->SetSelection(mode);studioLightDirectionChoice->SetSelection(direction);
    studioLightStrengthSlider->SetValue(strength);studioFrameChoice->SetSelection(frame?1:0);
    studioLightStrengthLabel->SetLabel(wxString::Format("%d%%",strength));
    studioLightChoice->Enable(!bypassed);
    studioLightDirectionChoice->Enable(mode!=0&&mode!=4&&!bypassed);studioLightStrengthSlider->Enable(mode!=0&&!bypassed);
    studioLightScopeChoice->Enable(mode!=0&&!bypassed);
    customLightPanel->Show(mode==4);customLightPanel->Enable(mode==4&&!bypassed);
    studioLightPositionPad->Enable(mode==4&&!bypassed);studioFrameChoice->Enable(!bypassed);
    studioEffectSection->SetValue(bypassed?wxString("Bypassed"):(mode?studioLightChoice->GetStringSelection()+(frame?" + frame":""):(frame?wxString("Frame"):wxString("Off"))));
}
void Window::SetCustomLightingPresentation(const PrismForge::Effects::Lighting::StudioLight& key,
    const PrismForge::Effects::Lighting::StudioLight& fill,bool personOnly,bool bypassed,bool faceLighting)
{
    if(!studioLightPositionPad)return;
    studioLightPositionPad->SetPositions(key.posX,key.posY,fill.posX,fill.posY);
    studioLightScopeChoice->SetSelection(faceLighting?2:personOnly?1:0);
    auto intensity=[&](Studio::GlassSlider* slider,Studio::GlassLabel* label,float value){
        const int percent=std::clamp(int(std::lround(value*100)),0,200);slider->SetValue(percent);label->SetLabel(wxString::Format("%d%%",percent));};
    auto temperature=[&](Studio::GlassSlider* slider,Studio::GlassLabel* label,float value){
        const int kelvin=std::clamp(int(std::lround(value)),1500,15000);slider->SetValue(kelvin);label->SetLabel(wxString::Format("%d K",kelvin));};
    intensity(studioKeyIntensitySlider,studioKeyIntensityLabel,key.intensity);
    intensity(studioFillIntensitySlider,studioFillIntensityLabel,fill.intensity);
    temperature(studioKeyTemperatureSlider,studioKeyTemperatureLabel,key.colorTemperature);
    temperature(studioFillTemperatureSlider,studioFillTemperatureLabel,fill.colorTemperature);
    studioLightPositionPad->Enable(studioLightChoice->GetSelection()==4&&!bypassed);
}

void Window::SetTouchupPresentation(float strength,float detail,bool bypassed)
{
    if(!touchupStrengthSlider)return;
    const int percent=std::isfinite(strength)?std::clamp(int(std::lround(std::clamp(strength,0.f,1.f)*100)),0,100):0;
    const int detailPercent=std::isfinite(detail)?std::clamp(int(std::lround(std::clamp(detail,0.f,1.f)*100)),0,100):75;
    touchupStrengthSlider->SetValue(percent);touchupDetailSlider->SetValue(detailPercent);
    touchupStrengthLabel->SetLabel(wxString::Format("%d%%",percent));
    touchupDetailLabel->SetLabel(wxString::Format("%d%%",detailPercent));
    touchupStrengthSlider->Enable(!bypassed);touchupDetailSlider->Enable(!bypassed);
    const wxString summary=bypassed?wxString("Bypassed"):percent&&detailPercent<100?wxString::Format("%d%%",percent):wxString("Off");
    // Section::SetValue would recalculate layout on every slider tick. This fixed header only repaints.
    if(touchupSection->header->valueTag!=summary){touchupSection->header->valueTag=summary;touchupSection->header->Refresh();}
}

void Window::SetTouchupStatus(const wxString& status,const wxString& detail)
{
    if(!touchupStatusLabel)return;
    wxString boundedStatus="Unavailable";
    for(const auto& allowed:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off"})
        if(status==allowed){boundedStatus=allowed;break;}
    wxString boundedDetail=detail.Left(240);boundedDetail.Replace("\r"," ");boundedDetail.Replace("\n"," ");boundedDetail.Replace("\t"," ");
    if(touchupStatus==boundedStatus&&touchupStatusDetail==boundedDetail)return;
    touchupStatus=boundedStatus;touchupStatusDetail=boundedDetail;
    touchupStatusLabel->SetLabel("Touch-up: "+boundedStatus);touchupStatusLabel->SetToolTip(boundedDetail);
    // Single-line, fixed-height status does not resize or relayout the inspector.
}

bool Window::VerifyTouchupControls()
{
    if(!touchupSection||!touchupStrengthSlider||!touchupDetailSlider||!touchupStatusLabel)return false;
    bool passed=true;auto check=[&](bool ok){passed=passed&&ok;};
    const int heldStrength=touchupStrengthSlider->GetValue(),heldDetail=touchupDetailSlider->GetValue();
    const bool heldBypass=!touchupStrengthSlider->IsEnabled();
    const auto heldStatus=touchupStatus,heldStatusDetail=touchupStatusDetail;
    const auto statusSize=touchupStatusLabel->GetSize(),statusMin=touchupStatusLabel->GetMinSize(),statusMax=touchupStatusLabel->GetMaxSize();
    const auto strengthMin=touchupStrengthLabel->GetMinSize(),strengthMax=touchupStrengthLabel->GetMaxSize();
    const auto detailMin=touchupDetailLabel->GetMinSize(),detailMax=touchupDetailLabel->GetMaxSize();
    const auto heldLayout=inspectorLayoutRevision;
    check(touchupSection->header->GetLabel()=="Face touch-up"&&!touchupSection->header->expanded);
    for(const auto& title:{"Smoothing strength","Detail preservation"}){
        auto* label=wxWindow::FindWindowByName("Touch-up label: "+wxString(title),touchupSection->body);
        check(label&&label->GetLabel()==title);
    }
    for(int percent:{0,1,37,100}){
        SetTouchupPresentation(percent/100.f,.75f,false);
        check(touchupStrengthSlider->GetValue()==percent&&touchupDetailSlider->GetValue()==75);
        check(touchupStrengthLabel->GetLabel()==wxString::Format("%d%%",percent)&&touchupDetailLabel->GetLabel()=="75%");
        check(touchupStrengthSlider->IsEnabled()&&touchupDetailSlider->IsEnabled()&&inspectorLayoutRevision==heldLayout);
        check(touchupStrengthLabel->GetMinSize()==strengthMin&&touchupStrengthLabel->GetMaxSize()==strengthMax&&
            touchupDetailLabel->GetMinSize()==detailMin&&touchupDetailLabel->GetMaxSize()==detailMax);
    }
    SetTouchupPresentation(.37f,.82f,true);
    check(!touchupStrengthSlider->IsEnabled()&&!touchupDetailSlider->IsEnabled()&&touchupSection->header->valueTag=="Bypassed");
    check(touchupStrengthSlider->GetValue()==37&&touchupDetailSlider->GetValue()==82);
    SetTouchupPresentation(.37f,.82f,false);
    check(touchupStrengthSlider->IsEnabled()&&touchupDetailSlider->IsEnabled()&&touchupSection->header->valueTag=="37%");
    SetTouchupPresentation(.37f,1.f,false);
    check(touchupSection->header->valueTag=="Off"&&touchupStrengthSlider->GetValue()==37&&touchupDetailSlider->GetValue()==100&&
        touchupStrengthSlider->IsEnabled()&&touchupDetailSlider->IsEnabled()&&inspectorLayoutRevision==heldLayout);
    SetTouchupPresentation(-3.f,4.f,false);check(touchupStrengthSlider->GetValue()==0&&touchupDetailSlider->GetValue()==100);
    for(const auto& status:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off"}){
        SetTouchupStatus(status,"Current face service detail");
        check(touchupStatusLabel->GetLabel()=="Touch-up: "+wxString(status)&&touchupStatusLabel->GetToolTipText()=="Current face service detail");
        check(touchupStatusLabel->GetSize()==statusSize&&touchupStatusLabel->GetMinSize()==statusMin&&
            touchupStatusLabel->GetMaxSize()==statusMax&&inspectorLayoutRevision==heldLayout);
    }
    wxString oversized="Line\r\n\t";for(int i=0;i<300;++i)oversized+="bounded ";
    SetTouchupStatus(oversized,oversized);const auto tooltip=touchupStatusLabel->GetToolTipText();
    check(touchupStatusLabel->GetLabel()=="Touch-up: Unavailable"&&tooltip.length()<=240&&
        !tooltip.Contains("\r")&&!tooltip.Contains("\n")&&!tooltip.Contains("\t"));
    SetTouchupStatus(oversized,oversized);check(touchupStatusLabel->GetToolTipText()==tooltip&&inspectorLayoutRevision==heldLayout);
    SetTouchupPresentation(heldStrength/100.f,heldDetail/100.f,heldBypass);SetTouchupStatus(heldStatus,heldStatusDetail);
    logger<<(passed?"[UI-VERIFY] Face touch-up labels/bounds/no-layout PASS":"[UI-VERIFY] Face touch-up labels/bounds/no-layout FAIL")<<std::endl;
    return passed;
}

void Window::SetAccessoryPresentation(bool glasses,bool cap,float scale,bool bypassed,int capStyle,
    int glassesStyle,float capScale,float capVerticalOffset)
{
    if(!accessoryGlassesChoice)return;
    const int percent=std::isfinite(scale)?std::clamp(int(std::lround(std::clamp(scale,.5f,1.5f)*100)),50,150):100;
    const int capPercent=std::isfinite(capScale)?std::clamp(int(std::lround(std::clamp(capScale,.75f,1.5f)*100)),75,150):100;
    const int capHeight=std::isfinite(capVerticalOffset)?std::clamp(int(std::lround((std::clamp(capVerticalOffset,-2.f,3.f)+2.f)*10)),0,50):20;
    cap=cap&&capStyle==2;accessoryCapStyle=2;
    accessoryGlassesStyle=glassesStyle>=0&&glassesStyle<=3?glassesStyle:0;
    accessoryGlassesChoice->SetSelection(glasses?accessoryGlassesStyle+1:0);accessoryCapChoice->SetSelection(cap?1:0);
    accessoryScaleSlider->SetValue(percent);accessoryScaleLabel->SetLabel(wxString::Format("%d%%",percent));
    accessoryCapScaleSlider->SetValue(capPercent);accessoryCapScaleLabel->SetLabel(wxString::Format("%d%%",capPercent));
    accessoryCapHeightSlider->SetValue(capHeight);accessoryCapHeightLabel->SetLabel(wxString::Format("%.1f cm",capHeight/10.f-2.f));
    accessoryGlassesChoice->Enable(!bypassed);accessoryCapChoice->Enable(!bypassed);
    accessoryScaleSlider->Enable(!bypassed);accessoryCapScaleSlider->Enable(!bypassed);accessoryCapHeightSlider->Enable(!bypassed);
    const wxString summary=bypassed?wxString("Bypassed"):glasses&&cap?wxString("Glasses + cap"):
        glasses?wxString("Glasses"):cap?wxString("Cap"):wxString("Off");
    // Selection, fit and service status repaint without recalculating inspector layout.
    if(accessorySection->header->valueTag!=summary){accessorySection->header->valueTag=summary;accessorySection->header->Refresh();}
}

void Window::SetAccessoryStatus(const wxString& status,const wxString& detail)
{
    if(!accessoryStatusLabel)return;
    wxString boundedStatus="Unavailable";
    for(const auto& allowed:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off","Paused"})
        if(status==allowed){boundedStatus=allowed;break;}
    wxString boundedDetail=detail.Left(240);boundedDetail.Replace("\r"," ");boundedDetail.Replace("\n"," ");boundedDetail.Replace("\t"," ");
    if(accessoryStatus==boundedStatus&&accessoryStatusDetail==boundedDetail)return;
    accessoryStatus=boundedStatus;accessoryStatusDetail=boundedDetail;
    accessoryStatusLabel->SetLabel("Accessories: "+boundedStatus);accessoryStatusLabel->SetToolTip(boundedDetail);
}

bool Window::VerifyAccessoryControls()
{
    if(!accessorySection||!accessoryGlassesChoice||!accessoryCapChoice||!accessoryScaleSlider||
        !accessoryCapScaleSlider||!accessoryCapHeightSlider||!accessoryStatusLabel)return false;
    bool passed=true;auto check=[&](bool ok,const char* reason){if(!ok){passed=false;logger<<"[UI-VERIFY] Face accessories failure: "<<reason<<std::endl;}};
    const bool heldGlasses=accessoryGlassesChoice->GetSelection()>0,heldCap=accessoryCapChoice->GetSelection()>0;
    const int heldScale=accessoryScaleSlider->GetValue(),heldCapScale=accessoryCapScaleSlider->GetValue(),heldHeight=accessoryCapHeightSlider->GetValue();
    const int heldCapStyle=accessoryCapStyle,heldGlassesStyle=accessoryGlassesStyle;
    const bool heldBypass=!accessoryScaleSlider->IsEnabled();const auto heldStatus=accessoryStatus,heldDetail=accessoryStatusDetail;
    const auto heldLayout=inspectorLayoutRevision;
    const auto statusSize=accessoryStatusLabel->GetSize(),statusMin=accessoryStatusLabel->GetMinSize(),statusMax=accessoryStatusLabel->GetMaxSize();
    auto stableFitBounds=[&]{
        return accessoryScaleLabel->GetMinSize()==FromDIP(wxSize(44,18))&&accessoryScaleLabel->GetMaxSize()==FromDIP(wxSize(44,18))&&
            accessoryCapScaleLabel->GetMinSize()==FromDIP(wxSize(44,18))&&accessoryCapScaleLabel->GetMaxSize()==FromDIP(wxSize(44,18))&&
            accessoryCapHeightLabel->GetMinSize()==FromDIP(wxSize(64,18))&&accessoryCapHeightLabel->GetMaxSize()==FromDIP(wxSize(64,18));
    };
    check(accessorySection->header->GetLabel()=="Face accessories"&&!accessorySection->header->expanded,"card is titled and initially collapsed");
    const wxString glassesNames[]={"Off","Classic Wayfarer","Gold Teardrop Aviator","Vintage Round Tortoise","Chic Cat-Eye Emerald"};
    check(accessoryGlassesChoice->GetCount()==5,"all four reference glasses are selectable");
    for(unsigned i=0;i<5&&i<accessoryGlassesChoice->GetCount();++i)
        check(accessoryGlassesChoice->GetString(i)==glassesNames[i],"glasses labels match reference styles");
    check(accessoryCapChoice->GetCount()==2&&accessoryCapChoice->GetString(0)=="Off"&&
        accessoryCapChoice->GetString(1)=="Pokemon Masters cap","only the accepted cap is selectable");
    for(const auto& title:{"Glasses","Cap","Glasses size","Cap size","Cap height"}){
        auto* label=wxWindow::FindWindowByName("Accessory label: "+wxString(title),accessorySection->body);
        check(label&&label->GetLabel()==title,"plain visible independent fit labels");
    }
    check(accessoryScaleSlider->GetMin()==50&&accessoryScaleSlider->GetMax()==150&&
        accessoryCapScaleSlider->GetMin()==75&&accessoryCapScaleSlider->GetMax()==150&&
        accessoryCapHeightSlider->GetMin()==0&&accessoryCapHeightSlider->GetMax()==50,"independent native fit ranges");
    // Intercept actual control routes so verification does not save settings or start tracking.
    wxEvtHandler route;int routed=wxID_NONE;
    route.Bind(wxEVT_MENU,[&](wxCommandEvent& event){routed=event.GetId();},ACCESSORY_GLASSES,ACCESSORY_CAP_HEIGHT);
    PushEventHandler(&route);
    auto fire=[&](wxWindow* control,wxEventType type,int command){
        routed=wxID_NONE;wxCommandEvent event(type,control->GetId());event.SetEventObject(control);
        control->GetEventHandler()->ProcessEvent(event);check(routed==command,"assigned native event dispatch");
    };
    for(int style=0;style<4;++style)for(int combination=0;combination<4;++combination)for(int fit=0;fit<3;++fit){
        const bool glasses=(combination&1)!=0,cap=(combination&2)!=0;
        const int percent=fit==0?50:fit==1?100:150,capPercent=fit==0?150:fit==1?75:100,height=fit==0?50:fit==1?0:20;
        SetAccessoryPresentation(glasses,cap,percent/100.f,false,2,style,capPercent/100.f,height/10.f-2.f);
        check(accessoryGlassesChoice->GetSelection()==(glasses?style+1:0)&&accessoryGlassesStyle==style&&
            accessoryCapChoice->GetSelection()==int(cap)&&accessoryCapStyle==2&&accessoryScaleSlider->GetValue()==percent&&
            accessoryCapScaleSlider->GetValue()==capPercent&&accessoryCapHeightSlider->GetValue()==height&&
            accessoryScaleLabel->GetLabel()==wxString::Format("%d%%",percent)&&
            accessoryCapScaleLabel->GetLabel()==wxString::Format("%d%%",capPercent)&&
            accessoryCapHeightLabel->GetLabel()==wxString::Format("%.1f cm",height/10.f-2.f),"four styles and independent off/on fit presentation");
        fire(accessoryGlassesChoice,wxEVT_CHOICE,ACCESSORY_GLASSES);fire(accessoryCapChoice,wxEVT_CHOICE,ACCESSORY_CAP);
        fire(accessoryScaleSlider,wxEVT_SLIDER,ACCESSORY_SCALE);fire(accessoryCapScaleSlider,wxEVT_SLIDER,ACCESSORY_CAP_SCALE);
        fire(accessoryCapHeightSlider,wxEVT_SLIDER,ACCESSORY_CAP_HEIGHT);
        check(inspectorLayoutRevision==heldLayout&&stableFitBounds(),"repeated control presentation retains fixed inspector geometry");
    }
    PopEventHandler(false);
    SetAccessoryPresentation(true,true,1.37f,true,2,3,1.23f,-.7f);
    check(!accessoryGlassesChoice->IsEnabled()&&!accessoryCapChoice->IsEnabled()&&!accessoryScaleSlider->IsEnabled()&&
        !accessoryCapScaleSlider->IsEnabled()&&!accessoryCapHeightSlider->IsEnabled()&&accessorySection->header->valueTag=="Bypassed"&&
        accessoryGlassesChoice->GetSelection()==4&&accessoryCapChoice->GetSelection()==1&&accessoryScaleSlider->GetValue()==137&&
        accessoryCapScaleSlider->GetValue()==123&&accessoryCapHeightSlider->GetValue()==13,"bypass disables all choices and independent fits without resetting them");
    SetAccessoryPresentation(true,true,1.37f,false,2,3,1.23f,-.7f);
    check(accessoryGlassesChoice->IsEnabled()&&accessoryCapChoice->IsEnabled()&&accessoryScaleSlider->IsEnabled()&&
        accessoryCapScaleSlider->IsEnabled()&&accessoryCapHeightSlider->IsEnabled()&&accessorySection->header->valueTag=="Glasses + cap"&&
        accessoryGlassesChoice->GetSelection()==4&&accessoryCapChoice->GetSelection()==1&&accessoryScaleSlider->GetValue()==137&&
        accessoryCapScaleSlider->GetValue()==123&&accessoryCapHeightSlider->GetValue()==13,"bypass restores each style and fit");
    SetAccessoryPresentation(false,false,1.37f,false,2,3,1.23f,-.7f);
    check(accessoryGlassesChoice->GetSelection()==0&&accessoryCapChoice->GetSelection()==0&&accessoryGlassesStyle==3&&accessoryCapStyle==2&&
        accessoryScaleSlider->GetValue()==137&&accessoryCapScaleSlider->GetValue()==123&&accessoryCapHeightSlider->GetValue()==13,"Off retains both styles and independent fitting");
    SetAccessoryPresentation(true,true,1.f,false,accessoryCapStyle,accessoryGlassesStyle,1.f,0.f);
    check(accessoryGlassesChoice->GetSelection()==4&&accessoryCapChoice->GetSelection()==1&&accessoryScaleSlider->GetValue()==100&&
        accessoryCapScaleSlider->GetValue()==100&&accessoryCapHeightSlider->GetValue()==20,"neutral fit reset preserves selected styles");
    for(int invalidStyle:{-1,0,1,3}){
        SetAccessoryPresentation(false,true,1.f,false,invalidStyle);
        check(accessoryCapChoice->GetSelection()==0&&accessoryCapStyle==2&&accessorySection->header->valueTag=="Off","retired and invalid cap styles stay off");
    }
    for(int invalidStyle:{-1,4}){
        SetAccessoryPresentation(true,false,1.f,false,2,invalidStyle);
        check(accessoryGlassesChoice->GetSelection()==1&&accessoryGlassesStyle==0,"invalid glasses style uses the default reference frame");
    }
    SetAccessoryPresentation(false,false,-3.f,false,2,3,-3.f,-9.f);
    check(accessoryScaleSlider->GetValue()==50&&accessoryCapScaleSlider->GetValue()==75&&accessoryCapHeightSlider->GetValue()==0,"low fits clamp independently");
    SetAccessoryPresentation(true,true,4.f,false,2,3,4.f,9.f);
    check(accessoryScaleSlider->GetValue()==150&&accessoryCapScaleSlider->GetValue()==150&&accessoryCapHeightSlider->GetValue()==50,"high fits clamp independently");
    SetAccessoryPresentation(true,true,std::nanf(""),false,2,3,std::nanf(""),std::nanf(""));
    check(accessoryScaleSlider->GetValue()==100&&accessoryCapScaleSlider->GetValue()==100&&accessoryCapHeightSlider->GetValue()==20,"nonfinite fits have neutral defaults");
    for(const auto& status:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off"}){
        SetAccessoryStatus(status,"Current face service detail");
        check(accessoryStatusLabel->GetLabel()=="Accessories: "+wxString(status)&&accessoryStatusLabel->GetToolTipText()=="Current face service detail",
            "service state is displayed without invented tracking");
        check(accessoryStatusLabel->GetSize()==statusSize&&accessoryStatusLabel->GetMinSize()==statusMin&&accessoryStatusLabel->GetMaxSize()==statusMax&&
            inspectorLayoutRevision==heldLayout&&stableFitBounds(),"status and fit values retain fixed sizes without layout");
    }
    wxString oversized="Line\r\n\t";for(int i=0;i<300;++i)oversized+="bounded ";
    SetAccessoryStatus(oversized,oversized);const auto tooltip=accessoryStatusLabel->GetToolTipText();
    check(accessoryStatusLabel->GetLabel()=="Accessories: Unavailable"&&tooltip.length()<=240&&!tooltip.Contains("\r")&&
        !tooltip.Contains("\n")&&!tooltip.Contains("\t"),"unknown state and long detail are bounded");
    SetAccessoryStatus(oversized,oversized);check(accessoryStatusLabel->GetToolTipText()==tooltip&&inspectorLayoutRevision==heldLayout,"unchanged status avoids layout");
    SetAccessoryPresentation(heldGlasses,heldCap,heldScale/100.f,heldBypass,heldCapStyle,heldGlassesStyle,heldCapScale/100.f,heldHeight/10.f-2.f);
    SetAccessoryStatus(heldStatus,heldDetail);
    logger<<(passed?"[UI-VERIFY] Face accessories labels/bounds/events/no-layout PASS":"[UI-VERIFY] Face accessories labels/bounds/events/no-layout FAIL")<<std::endl;
    return passed;
}


void Window::SetFaceFilterPresentation(int style,float scale,float verticalOffset,bool bypassed)
{
    if(!faceFilterChoice)return;
    style=style>=0&&style<=5?style:0;
    const int percent=std::isfinite(scale)?int(std::lround(std::clamp(scale,.75f,1.5f)*100)):100;
    const int height=std::isfinite(verticalOffset)?int(std::lround((std::clamp(verticalOffset,-3.f,3.f)+3.f)*10)):30;
    faceFilterChoice->SetSelection(style);faceFilterScaleSlider->SetValue(percent);faceFilterHeightSlider->SetValue(height);
    faceFilterScaleLabel->SetLabel(wxString::Format("%d%%",percent));faceFilterHeightLabel->SetLabel(wxString::Format("%.1f cm",height/10.f-3.f));
    faceFilterChoice->Enable(!bypassed);faceFilterScaleSlider->Enable(!bypassed);faceFilterHeightSlider->Enable(!bypassed);
    const wxString summary=bypassed?wxString("Bypassed"):style==0?wxString("Off"):style==1?wxString("Fox"):style==2?wxString("Owl"):style==3?wxString("Explorer"):style==4?wxString("Local"):wxString("Crown");
    if(faceFilterSection->header->valueTag!=summary){faceFilterSection->header->valueTag=summary;faceFilterSection->header->Refresh();}
}
void Window::SetLocalFaceFilterPresentation(const wxString& name,const PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit& fit,bool available,bool busy,bool bypassed)
{
    if(!faceFilterLocalName)return;faceFilterLocalAvailable=available;faceFilterImportBusy=busy;
    wxString display=name.Left(100);display.Replace("\r"," ");display.Replace("\n"," ");
    faceFilterLocalName->SetLabel(busy?wxString("Importing model; camera stays live..."):available?wxString("Local: ")+display:wxString("No local model imported"));
    faceFilterImportButton->Enable(!busy&&!bypassed);faceFilterRemoveButton->Enable(available&&!busy&&!bypassed);
    faceFilterChoice->Enable(!bypassed&&!busy);faceFilterScaleSlider->Enable(!bypassed&&!busy);faceFilterHeightSlider->Enable(!bypassed&&!busy);
    const float values[]={fit.xCm,fit.yCm,fit.zCm,fit.tiltDegrees,fit.turnDegrees,fit.rollDegrees};
    for(size_t axis=0;axis<6;++axis){const int limit=axis<3?200:180;const float factor=axis<3?10.f:1.f;
        const int value=std::isfinite(values[axis])?int(std::lround(std::clamp(values[axis]*factor,float(-limit),float(limit)))):0;
        faceFilterCustomSliders[axis]->SetValue(value+limit);faceFilterCustomSliders[axis]->Enable(!bypassed&&!busy);
        faceFilterCustomLabels[axis]->SetLabel(axis<3?wxString::Format("%.1f cm",value/10.f):wxString::Format("%d deg",value));}
    faceFilterOcclusionChoice->SetSelection(fit.headOcclusion?0:1);faceFilterOcclusionChoice->Enable(!bypassed&&!busy);faceFilterResetButton->Enable(!bypassed&&!busy);
    const bool show=faceFilterChoice->GetSelection()==4;
    if(faceFilterCustomPanel->IsShown()!=show){faceFilterCustomPanel->Show(show);UpdateInspectorLayout();}
}
PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit Window::GetLocalFaceFilterFit() const
{
    PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit fit;
    float* values[]={&fit.xCm,&fit.yCm,&fit.zCm,&fit.tiltDegrees,&fit.turnDegrees,&fit.rollDegrees};
    for(size_t axis=0;axis<6;++axis)if(faceFilterCustomSliders[axis])*values[axis]=(faceFilterCustomSliders[axis]->GetValue()-(axis<3?200:180))/(axis<3?10.f:1.f);
    fit.headOcclusion=!faceFilterOcclusionChoice||faceFilterOcclusionChoice->GetSelection()!=1;return fit;
}
void Window::SetModelPerformancePresentation(const PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings& settings,
    const wxString& summary,bool busy,bool bypassed)
{
    if(!modelPerformanceChoice)return;
    (void)bypassed;modelPerformanceBusy=busy;
    const int mode=std::clamp(int(settings.mode),0,4);modelPerformanceChoice->SetSelection(mode);modelPerformanceChoice->Enable(!busy);
    const int values[]={int(std::clamp(settings.customNodes,512u,16384u)),int(std::clamp(settings.customVertices,1000u,1000000u)/1000u),
        int(std::clamp(settings.customTriangles,1000u,1000000u)/1000u),int(std::clamp(settings.customRenderEdge,256u,1536u))};
    for(size_t field=0;field<modelPerformanceSliders.size();++field){
        modelPerformanceSliders[field]->SetValue(values[field]);modelPerformanceSliders[field]->Enable(!busy);
        const int displayed=field==1||field==2?values[field]*1000:values[field];
        modelPerformanceLabels[field]->SetLabel(field==3?wxString::Format("%d px",displayed):wxString::Format("%d",displayed));
    }
    wxString text=summary.Left(240);text.Replace("\r"," ");text.Replace("\n"," ");text.Replace("\t"," ");
    if(text.empty())text="Choose a model performance budget.";
    if(modelPerformanceSummary->GetLabel()!=text){modelPerformanceSummary->SetLabel(text);modelPerformanceSummary->Wrap(FromDIP(240));}
    const bool show=mode==int(PrismForge::Effects::Overlays::Realistic::ModelPerformanceMode::Custom);
    if(modelPerformanceCustomPanel->IsShown()!=show){modelPerformanceCustomPanel->Show(show);UpdateInspectorLayout();}
}
PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings Window::GetModelPerformanceSettings() const
{
    PrismForge::Effects::Overlays::Realistic::ModelPerformanceSettings settings;
    if(!modelPerformanceChoice)return settings;
    settings.mode=PrismForge::Effects::Overlays::Realistic::ModelPerformanceMode(std::clamp(modelPerformanceChoice->GetSelection(),0,4));
    settings.customNodes=uint32_t(modelPerformanceSliders[0]->GetValue());settings.customVertices=uint32_t(modelPerformanceSliders[1]->GetValue())*1000u;
    settings.customTriangles=uint32_t(modelPerformanceSliders[2]->GetValue())*1000u;settings.customRenderEdge=uint32_t(modelPerformanceSliders[3]->GetValue());return settings;
}
void Window::SetFaceFilterStatus(const wxString& status,const wxString& detail)
{
    if(!faceFilterStatusLabel)return;
    wxString bounded="Unavailable";
    for(const auto& allowed:{"Loading","Tracking","No face","Waiting for current frame","Paused","Unavailable","Off"})
        if(status==allowed){bounded=allowed;break;}
    wxString note=detail.Left(240);note.Replace("\r"," ");note.Replace("\n"," ");note.Replace("\t"," ");
    if(faceFilterStatus==bounded&&faceFilterStatusDetail==note)return;
    faceFilterStatus=bounded;faceFilterStatusDetail=note;faceFilterStatusLabel->SetLabel("3D filter: "+bounded);faceFilterStatusLabel->SetToolTip(note);
}
bool Window::VerifyFaceFilterControls()
{
    if(!faceFilterChoice||!faceFilterScaleSlider||!faceFilterHeightSlider||!faceFilterStatusLabel||!modelPerformanceChoice||!modelPerformanceSummary||!modelPerformanceCustomPanel)return false;
    const int heldStyle=faceFilterChoice->GetSelection(),heldScale=faceFilterScaleSlider->GetValue(),heldHeight=faceFilterHeightSlider->GetValue();
    const bool heldBypass=!faceFilterChoice->IsEnabled();const auto heldStatus=faceFilterStatus,heldDetail=faceFilterStatusDetail;
    bool passed=true;auto check=[&](bool ok,const char* why){if(!ok){passed=false;logger<<"[UI-VERIFY] 3D face filters controls failure: "<<why<<std::endl;}};
    check(faceFilterChoice->GetCount()==6,"Off, three original models, local model and crown available");
    const char* names[]={"Off","Aurora fox mask","Nocturne owl mask","Atlas explorer helmet","Local model","Royal crown"};
    for(int style=0;style<6;++style){SetFaceFilterPresentation(style,1.23f,-.7f,false);check(faceFilterChoice->GetStringSelection()==names[style]&&faceFilterScaleSlider->GetValue()==123&&faceFilterHeightSlider->GetValue()==23,"model and independent fit shown");}
    SetFaceFilterPresentation(3,1.23f,-.7f,true);check(!faceFilterChoice->IsEnabled()&&!faceFilterScaleSlider->IsEnabled()&&!faceFilterHeightSlider->IsEnabled()&&faceFilterChoice->GetSelection()==3,"bypass retains selection and fit");
    SetFaceFilterPresentation(-1,-4.f,-9.f,false);check(faceFilterChoice->GetSelection()==0&&faceFilterScaleSlider->GetValue()==75&&faceFilterHeightSlider->GetValue()==0,"invalid style and low fit bounded");
    SetFaceFilterPresentation(8,9.f,9.f,false);check(faceFilterChoice->GetSelection()==0&&faceFilterScaleSlider->GetValue()==150&&faceFilterHeightSlider->GetValue()==60,"high fit bounded");
    SetFaceFilterPresentation(1,std::nanf(""),std::nanf(""),false);check(faceFilterScaleSlider->GetValue()==100&&faceFilterHeightSlider->GetValue()==30,"nonfinite fit resets");
    for(const auto& status:{"Loading","Tracking","No face","Waiting for current frame","Paused","Unavailable","Off"}){SetFaceFilterStatus(status,"Current frame detail");check(faceFilterStatusLabel->GetLabel()=="3D filter: "+wxString(status),"honest bounded effect status");}
    SetFaceFilterStatus("Unknown",wxString(300,'x')+"\n");check(faceFilterStatusLabel->GetLabel()=="3D filter: Unavailable"&&faceFilterStatusLabel->GetToolTipText().length()<=240,"unknown or long status bounded");
    const bool heldAvailable=faceFilterLocalAvailable,heldBusy=faceFilterImportBusy;const auto heldLocalFit=GetLocalFaceFilterFit();const auto heldName=faceFilterLocalName->GetLabel();
    PrismForge::Effects::Overlays::Realistic::CustomFaceFilterFit fit;fit.xCm=-12.3f;fit.yCm=8.2f;fit.zCm=4.5f;fit.tiltDegrees=-35;fit.turnDegrees=92;fit.rollDegrees=14;fit.headOcclusion=false;
    SetFaceFilterPresentation(4,1.23f,-.7f,false);SetLocalFaceFilterPresentation("Test local model",fit,true,false,false);const auto shown=GetLocalFaceFilterFit();
    check(faceFilterCustomPanel->IsShown()&&faceFilterChoice->GetStringSelection()=="Local model"&&faceFilterImportButton->IsEnabled()&&faceFilterRemoveButton->IsEnabled(),"local model exposes fitting and owned import/remove controls");
    check(std::abs(shown.xCm-fit.xCm)<.001f&&std::abs(shown.yCm-fit.yCm)<.001f&&std::abs(shown.zCm-fit.zCm)<.001f&&shown.tiltDegrees==fit.tiltDegrees&&shown.turnDegrees==fit.turnDegrees&&shown.rollDegrees==fit.rollDegrees&&!shown.headOcclusion,"all six head-local controls and occlusion shown");
    SetLocalFaceFilterPresentation("Test local model",fit,true,true,false);check(!faceFilterChoice->IsEnabled()&&!faceFilterImportButton->IsEnabled()&&!faceFilterRemoveButton->IsEnabled()&&!faceFilterResetButton->IsEnabled(),"active worker excludes competing product changes");
    SetLocalFaceFilterPresentation("Test local model",fit,true,false,true);check(!faceFilterCustomSliders[0]->IsEnabled()&&!faceFilterOcclusionChoice->IsEnabled(),"bypass preserves and disables local fitting");
    SetFaceFilterPresentation(5,1.23f,-.7f,false);SetLocalFaceFilterPresentation("Test local model",fit,true,false,false);
    check(!faceFilterCustomPanel->IsShown()&&faceFilterChoice->GetSelection()==5&&faceFilterChoice->GetStringSelection()=="Royal crown"&&faceFilterSection->header->valueTag=="Crown", "crown retains independent standard fit and compact bundled controls");
    const auto retainedLocalFit=GetLocalFaceFilterFit();check(retainedLocalFit.xCm==shown.xCm&&retainedLocalFit.turnDegrees==shown.turnDegrees&&retainedLocalFit.headOcclusion==shown.headOcclusion&&faceFilterRemoveButton->IsEnabled(), "crown selection preserves owned local model and fit");
    SetFaceFilterPresentation(1,1.f,0.f,false);SetLocalFaceFilterPresentation({},fit,true,false,false);check(!faceFilterCustomPanel->IsShown(),"bundled models keep compact controls");
    const auto heldPerformance=GetModelPerformanceSettings();const auto heldRecommendation=modelPerformanceSummary->GetLabel();const bool heldPerformanceBusy=modelPerformanceBusy;
    // Intercept native menu routes so this UI check never changes saved settings or import work.
    wxEvtHandler performanceRoute;int performanceEvents=0;
    performanceRoute.Bind(wxEVT_MENU,[&](wxCommandEvent&){++performanceEvents;},FACE_FILTER_PERFORMANCE);
    PushEventHandler(&performanceRoute);
    check(modelPerformanceChoice->GetCount()==5,"all five model performance modes available");
    const char* performanceNames[]={"Auto","Performance","Balanced","Quality","Custom"};
    auto performance=heldPerformance;performance.customNodes=8192;performance.customVertices=345000;performance.customTriangles=678000;performance.customRenderEdge=1280;
    for(int mode=0;mode<5;++mode){
        performance.mode=PrismForge::Effects::Overlays::Realistic::ModelPerformanceMode(mode);
        SetModelPerformancePresentation(performance,"Hardware recommendation: Balanced",false,false);const auto roundtrip=GetModelPerformanceSettings();
        check(modelPerformanceChoice->GetStringSelection()==performanceNames[mode]&&int(roundtrip.mode)==mode,"model performance selection reflected");
        check(modelPerformanceCustomPanel->IsShown()==(mode==4),"custom budgets visible only in Custom mode");
        check(roundtrip.customNodes==8192&&roundtrip.customVertices==345000&&roundtrip.customTriangles==678000&&roundtrip.customRenderEdge==1280,"all four budget values roundtrip in native units");
    }
    check(modelPerformanceLabels[0]->GetLabel()=="8192"&&modelPerformanceLabels[1]->GetLabel()=="345000"&&modelPerformanceLabels[2]->GetLabel()=="678000"&&modelPerformanceLabels[3]->GetLabel()=="1280 px","budget numbers and render units readable");
    check(modelPerformanceSliders[0]->GetMin()==512&&modelPerformanceSliders[0]->GetMax()==16384&&
        modelPerformanceSliders[1]->GetMin()==1&&modelPerformanceSliders[1]->GetMax()==1000&&
        modelPerformanceSliders[2]->GetMin()==1&&modelPerformanceSliders[2]->GetMax()==1000&&
        modelPerformanceSliders[3]->GetMin()==256&&modelPerformanceSliders[3]->GetMax()==1536,"native slider ranges enforce model budgets");
    const auto heldPerformanceLayout=inspectorLayoutRevision;
    SetModelPerformancePresentation(performance,"Hardware recommendation: Balanced",true,false);
    check(!modelPerformanceChoice->IsEnabled()&&std::all_of(modelPerformanceSliders.begin(),modelPerformanceSliders.end(),[](auto* slider){return !slider->IsEnabled();}),"import worker disables competing budget changes");
    SetModelPerformancePresentation(performance,"Hardware recommendation: Balanced",false,true);
    check(modelPerformanceChoice->IsEnabled()&&std::all_of(modelPerformanceSliders.begin(),modelPerformanceSliders.end(),[](auto* slider){return slider->IsEnabled();}),"bypass still allows model budget configuration");
    check(performanceEvents==0,"performance presentation setter never dispatches product changes");
    check(inspectorLayoutRevision==heldPerformanceLayout,"unchanged Custom visibility avoids layout during budget presentation");
    auto firePerformance=[&](wxWindow* control,wxEventType type){const int before=performanceEvents;wxCommandEvent event(type,control->GetId());event.SetEventObject(control);control->GetEventHandler()->ProcessEvent(event);check(performanceEvents==before+1,"real performance control dispatches its assigned command once");};
    firePerformance(modelPerformanceChoice,wxEVT_CHOICE);for(auto* slider:modelPerformanceSliders)firePerformance(slider,wxEVT_SLIDER);
    SetModelPerformancePresentation(heldPerformance,heldRecommendation,heldPerformanceBusy,heldBypass);
    const auto restoredPerformance=GetModelPerformanceSettings();check(restoredPerformance.mode==heldPerformance.mode&&restoredPerformance.customNodes==heldPerformance.customNodes&&restoredPerformance.customVertices==heldPerformance.customVertices&&restoredPerformance.customTriangles==heldPerformance.customTriangles&&restoredPerformance.customRenderEdge==heldPerformance.customRenderEdge&&modelPerformanceBusy==heldPerformanceBusy,"model performance controls restore their held state");
    PopEventHandler(false);
    SetFaceFilterPresentation(heldStyle,heldScale/100.f,heldHeight/10.f-3.f,heldBypass);SetFaceFilterStatus(heldStatus,heldDetail);
    SetLocalFaceFilterPresentation(heldName.StartsWith("Local: ")?heldName.Mid(7):wxString{},heldLocalFit,heldAvailable,heldBusy,heldBypass);
    logger<<(passed?"[UI-VERIFY] 3D face filters controls PASS":"[UI-VERIFY] 3D face filters controls FAIL")<<std::endl;
    return passed;
}

void Window::SetFaceLightingStatus(const wxString& status,const wxString& detail)
{
    if(!faceLightingStatusLabel)return;
    wxString boundedStatus="Unavailable";
    for(const auto& allowed:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off"})
        if(status==allowed){boundedStatus=allowed;break;}
    wxString boundedDetail=detail.Left(240);
    boundedDetail.Replace("\r"," ");boundedDetail.Replace("\n"," ");boundedDetail.Replace("\t"," ");
    if(faceLightingStatus==boundedStatus&&faceLightingDetail==boundedDetail)return;
    faceLightingStatus=boundedStatus;faceLightingDetail=boundedDetail;
    faceLightingStatusLabel->SetLabel("Face lighting: "+boundedStatus);
    faceLightingStatusLabel->SetToolTip(boundedDetail);
    // This single-line status changes neither card height nor inspector layout.
}

bool Window::VerifyFaceLightingStatus()
{
    if(!faceLightingStatusLabel)return false;
    const auto heldStatus=faceLightingStatus,heldDetail=faceLightingDetail;
    const auto heldSize=faceLightingStatusLabel->GetSize();
    const auto heldMinSize=faceLightingStatusLabel->GetMinSize();
    const auto heldMaxSize=faceLightingStatusLabel->GetMaxSize();
    const auto heldLayout=inspectorLayoutRevision;
    bool passed=true;auto check=[&](bool ok){passed=passed&&ok;};
    for(const auto& state:{"Loading","Tracking","No face","Waiting for current frame","Unavailable","Off"}){
        SetFaceLightingStatus(state,"Current face service detail");
        check(faceLightingStatusLabel->GetLabel()=="Face lighting: "+wxString(state));
        check(faceLightingStatusLabel->GetToolTipText()=="Current face service detail");
        check(faceLightingStatusLabel->GetSize()==heldSize&&faceLightingStatusLabel->GetMinSize()==heldMinSize&&
            faceLightingStatusLabel->GetMaxSize()==heldMaxSize&&inspectorLayoutRevision==heldLayout);
    }
    wxString longDetail="Line\r\n\t";for(int i=0;i<300;++i)longDetail+="bounded ";
    SetFaceLightingStatus(longDetail,longDetail);
    check(faceLightingStatusLabel->GetLabel()=="Face lighting: Unavailable");
    const auto tooltip=faceLightingStatusLabel->GetToolTipText();
    check(tooltip.length()<=240&&!tooltip.Contains("\r")&&!tooltip.Contains("\n")&&!tooltip.Contains("\t"));
    const auto repeatedLabel=faceLightingStatusLabel->GetLabel();
    SetFaceLightingStatus(longDetail,longDetail);
    check(faceLightingStatusLabel->GetLabel()==repeatedLabel&&faceLightingStatusLabel->GetToolTipText()==tooltip&&
        faceLightingStatusLabel->GetSize()==heldSize&&inspectorLayoutRevision==heldLayout);
    SetFaceLightingStatus(heldStatus,heldDetail);
    logger<<(passed?"[UI-VERIFY] Face lighting status states/bounds/layout PASS":"[UI-VERIFY] Face lighting status states/bounds/layout FAIL")<<std::endl;
    return passed;
}

bool Window::VerifyCustomLightingControls()
{
    bool labelsPassed=true;
    for(const auto& name:{"Key light (1) brightness","Key light (1) temperature","Fill light (2) brightness","Fill light (2) temperature"}){
        auto* label=wxWindow::FindWindowByName("Studio label: "+wxString(name),customLightPanel);
        labelsPassed=labelsPassed&&label&&label->GetLabel()==name;
    }
    logger<<(labelsPassed?"[UI-VERIFY] Light control labels PASS":"[UI-VERIFY] Light control labels FAIL")<<std::endl;
    wxString failure;
    const bool statusPassed=VerifyFaceLightingStatus();
    const bool passed=studioLightPositionPad->VerifyInteractionForTesting(&failure)&&labelsPassed&&statusPassed;
    if(!passed)logger<<"[UI-VERIFY] Light position control failure: "<<failure<<std::endl;
    logger<<(passed?"[UI-VERIFY] Light position control PASS":"[UI-VERIFY] Light position control FAIL")<<std::endl;
    return passed;
}

void Window::SetLookPresentation(int selection, bool bypassed)
{
    lookChoice->SetSelection(std::clamp(selection, 0, 3));
    lookSection->SetValue(bypassed ? wxString("Bypassed") : lookChoice->GetStringSelection());
    lookBypassButton->selected = bypassed;
    lookBypassButton->SetLabel(bypassed ? "Restore effects" : "Bypass effects");
    lookBypassButton->SetName(lookBypassButton->GetLabel());
    lookBypassButton->Refresh();
    UpdateInspectorLayout();
}

void Window::SetLutPresentation(const wxString& name, int intensity)
{
    const bool loaded = !name.empty();
    const wxString label = loaded ? name.Left(100) : wxString("No LUT loaded");
    if (lutNameLabel->GetLabel() != label) {
        lutNameLabel->SetLabel(label);
        lutNameLabel->Wrap(FromDIP(240));
        lutNameLabel->SetToolTip(loaded ? name : wxString("Load a local 3D .cube file."));
    }
    lutIntensitySlider->Enable(loaded);
    lutClearButton->Enable(loaded);
    lutIntensitySlider->SetValue(std::clamp(intensity, 0, 100));
    lutIntensityLabel->SetLabel(wxString::Format("%d%%", lutIntensitySlider->GetValue()));
    UpdateInspectorLayout();
}

void Window::ShowCommands()
{
    Studio::GlassDialogBase dialog(this, wxID_ANY, "Search actions", wxDefaultPosition, FromDIP(wxSize(500,440)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetMinSize(FromDIP(wxSize(380,320)));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* search = new wxSearchCtrl(&dialog, wxID_ANY); search->SetDescriptiveText("Type an action...");
    auto* list = new wxListBox(&dialog,wxID_ANY);
    struct Action { wxString title; int id; bool needsCamera; };
    const std::vector<Action> actions = {{"Connect a phone    Ctrl+N",MenuIDs::QR,false},{"Show / hide controls    Ctrl+,",CONTROLS,false},
        {"Reconnect phone    Ctrl+R",RECONNECT,true},{"Take snapshot    Ctrl+Shift+S",SNAPSHOT_ACTION,true},
        {"Open snapshots folder",SNAPSHOTS,false},{"Save quality profile",PRESET_SAVE,true},{"Import quality profile",PRESET_IMPORT,false},
        {"Load custom LUT",LUT_IMPORT,false},{"Clear custom LUT",LUT_CLEAR,false},{"Effects packs",PACK_LIBRARY,false},
        {"Load background image",BACKGROUND_IMPORT,false},{"Clear background image",BACKGROUND_CLEAR,false},
        {"Choose background colour",BACKGROUND_COLOUR,false},
        {"Bypass / restore desktop effects",LOOK_BYPASS,false},{"Save desktop look preset",LOOK_PRESET_SAVE,false},
        {"Import desktop look preset",LOOK_PRESET_IMPORT,false},{"Export desktop look preset",LOOK_PRESET_EXPORT,false},
        {"Export current quality profile",PRESET_EXPORT,true},{"Export diagnostics",DIAGNOSTICS,false},{"Connected devices",MenuIDs::DEVICES,false},
        {"Full screen    F11",FOCUS_PREVIEW,false},{"Dark appearance",THEME_DARK,false},{"Light appearance",THEME_LIGHT,false},{"Glass appearance",THEME_GLASS,false},{"Follow system appearance",THEME_SYSTEM,false}};
    std::vector<int> ids;
    auto filter = [&] { list->Clear(); ids.clear(); for (const auto& a : actions) if ((!a.needsCamera || connected) && a.title.Lower().Contains(search->GetValue().Lower())) { list->Append(a.title); ids.push_back(a.id); } if (!ids.empty()) list->SetSelection(0); };
    int selected = -1;
    auto run = [&] { int n = list->GetSelection(); if (n >= 0 && n < (int)ids.size()) { selected = ids[n]; dialog.EndModal(wxID_OK); } };
    search->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { filter(); });
    list->Bind(wxEVT_LISTBOX_DCLICK, [&](wxCommandEvent&) { run(); });
    dialog.Bind(wxEVT_CHAR_HOOK,[&](wxKeyEvent& e) { if(e.GetKeyCode()==WXK_RETURN) run(); else if(e.GetKeyCode()==WXK_DOWN && !ids.empty()) { list->SetFocus(); list->SetSelection(std::min((int)ids.size()-1,list->GetSelection()+1)); } else e.Skip(); });
    sizer->Add(search,0,wxEXPAND|wxALL,FromDIP(18)); sizer->Add(list,1,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(18));
    sizer->Add(Studio::Label(&dialog,"Enter to run | Esc to close",9,true),0,wxALL,FromDIP(18));
    dialog.SetSizer(sizer); dialog.CenterOnParent(); filter(); search->SetFocus();
    if(dialog.ShowModal()==wxID_OK && selected>=0) Dispatch(selected);
}

VCamTaskBarIcon::VCamTaskBarIcon(Window* window)
	: parentWindow(window)
{
	Bind(wxEVT_TASKBAR_LEFT_UP, &VCamTaskBarIcon::OnLeftClick, this);
	Bind(wxEVT_TASKBAR_LEFT_DCLICK, &VCamTaskBarIcon::OnLeftClick, this);
	Bind(wxEVT_MENU, &VCamTaskBarIcon::OnMenuOpen, this, ID_TRAY_OPEN);
	Bind(wxEVT_MENU, &VCamTaskBarIcon::OnMenuHide, this, ID_TRAY_HIDE);
	Bind(wxEVT_MENU, &VCamTaskBarIcon::OnMenuAbout, this, ID_TRAY_ABOUT);
	Bind(wxEVT_MENU, &VCamTaskBarIcon::OnMenuExit, this, ID_TRAY_EXIT);
}

wxMenu* VCamTaskBarIcon::CreatePopupMenu()
{
	wxMenu* menu = new wxMenu();
	menu->Append(ID_TRAY_OPEN, "&Open PrismForge");
	menu->Append(ID_TRAY_HIDE, "&Hide to tray");
	menu->AppendSeparator();
	menu->Append(ID_TRAY_ABOUT, "&About");
	menu->AppendSeparator();
	menu->Append(ID_TRAY_EXIT, "E&xit");
	return menu;
}

void VCamTaskBarIcon::OnLeftClick(wxTaskBarIconEvent& event)
{
	if (parentWindow)
	{
		if (parentWindow->IsShown() && !parentWindow->IsIconized())
		{
			parentWindow->Raise();
		}
		else
		{
			parentWindow->RestoreFromTray();
		}
	}
}

void VCamTaskBarIcon::OnMenuOpen(wxCommandEvent& event)
{
	if (parentWindow)
		parentWindow->RestoreFromTray();
}

void VCamTaskBarIcon::OnMenuHide(wxCommandEvent& event)
{
	if (parentWindow)
		parentWindow->HideToTray();
}

void VCamTaskBarIcon::OnMenuAbout(wxCommandEvent& event)
{
	Studio::AboutDialog(parentWindow).ShowModal();
}

void VCamTaskBarIcon::OnMenuExit(wxCommandEvent& event)
{
	if (parentWindow)
		parentWindow->Close(true);
}





void Window::MinimizeToTaskbar(wxIconizeEvent& evt)
{
	if (Settings::Get("MINIMIZE_TASKBAR") == 1 && evt.IsIconized())
	{
		this->Hide();
		evt.Skip();
	}
}

void Window::MaximizeFromTaskbar(wxTaskBarIconEvent& evt)
{
	RestoreFromTray();
	this->SetFocus();
	this->Raise();
	this->Show();
}

Canvas* Window::GetCanvas() { return canvas; }
Studio::GlassSelect* Window::GetSourceChoice() { return sourceChoice; }
wxButton* Window::GetStreamOptionsButton() { return streamOptionsButton; }

wxButton* Window::GetRotateLeftButton() { return rotateLeftButton; }
wxButton* Window::GetRotateRightButton() { return rotateRightButton; }
wxButton* Window::GetFlipButton() { return flipButton; }
wxButton* Window::GetFlipVerticalButton() { return flipVerticalButton; }

wxButton* Window::GetZoomInButton() { return zoomInButton; }
wxButton* Window::GetZoomOutButton() { return zoomOutButton; }
Studio::GlassLabel* Window::GetZoomLevelLabel() { return zoomLevelLabel; }

wxButton* Window::GetTorchButton() { return torchButton; }
wxButton* Window::GetSwapButton() { return swapButton; }

wxButton* Window::GetAdjustmentsButton() { return adjustmentsButton; }
wxButton* Window::GetSnapshotButton() { return snapshotButton; }

Studio::GlassLabel* Window::GetStatsText() { return statsText; }
wxTaskBarIcon* Window::GetTaskbarIcon() { return taskbarIcon; }
