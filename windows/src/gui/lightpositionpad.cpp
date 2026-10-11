#include "lightpositionpad.h"
#include "studio.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <wx/evtloop.h>

namespace Studio {
namespace {
constexpr std::array<float, 4> DefaultPositions{{ -0.45f, -0.35f, 0.45f, -0.15f }};
constexpr int DragNotificationIntervalMs = 16;
}

LightPositionPad::LightPositionPad(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize,
        wxBORDER_NONE | wxWANTS_CHARS)
{
    SetName("Studio light positions");
    SetLabel("Studio light positions");
    SetFont(wxFontInfo(9).FaceName("Segoe UI"));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(160, 184)));
    SetToolTip("Drag light 1 (warm key) or light 2 (cool fill). Arrow keys move the selected light; Shift moves faster. Press 1 or 2 to select, or Home to reset both positions.");
    Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
    Bind(wxEVT_PAINT, &LightPositionPad::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &LightPositionPad::OnLeftDown, this);
    Bind(wxEVT_MOTION, &LightPositionPad::OnMotion, this);
    Bind(wxEVT_LEFT_UP, &LightPositionPad::OnLeftUp, this);
    Bind(wxEVT_KEY_DOWN, &LightPositionPad::OnKeyDown, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &LightPositionPad::OnCaptureLost, this);
    m_changeTimer.Bind(wxEVT_TIMER, &LightPositionPad::OnChangeTimer, this);
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { EndDrag(true); Refresh(false); event.Skip(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        // Inspector publication can repeat a size event without changing the plot.
        if (m_dragging && PlotBounds() != m_dragPlot) EndDrag(true);
        Refresh(false); event.Skip();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) { if (!event.IsShown()) EndDrag(); event.Skip(); });
}

LightPositionPad::~LightPositionPad() {
    EndDrag();
}

float LightPositionPad::CleanCoordinate(float value) {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

void LightPositionPad::SetPositions(float x1, float y1, float x2, float y2) {
    const std::array<float, 4> replacement{{
        CleanCoordinate(x1), CleanCoordinate(y1),
        CleanCoordinate(x2), CleanCoordinate(y2) }};
    if (replacement == m_positions) return;
    EndDrag();
    m_positions = replacement;
    Refresh(false);
}

void LightPositionPad::SetSelectedMarker(int marker) {
    const int selected = std::clamp(marker, 0, 1);
    if (m_selected == selected) return;
    m_selected = selected;
    Refresh(false);
}

bool LightPositionPad::Enable(bool enabled) {
    if (!enabled) EndDrag();
    const bool changed = wxControl::Enable(enabled);
    if (changed) Refresh(false);
    return changed;
}

wxSize LightPositionPad::DoGetBestSize() const {
    return FromDIP(wxSize(240, 184));
}

wxRect LightPositionPad::PlotBounds() const {
    const wxSize size = GetClientSize();
    const int pad = FromDIP(23);
    const int bottom = FromDIP(35);
    return wxRect(pad, pad, std::max(0, size.x - pad * 2),
        std::max(0, size.y - pad - bottom));
}

wxPoint LightPositionPad::MarkerPoint(int marker) const {
    const wxRect plot = PlotBounds();
    marker = std::clamp(marker, 0, 1);
    return wxPoint(plot.x + static_cast<int>(std::lround(
        (m_positions[marker * 2] + 1.0f) * 0.5f * plot.width)),
        plot.y + static_cast<int>(std::lround(
        (m_positions[marker * 2 + 1] + 1.0f) * 0.5f * plot.height)));
}

int LightPositionPad::ClosestMarker(const wxPoint& point) const {
    const wxPoint p1 = MarkerPoint(0), p2 = MarkerPoint(1);
    const double d1 = std::hypot(double(point.x) - p1.x, double(point.y) - p1.y);
    const double d2 = std::hypot(double(point.x) - p2.x, double(point.y) - p2.y);
    return d1 == d2 ? m_selected : (d1 < d2 ? 0 : 1);
}

bool LightPositionPad::EditSelected(float x, float y, bool deferNotification) {
    x = CleanCoordinate(x); y = CleanCoordinate(y);
    if (x == m_positions[m_selected * 2] && y == m_positions[m_selected * 2 + 1])
        return false;
    m_positions[m_selected * 2] = x;
    m_positions[m_selected * 2 + 1] = y;
    Refresh(false);
    if (deferNotification) {
        // The invalidated local marker paints through the native event loop;
        // application-wide effects work is bounded by the notification timer.
        QueueChange();
    } else {
        CancelPendingChange();
        EmitChange();
    }
    return true;
}

void LightPositionPad::EditFromPoint(const wxPoint& point) {
    const wxRect plot = PlotBounds();
    if (plot.width <= 0 || plot.height <= 0) return;
    const double x = (double(point.x) + m_dragOffset.x - plot.x) / plot.width;
    const double y = (double(point.y) + m_dragOffset.y - plot.y) / plot.height;
    EditSelected(static_cast<float>(x * 2.0 - 1.0),
        static_cast<float>(y * 2.0 - 1.0), true);
}

void LightPositionPad::EmitChange() {
    wxCommandEvent event(wxEVT_SLIDER, GetId());
    event.SetEventObject(this);
    event.SetInt(m_selected);
    ProcessWindowEvent(event);
}

void LightPositionPad::QueueChange() {
    if (m_changePending) return;
    m_changePending = true;
    m_changeDue = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(DragNotificationIntervalMs);
    if (!m_changeTimer.StartOnce(DragNotificationIntervalMs)) FlushPendingChange();
}

void LightPositionPad::CancelPendingChange() {
    m_changeTimer.Stop();
    m_changePending = false;
}

void LightPositionPad::FlushPendingChange() {
    const bool pending = m_changePending;
    CancelPendingChange();
    if (pending) EmitChange();
}

void LightPositionPad::OnChangeTimer(wxTimerEvent&) {
    if (!m_changePending) return;
    if (!m_dragging || !IsEnabled() || !IsShownOnScreen()) { EndDrag(); return; }
    const auto remaining = m_changeDue - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero()) {
        // A native timer message queued before cancellation must not publish a
        // new drag prematurely if the platform reuses the stopped timer's ID.
        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1;
        if (!m_changeTimer.StartOnce(static_cast<int>(delay))) FlushPendingChange();
        return;
    }
    FlushPendingChange();
}

void LightPositionPad::EndDrag(bool flush) {
    const bool pending = flush && m_changePending;
    CancelPendingChange();
    m_dragging = false;
    m_dragMoved = false;
    m_dragOffset = wxPoint();
    if (HasCapture()) ReleaseMouse();
    // Clear capture/timer state before dispatch, as listeners can replace values.
    if (pending) EmitChange();
}

void LightPositionPad::OnLeftDown(wxMouseEvent& event) {
    if (!IsEnabled()) return;
    const wxRect plot = PlotBounds();
    if (plot.width <= 0 || plot.height <= 0) return;
    EndDrag(true);
    SetFocus();
    SetSelectedMarker(ClosestMarker(event.GetPosition()));
    const wxPoint marker = MarkerPoint(m_selected);
    const double distance = std::hypot(double(event.GetX()) - marker.x,
        double(event.GetY()) - marker.y);
    m_dragging = true;
    m_dragMoved = false;
    m_dragStartPoint = event.GetPosition();
    m_dragPlot = plot;
    m_dragOffset = distance <= FromDIP(13) ? marker - event.GetPosition() : wxPoint();
    if (!HasCapture() && IsShownOnScreen()) CaptureMouse();
    // Selecting a marker alone does not shift it or issue an effects update.
    if (distance > FromDIP(13)) EditFromPoint(event.GetPosition());
}

void LightPositionPad::OnMotion(wxMouseEvent& event) {
    if (!m_dragging) { event.Skip(); return; }
    if (!IsEnabled()) { EndDrag(); return; }
    if (!event.LeftIsDown()) { EndDrag(true); return; }
    if (!m_dragMoved && event.GetPosition() == m_dragStartPoint) return;
    m_dragMoved = true;
    EditFromPoint(event.GetPosition());
}

void LightPositionPad::OnLeftUp(wxMouseEvent& event) {
    if (!m_dragging) { event.Skip(); return; }
    if (IsEnabled() && (m_dragMoved || event.GetPosition() != m_dragStartPoint))
        EditFromPoint(event.GetPosition());
    EndDrag(true);
}

void LightPositionPad::OnCaptureLost(wxMouseCaptureLostEvent&) {
    EndDrag(true);
}

void LightPositionPad::OnKeyDown(wxKeyEvent& event) {
    if (!IsEnabled()) { event.Skip(); return; }
    const int code = event.GetKeyCode();
    if (code == WXK_TAB) {
        Navigate((event.ShiftDown() ? wxNavigationKeyEvent::IsBackward :
            wxNavigationKeyEvent::IsForward) | wxNavigationKeyEvent::FromTab);
        return;
    }
    if (code == '1' || code == '2') { EndDrag(true); SetSelectedMarker(code - '1'); return; }
    if (code == WXK_SPACE) { EndDrag(true); SetSelectedMarker(1 - m_selected); return; }
    if (code == WXK_ESCAPE) { EndDrag(true); return; }
    if (code == WXK_HOME) {
        if (m_positions == DefaultPositions) { EndDrag(true); return; }
        SetPositions(DefaultPositions[0], DefaultPositions[1], DefaultPositions[2], DefaultPositions[3]);
        EmitChange();
        return;
    }
    const float step = event.ShiftDown() ? 0.10f : 0.025f;
    float x = m_positions[m_selected * 2], y = m_positions[m_selected * 2 + 1];
    if (code == WXK_LEFT) x -= step;
    else if (code == WXK_RIGHT) x += step;
    else if (code == WXK_UP) y -= step;
    else if (code == WXK_DOWN) y += step;
    else { event.Skip(); return; }
    const bool pending = m_changePending;
    EndDrag();
    if (!EditSelected(x, y) && pending) EmitChange();
}

void LightPositionPad::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    PaintBackdrop(gc.get(), this, wxRect(0, 0, size.x, size.y));
    const auto& theme = CurrentTheme();
    const wxRect plot = PlotBounds();
    if (plot.width <= 0 || plot.height <= 0) return;
    const bool enabled = IsEnabled();
    gc->SetBrush(wxBrush(theme.inputFill));
    gc->SetPen(wxPen(theme.inputBorder, 1));
    gc->DrawRoundedRectangle(plot.x, plot.y, plot.width, plot.height, FromDIP(12));
    const double cx = plot.x + plot.width * 0.5, cy = plot.y + plot.height * 0.5;
    gc->SetPen(wxPen(theme.sliderTrack, 1));
    gc->StrokeLine(cx, plot.y + FromDIP(7), cx, plot.y + plot.height - FromDIP(7));
    gc->StrokeLine(plot.x + FromDIP(7), cy, plot.x + plot.width - FromDIP(7), cy);
    gc->SetFont(GetFont(), theme.muted);
    double tw = 0, th = 0;
    gc->GetTextExtent("Top", &tw, &th);
    gc->DrawText("Top", cx - tw * 0.5, 0);
    const wxString help = "Drag 1 or 2; arrow keys move";
    gc->GetTextExtent(help, &tw, &th);
    if (tw <= size.x) gc->DrawText(help, (size.x - tw) * 0.5, size.y - th - FromDIP(2));
    for (int marker = 0; marker != 2; ++marker) {
        const wxPoint point = MarkerPoint(marker);
        wxColour colour = marker == 0 ? wxColour(255, 202, 104) : wxColour(131, 202, 255);
        if (!enabled) colour = Mix(colour, theme.bg1, 0.6);
        const int radius = FromDIP(11);
        gc->SetBrush(wxBrush(colour));
        gc->SetPen(wxPen(enabled ? theme.text : theme.muted, 1));
        gc->DrawEllipse(point.x - radius, point.y - radius, radius * 2, radius * 2);
        gc->SetFont(GetFont(), wxColour(19, 25, 39));
        const wxString label = marker == 0 ? "1" : "2";
        gc->GetTextExtent(label, &tw, &th);
        gc->DrawText(label, point.x - tw * 0.5, point.y - th * 0.5);
        if (marker == m_selected && enabled) {
            const int ring = FromDIP(15);
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(theme.accent, FromDIP(2)));
            gc->DrawEllipse(point.x - ring, point.y - ring, ring * 2, ring * 2);
        }
    }
    if (enabled && FindFocus() == this) {
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(theme.accent, FromDIP(2)));
        gc->DrawRoundedRectangle(plot.x - FromDIP(3), plot.y - FromDIP(3),
            plot.width + FromDIP(6), plot.height + FromDIP(6), FromDIP(14));
    }
}

bool LightPositionPad::VerifyInteractionForTesting(wxString* failure) {
    const auto original = m_positions;
    const int originalSelection = m_selected;
    const bool originalEnabled = IsThisEnabled();
    const bool originalShown = IsShown();
    const wxSize originalSize = GetClientSize();
    EndDrag();
    if (failure) failure->clear();
    int changes = 0;
    std::array<float, 4> delivered = m_positions;
    wxEvtHandler observer;
    observer.Bind(wxEVT_SLIDER, [this, &changes, &delivered](wxCommandEvent& event) {
        if (event.GetEventObject() == this && event.GetId() == GetId()) {
            ++changes;
            delivered = GetPositions();
        }
        event.Skip();
    });
    PushEventHandler(&observer);
    bool ok = true;
    auto check = [&](bool passed, const wxString& message) {
        if (!passed && ok) { ok = false; if (failure) *failure = message; }
    };
    auto key = [&](int code) {
        wxKeyEvent event(wxEVT_KEY_DOWN); event.m_keyCode = code;
        event.SetEventObject(this); ProcessWindowEvent(event);
    };
    auto mouse = [&](wxEventType type, wxPoint position, bool leftDown) {
        wxMouseEvent event(type); event.SetPosition(position); event.SetLeftDown(leftDown);
        event.SetEventObject(this); ProcessWindowEvent(event);
    };
    // Exercise the actual native paint/one-shot timer loop without delivering
    // mouse/scroll input or inventing timer events. On MSW, dirty WM_PAINT work
    // must run before the lower-priority synthesized WM_TIMER can be delivered.
    // A deadline bounds a missing-timer failure.
    auto pumpTimers = [&](int durationMs, bool stopWhenDrained = false) {
        wxEventLoop loop;
        wxEventLoopActivator activate(&loop);
        wxStopWatch elapsed;
        do {
            wxMilliSleep(2);
            loop.YieldFor(wxEVT_CATEGORY_TIMER | wxEVT_CATEGORY_UI);
        } while (elapsed.Time() < durationMs && (!stopWhenDrained || m_changePending));
    };
    auto reset = [&] {
        SetPositions(DefaultPositions[0], DefaultPositions[1], DefaultPositions[2], DefaultPositions[3]);
        SetSelectedMarker(0);
    };
    Enable(true);
    SetPositions(8.0f, -8.0f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity());
    check(m_positions == std::array<float,4>{{1.0f,-1.0f,0.0f,0.0f}}, "Position clamp/non-finite rejection");
    check(changes == 0, "Programmatic update emitted a command");
    SetPositions(-0.4f, -0.2f, 0.5f, 0.2f);
    SetSelectedMarker(0);
    key(WXK_RIGHT);
    check(std::abs(m_positions[0] - (-0.375f)) < 0.0001f && changes == 1, "Arrow key edit/command dispatch");
    key('2');
    check(m_selected == 1 && changes == 1, "Keyboard marker selection must be silent");
    key(WXK_UP);
    check(std::abs(m_positions[3] - 0.175f) < 0.0001f && changes == 2, "Second marker keyboard edit");
    key(WXK_HOME);
    check(m_positions == DefaultPositions && changes == 3, "Home resets both light positions");
    const wxRect plot = PlotBounds();
    check(plot.width > 0 && plot.height > 0, "Pad has no native layout extent");
    const int beforeClick = changes;
    mouse(wxEVT_LEFT_DOWN, MarkerPoint(0), true);
    mouse(wxEVT_LEFT_UP, MarkerPoint(0), false);
    check(changes == beforeClick && !m_dragging && !HasCapture(), "Marker selection/click must be silent and release capture");

    // An edge grab follows the marker's original centre, preserving the offset.
    reset();
    const wxPoint edgeCentre = MarkerPoint(0);
    const wxPoint edgeGrab = edgeCentre + FromDIP(wxPoint(8, 4));
    const wxPoint edgeDelta = FromDIP(wxPoint(6, 4));
    const auto beforeEdge = m_positions;
    const int beforeEdgeChanges = changes;
    mouse(wxEVT_LEFT_DOWN, edgeGrab, true);
    check(m_positions == beforeEdge && changes == beforeEdgeChanges, "Edge grab must not jump or publish");
    mouse(wxEVT_MOTION, edgeGrab + edgeDelta, true);
    check(MarkerPoint(0) == edgeCentre + edgeDelta && changes == beforeEdgeChanges,
        "Dragging must preserve the grab offset and update locally before publishing");
    mouse(wxEVT_LEFT_UP, edgeGrab + edgeDelta, false);
    check(changes == beforeEdgeChanges + 1 && delivered == m_positions && !m_changePending &&
        !m_changeTimer.IsRunning() && !m_dragging && !HasCapture(), "Mouse-up must flush the latest position and release capture");
    pumpTimers(34);
    check(changes == beforeEdgeChanges + 1, "Mouse-up must not leave a duplicate timer command");

    reset();
    const wxPoint burstStart = MarkerPoint(0);
    const int beforeBurst = changes;
    check(IsShownOnScreen() && IsEnabled(), wxString::Format(
        "Native timer burst requires a visible enabled pad (shown=%d enabled=%d)",
        int(IsShownOnScreen()), int(IsEnabled())));
    mouse(wxEVT_LEFT_DOWN, burstStart, true);
    check(HasCapture(), "Native timer burst must start with actual mouse capture");
    for (int sample = 1; sample <= 8; ++sample)
        mouse(wxEVT_MOTION, burstStart + FromDIP(wxPoint(sample * 3, sample * 2)), true);
    const auto burstLatest = m_positions;
    check(changes == beforeBurst && m_changePending && m_changeTimer.IsRunning() &&
        MarkerPoint(0) == burstStart + FromDIP(wxPoint(24, 16)), "Motion burst must coalesce while the marker tracks the latest position");
    SetPositions(burstLatest[0], burstLatest[1], burstLatest[2], burstLatest[3]);
    check(m_dragging && m_changePending && m_changeTimer.IsRunning() && changes == beforeBurst,
        "Unchanged programmatic publication must preserve the pending drag");
    wxSizeEvent sameSize(GetClientSize(), GetId()); sameSize.SetEventObject(this); ProcessWindowEvent(sameSize);
    check(m_dragging && m_changePending, "Repeated same-size layout event must preserve dragging");
    pumpTimers(500, true);
    check(changes == beforeBurst + 1 && delivered == burstLatest && m_dragging &&
        !m_changePending && !m_changeTimer.IsRunning(), wxString::Format(
        "Native timer must publish exactly the latest burst position once (changes=%d expected=%d delivered=[%.6f,%.6f,%.6f,%.6f] latest=[%.6f,%.6f,%.6f,%.6f] pending=%d dragging=%d timer=%d capture=%d shown=%d enabled=%d)",
        changes, beforeBurst + 1, delivered[0], delivered[1], delivered[2], delivered[3],
        burstLatest[0], burstLatest[1], burstLatest[2], burstLatest[3], int(m_changePending),
        int(m_dragging), int(m_changeTimer.IsRunning()), int(HasCapture()), int(IsShownOnScreen()), int(IsEnabled())));
    const int beforeFinalBurst = changes;
    for (int sample = 9; sample <= 12; ++sample)
        mouse(wxEVT_MOTION, burstStart + FromDIP(wxPoint(sample * 3, sample * 2)), true);
    mouse(wxEVT_LEFT_UP, burstStart + FromDIP(wxPoint(42, 28)), false);
    check(changes == beforeFinalBurst + 1 && delivered == m_positions &&
        MarkerPoint(0) == burstStart + FromDIP(wxPoint(42, 28)), "Final release must include a newer release position before the next timer tick");
    pumpTimers(34);
    check(changes == beforeFinalBurst + 1 && !m_dragging && !HasCapture(), "Released drag must not publish again");

    reset();
    const int beforeLost = changes;
    mouse(wxEVT_LEFT_DOWN, MarkerPoint(0), true);
    mouse(wxEVT_MOTION, wxPoint(plot.x + plot.width + 20, plot.y - 20), true);
    check(m_positions[0] == 1.0f && m_positions[1] == -1.0f && changes == beforeLost,
        "Mouse coordinate mapping/bounds clamp must remain local until notification");
    wxMouseCaptureLostEvent lost(GetId()); lost.SetEventObject(this); ProcessWindowEvent(lost);
    check(changes == beforeLost + 1 && delivered == m_positions && !m_dragging && !HasCapture() &&
        !m_changePending && !m_changeTimer.IsRunning(), "Capture loss must flush latest values and clear drag/timer state");
    mouse(wxEVT_MOTION, wxPoint(plot.x, plot.y + plot.height), true);
    pumpTimers(34);
    check(changes == beforeLost + 1, "Motion/timer after capture loss must not edit or publish");

    reset();
    mouse(wxEVT_LEFT_DOWN, MarkerPoint(1), true);
    const wxPoint releasedPoint = MarkerPoint(1) + FromDIP(wxPoint(-9, 5));
    const int beforeMissingRelease = changes;
    mouse(wxEVT_MOTION, releasedPoint, true);
    mouse(wxEVT_MOTION, releasedPoint, false);
    check(changes == beforeMissingRelease + 1 && delivered == m_positions && !HasCapture() && !m_dragging,
        "Missing left-up must safely flush when motion reports the button released");

    reset();
    const wxPoint keyStart = MarkerPoint(0);
    mouse(wxEVT_LEFT_DOWN, keyStart, true);
    mouse(wxEVT_MOTION, keyStart + FromDIP(wxPoint(5, 3)), true);
    const float queuedX = m_positions[0];
    const int beforeImmediateKey = changes;
    key(WXK_RIGHT);
    check(changes == beforeImmediateKey + 1 && std::abs(m_positions[0] - queuedX - 0.025f) < 0.0001f &&
        delivered == m_positions && !m_changePending && !m_dragging, "Keyboard edit during a queued drag must publish immediately once");
    pumpTimers(34);
    check(changes == beforeImmediateKey + 1, "Immediate keyboard edit must consume the pending drag notification");

    reset();
    const wxPoint replacementStart = MarkerPoint(0);
    mouse(wxEVT_LEFT_DOWN, replacementStart, true);
    mouse(wxEVT_MOTION, replacementStart + FromDIP(wxPoint(8, 5)), true);
    const int beforeReplacement = changes;
    SetPositions(0.2f, 0.4f, -0.6f, -0.3f);
    const auto replacement = m_positions;
    mouse(wxEVT_LEFT_UP, replacementStart + FromDIP(wxPoint(15, 9)), false);
    pumpTimers(34);
    check(changes == beforeReplacement && m_positions == replacement && !m_dragging && !HasCapture() &&
        !m_changePending && !m_changeTimer.IsRunning(), "Programmatic replacement must cancel stale capture and queued publication");

    reset();
    const wxPoint disabledStart = MarkerPoint(1);
    mouse(wxEVT_LEFT_DOWN, disabledStart, true);
    mouse(wxEVT_MOTION, disabledStart + FromDIP(wxPoint(-9, 4)), true);
    const int beforeDisable = changes;
    Enable(false);
    const auto disabled = m_positions;
    key(WXK_LEFT);
    mouse(wxEVT_MOTION, wxPoint(plot.x, plot.y), true);
    pumpTimers(34);
    check(changes == beforeDisable && !m_dragging && !HasCapture() && m_positions == disabled &&
        !m_changePending && !m_changeTimer.IsRunning(), "Disabled pad must cancel queued publication, release capture and ignore input");
    Enable(true);

    reset();
    const wxPoint hiddenStart = MarkerPoint(0);
    mouse(wxEVT_LEFT_DOWN, hiddenStart, true);
    mouse(wxEVT_MOTION, hiddenStart + FromDIP(wxPoint(7, 4)), true);
    const int beforeHide = changes;
    Hide();
    Show(originalShown);
    pumpTimers(34);
    check(changes == beforeHide && !m_dragging && !HasCapture() && !m_changePending && !m_changeTimer.IsRunning(),
        "Hiding must cancel a pending drag without leaking a command after showing");

    SetPositions(1.0f, 1.0f, -1.0f, -1.0f);
    SetSelectedMarker(0);
    const int beforeClampKey = changes;
    key(WXK_RIGHT);
    check(changes == beforeClampKey, "Key at limit must not issue an unchanged command");
    mouse(wxEVT_LEFT_DOWN, MarkerPoint(0), true);
    check(m_dragging, "Resize test must begin with an active drag");
    SetClientSize(originalSize + FromDIP(wxSize(7, 5)));
    check(!m_dragging && !HasCapture(), "Actual plot geometry change must cancel captured drag");
    SetClientSize(originalSize);

    reset();
    const int beforeFarClick = changes;
    const wxPoint farClick(plot.x, plot.y);
    mouse(wxEVT_LEFT_DOWN, farClick, true);
    check(m_positions[0] == -1.0f && m_positions[1] == -1.0f && changes == beforeFarClick,
        "Click away from a marker must still reposition it locally");
    mouse(wxEVT_LEFT_UP, farClick, false);
    check(changes == beforeFarClick + 1 && delivered == m_positions, "Click-to-reposition must flush once on release");

    int destroyedChanges = 0;
    auto* disposable = new LightPositionPad(GetParent());
    disposable->SetClientSize(originalSize);
    disposable->Hide();
    disposable->Bind(wxEVT_SLIDER, [&destroyedChanges](wxCommandEvent&) { ++destroyedChanges; });
    wxMouseEvent disposeDown(wxEVT_LEFT_DOWN); disposeDown.SetPosition(disposable->MarkerPoint(0));
    disposeDown.SetLeftDown(true); disposeDown.SetEventObject(disposable); disposable->ProcessWindowEvent(disposeDown);
    wxMouseEvent disposeMove(wxEVT_MOTION); disposeMove.SetPosition(disposeDown.GetPosition() + FromDIP(wxPoint(9, 5)));
    disposeMove.SetLeftDown(true); disposeMove.SetEventObject(disposable); disposable->ProcessWindowEvent(disposeMove);
    check(disposable->m_changePending && disposable->m_changeTimer.IsRunning(), "Destruction probe must have a scheduled native timer");
    delete disposable;
    pumpTimers(34);
    check(destroyedChanges == 0, "Destroyed pad must not deliver its pending drag command");

    RemoveEventHandler(&observer);
    EndDrag();
    SetClientSize(originalSize);
    Show(originalShown);
    SetPositions(original[0], original[1], original[2], original[3]);
    SetSelectedMarker(originalSelection);
    EmitChange(); // Restore the root's current configuration after dispatched edits.
    Enable(originalEnabled);
    return ok;
}

} // namespace Studio


