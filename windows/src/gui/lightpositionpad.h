#pragma once

#include <wx/wx.h>
#include <wx/timer.h>
#include <array>
#include <chrono>

namespace Studio {

// Inspector-only position editor. Coordinates describe a lighting direction,
// not detected facial geometry: x is left/right and y is top/bottom.
class LightPositionPad final : public wxControl {
public:
    explicit LightPositionPad(wxWindow* parent, wxWindowID id = wxID_ANY);
    ~LightPositionPad() override;

    // Configuration updates are silent; actual user edits emit wxEVT_SLIDER.
    // Drag commands coalesce at 16 ms, with the latest edit flushed on release.
    void SetPositions(float x1, float y1, float x2, float y2);
    std::array<float, 4> GetPositions() const { return m_positions; }
    int GetSelectedMarker() const { return m_selected; }
    void SetSelectedMarker(int marker);
    bool Enable(bool enabled = true) override;
    bool VerifyInteractionForTesting(wxString* failure = nullptr);

protected:
    wxSize DoGetBestSize() const override;

private:
    std::array<float, 4> m_positions{{ -0.45f, -0.35f, 0.45f, -0.15f }};
    int m_selected = 0;
    bool m_dragging = false;
    bool m_dragMoved = false;
    wxPoint m_dragStartPoint;
    wxPoint m_dragOffset;
    wxRect m_dragPlot;
    wxTimer m_changeTimer;
    bool m_changePending = false;
    std::chrono::steady_clock::time_point m_changeDue;

    static float CleanCoordinate(float value);
    wxRect PlotBounds() const;
    wxPoint MarkerPoint(int marker) const;
    int ClosestMarker(const wxPoint& point) const;
    bool EditSelected(float x, float y, bool deferNotification = false);
    void EditFromPoint(const wxPoint& point);
    void EmitChange();
    void QueueChange();
    void CancelPendingChange();
    void FlushPendingChange();
    void EndDrag(bool flush = false);
    void OnChangeTimer(wxTimerEvent& event);
    void OnPaint(wxPaintEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);
};

} // namespace Studio

