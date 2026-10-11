#pragma once
#include <wx/wx.h>
#include <wx/settings.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <wx/tokenzr.h>
#include <wx/clipbrd.h>
#include <wx/msw/dc.h>
#include <memory>
#include <algorithm>
#include <functional>
#include <vector>
#include <cmath>

namespace Studio {

struct Theme {
    int id = 3; // 0=System, 1=Dark, 2=Light, 3=Glass
    wxString name = "Glass";
    bool isDark = true;
    bool isGlass = true;

    // Ambient background
    wxColour bg1 = wxColour("#0b1030");
    wxColour bg2 = wxColour("#1a1147");
    wxColour orb1 = wxColour(109, 94, 252); // #6d5efc
    wxColour orb2 = wxColour(25, 182, 201);  // #19b6c9
    wxColour orb3 = wxColour(224, 80, 143); // #e0508f

    // Typography & States
    wxColour text = wxColour("#f1f3ff");
    wxColour muted = wxColour(241, 243, 255, 217); // rgba(241,243,255,.85) — min 4.5:1 contrast
    wxColour accent = wxColour("#8b93ff");
    wxColour accentInk = wxColour("#0d1030");
    wxColour live = wxColour("#5df2a0");

    // Glass parameters
    wxColour glassRgb = wxColour(255, 255, 255);
    double fillAlphaTop = 0.12;
    double fillAlphaBottom = 0.05;
    double borderAlpha = 0.16;
    double highlightAlpha = 0.22;
    wxColour shadowCol = wxColour(0, 0, 0, 89); // rgba(0,0,0,.35)

    // Elements
    wxColour inputFill = wxColour(0, 0, 0, 51); // rgba(0,0,0,.20)
    wxColour inputBorder = wxColour(255, 255, 255, 31); // rgba(255,255,255,.12)
    wxColour segContainer = wxColour(0, 0, 0, 51);
    wxColour segPill = wxColour(255, 255, 255, 46); // rgba(255,255,255,.18)
    wxColour segPillBorder = wxColour(255, 255, 255, 56); // rgba(255,255,255,.22)
    wxColour sliderTrack = wxColour(255, 255, 255, 46); // rgba(255,255,255,.18)
};

inline int appearance = 3; // 0=System, 1=Dark, 2=Light, 3=Glass (Default Glass)
inline float glassFill = 0.08f; // 8% opacity default
inline int glassBlur = 24;      // 24px blur default

inline const Theme& GetTheme(int appearanceMode) {
    static Theme s_glassTheme;
    static bool s_glassInited = false;
    if (!s_glassInited) {
        s_glassTheme.id = 3;
        s_glassTheme.name = "Glass";
        s_glassTheme.isDark = true;
        s_glassTheme.isGlass = true;
        s_glassTheme.bg1 = wxColour("#0b1030");
        s_glassTheme.bg2 = wxColour("#1a1147");
        s_glassTheme.orb1 = wxColour(109, 94, 252);
        s_glassTheme.orb2 = wxColour(25, 182, 201);
        s_glassTheme.orb3 = wxColour(224, 80, 143);
        s_glassTheme.text = wxColour("#f1f3ff");
        s_glassTheme.muted = wxColour(241, 243, 255, 217); // rgba(241,243,255,.85)
        s_glassTheme.accent = wxColour("#8b93ff");
        s_glassTheme.accentInk = wxColour("#0d1030");
        s_glassTheme.live = wxColour("#5df2a0");
        s_glassTheme.glassRgb = wxColour(255, 255, 255);
        s_glassTheme.fillAlphaTop = 0.12;
        s_glassTheme.fillAlphaBottom = 0.05;
        s_glassTheme.borderAlpha = 0.16;
        s_glassTheme.highlightAlpha = 0.22;
        s_glassTheme.shadowCol = wxColour(0, 0, 0, 89);
        s_glassTheme.inputFill = wxColour(0, 0, 0, 51);
        s_glassTheme.inputBorder = wxColour(255, 255, 255, 31);
        s_glassTheme.segContainer = wxColour(0, 0, 0, 51);
        s_glassTheme.segPill = wxColour(255, 255, 255, 46);
        s_glassTheme.segPillBorder = wxColour(255, 255, 255, 56);
        s_glassTheme.sliderTrack = wxColour(255, 255, 255, 46);
        s_glassInited = true;
    }

    static Theme s_lightTheme;
    static bool s_lightInited = false;
    if (!s_lightInited) {
        s_lightTheme.id = 2;
        s_lightTheme.name = "Light";
        s_lightTheme.isDark = false;
        s_lightTheme.isGlass = true;
        s_lightTheme.bg1 = wxColour("#e9ecff");
        s_lightTheme.bg2 = wxColour("#f6e9f5");
        s_lightTheme.orb1 = wxColour(180, 175, 255);
        s_lightTheme.orb2 = wxColour(160, 230, 245);
        s_lightTheme.orb3 = wxColour(255, 195, 220);
        s_lightTheme.text = wxColour("#171a33");
        s_lightTheme.muted = wxColour(23, 26, 51, 153);
        s_lightTheme.accent = wxColour("#4f56e6");
        s_lightTheme.accentInk = wxColour("#ffffff");
        s_lightTheme.live = wxColour("#0f9d58");
        s_lightTheme.glassRgb = wxColour(255, 255, 255);
        s_lightTheme.fillAlphaTop = 0.55;
        s_lightTheme.fillAlphaBottom = 0.40;
        s_lightTheme.borderAlpha = 0.80;
        s_lightTheme.highlightAlpha = 0.90;
        s_lightTheme.shadowCol = wxColour(20, 20, 60, 31);
        s_lightTheme.inputFill = wxColour(255, 255, 255, 140);
        s_lightTheme.inputBorder = wxColour(79, 86, 230, 64);
        s_lightTheme.segContainer = wxColour(220, 223, 245, 128);
        s_lightTheme.segPill = wxColour(255, 255, 255, 217);
        s_lightTheme.segPillBorder = wxColour(255, 255, 255, 242);
        s_lightTheme.sliderTrack = wxColour(79, 86, 230, 51);
        s_lightInited = true;
    }

    static Theme s_darkTheme;
    static bool s_darkInited = false;
    if (!s_darkInited) {
        s_darkTheme.id = 1;
        s_darkTheme.name = "Dark";
        s_darkTheme.isDark = true;
        s_darkTheme.isGlass = false;
        s_darkTheme.bg1 = wxColour("#12141c");
        s_darkTheme.bg2 = wxColour("#12141c");
        s_darkTheme.text = wxColour("#f1f3ff");
        s_darkTheme.muted = wxColour(230, 233, 255, 158);
        s_darkTheme.accent = wxColour("#8b93ff");
        s_darkTheme.accentInk = wxColour("#0d1030");
        s_darkTheme.live = wxColour("#5df2a0");
        s_darkTheme.glassRgb = wxColour(255, 255, 255);
        s_darkTheme.fillAlphaTop = 0.05;
        s_darkTheme.fillAlphaBottom = 0.05;
        s_darkTheme.borderAlpha = 0.10;
        s_darkTheme.highlightAlpha = 0.12;
        s_darkTheme.shadowCol = wxColour(0, 0, 0, 80);
        s_darkTheme.inputFill = wxColour(0, 0, 0, 89);
        s_darkTheme.inputBorder = wxColour(255, 255, 255, 26);
        s_darkTheme.segContainer = wxColour(0, 0, 0, 89);
        s_darkTheme.segPill = wxColour(255, 255, 255, 31);
        s_darkTheme.segPillBorder = wxColour(255, 255, 255, 40);
        s_darkTheme.sliderTrack = wxColour(255, 255, 255, 36);
        s_darkInited = true;
    }

    if (appearanceMode == 0) {
        bool sysDark = wxSystemSettings::GetAppearance().IsDark();
        return sysDark ? s_darkTheme : s_lightTheme;
    }
    if (appearanceMode == 1) return s_darkTheme;
    if (appearanceMode == 2) return s_lightTheme;
    return s_glassTheme; // 3 = Glass
}

inline const Theme& CurrentTheme() {
    return GetTheme(appearance);
}

inline bool Glass() { return CurrentTheme().isGlass; }
inline bool Dark() { return CurrentTheme().isDark; }

inline wxColour GlassBg1()  { return CurrentTheme().bg1; }
inline wxColour GlassBg2()  { return CurrentTheme().bg2; }
inline wxColour Orb1()      { return CurrentTheme().orb1; }
inline wxColour Orb2()      { return CurrentTheme().orb2; }
inline wxColour Orb3()      { return CurrentTheme().orb3; }

inline wxColour Background() { return CurrentTheme().bg1; }
inline wxColour Rail()       { return CurrentTheme().bg1; }
inline wxColour Surface()    { return CurrentTheme().isGlass ? wxColour(17, 23, 44) : (Dark() ? wxColour(26, 29, 40) : wxColour(255, 255, 255)); }
inline wxColour Raised()     { return CurrentTheme().isGlass ? wxColour(24, 32, 58) : (Dark() ? wxColour(33, 37, 51) : wxColour(246, 247, 251)); }
inline wxColour Line()       { return wxColour(CurrentTheme().glassRgb.Red(), CurrentTheme().glassRgb.Green(), CurrentTheme().glassRgb.Blue(), int(CurrentTheme().borderAlpha * 255.0)); }
inline wxColour Text()       { return CurrentTheme().text; }
inline wxColour Muted()      { return CurrentTheme().muted; }
inline wxColour Accent()     { return CurrentTheme().accent; }
inline wxColour AccentInk()  { return CurrentTheme().accentInk; }
inline wxColour Live()       { return CurrentTheme().live; }
inline wxColour Stage()      { return CurrentTheme().bg1; }
inline wxColour DockBg()     { return CurrentTheme().inputFill; }
inline wxColour SegBg()      { return CurrentTheme().segContainer; }

inline wxColour Mix(wxColour a, wxColour b, double ratio) {
    return wxColour(
        int(a.Red() * (1.0 - ratio) + b.Red() * ratio),
        int(a.Green() * (1.0 - ratio) + b.Green() * ratio),
        int(a.Blue() * (1.0 - ratio) + b.Blue() * ratio)
    );
}

struct Role : wxClientData {
    bool muted = false, primary = false;
    Role(bool m, bool p) : muted(m), primary(p) {}
};

inline void Style(wxWindow* w, bool surface = false) {
    w->SetBackgroundColour(surface ? Surface() : Background());
    w->SetForegroundColour(Text());
    w->SetFont(wxFontInfo(10).FaceName("Segoe UI"));
}

inline wxBitmap s_ambientBg;
inline wxWindow* s_rootShell = nullptr;

inline void InvalidateBackground() {
    s_ambientBg = wxNullBitmap;
}

inline wxPoint GetPosInTop(wxWindow* win) {
    wxPoint pos(0, 0);
    wxWindow* w = win;
    while (w && w->GetParent()) {
        pos += w->GetPosition();
        w = w->GetParent();
    }
    return pos;
}

inline void ApplyImmersiveDarkMode(HWND hwnd, bool dark) {
    if (!hwnd) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm) {
        auto fn = (DwmSetWindowAttributeFn)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (fn) {
            BOOL val = dark ? TRUE : FALSE;
            fn(hwnd, 20, &val, sizeof(val));
            fn(hwnd, 19, &val, sizeof(val));
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        FreeLibrary(hDwm);
    }
}

inline void RenderAmbientToContext(wxGraphicsContext* g, int w, int h) {
    if (!g || w <= 0 || h <= 0) return;
    const auto& th = CurrentTheme();
    if (th.isGlass) {
        // Base linear gradient: 160deg
        g->SetBrush(g->CreateLinearGradientBrush(w * 0.35, 0, 0, h, th.bg1, th.bg2));
        g->SetPen(*wxTRANSPARENT_PEN);
        g->DrawRectangle(0, 0, w, h);

        double blurScale = std::clamp(glassBlur / 24.0, 0.4, 2.5);

        // Orb 1: top-left
        double r1 = std::max(180.0, w * 0.46 * blurScale);
        double cx1 = w * 0.12, cy1 = h * 0.08;
        g->SetBrush(g->CreateRadialGradientBrush(cx1, cy1, cx1, cy1, r1,
            wxColour(th.orb1.Red(), th.orb1.Green(), th.orb1.Blue(), th.isDark ? 190 : 150),
            wxColour(th.orb1.Red(), th.orb1.Green(), th.orb1.Blue(), 0)));
        g->DrawEllipse(cx1 - r1, cy1 - r1, r1 * 2, r1 * 2);

        // Orb 2: middle-right
        double r2 = std::max(160.0, w * 0.38 * blurScale);
        double cx2 = w * 0.88, cy2 = h * 0.32;
        g->SetBrush(g->CreateRadialGradientBrush(cx2, cy2, cx2, cy2, r2,
            wxColour(th.orb2.Red(), th.orb2.Green(), th.orb2.Blue(), th.isDark ? 140 : 120),
            wxColour(th.orb2.Red(), th.orb2.Green(), th.orb2.Blue(), 0)));
        g->DrawEllipse(cx2 - r2, cy2 - r2, r2 * 2, r2 * 2);

        // Orb 3: bottom-center
        double r3 = std::max(150.0, w * 0.35 * blurScale);
        double cx3 = w * 0.46, cy3 = h * 0.90;
        g->SetBrush(g->CreateRadialGradientBrush(cx3, cy3, cx3, cy3, r3,
            wxColour(th.orb3.Red(), th.orb3.Green(), th.orb3.Blue(), th.isDark ? 135 : 110),
            wxColour(th.orb3.Red(), th.orb3.Green(), th.orb3.Blue(), 0)));
        g->DrawEllipse(cx3 - r3, cy3 - r3, r3 * 2, r3 * 2);
    } else {
        g->SetBrush(wxBrush(th.bg1));
        g->SetPen(*wxTRANSPARENT_PEN);
        g->DrawRectangle(0, 0, w, h);
    }
}

inline void RenderAmbientBitmap(wxBitmap& bmp, int w, int h) {
    if (w <= 0 || h <= 0) return;
    bmp.Create(w, h, 32);
    wxMemoryDC memDC(bmp);
    std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(memDC));
    if (!g) return;
    g->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    RenderAmbientToContext(g.get(), w, h);
}

inline void RenderAmbientBackground(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (s_ambientBg.IsOk() && s_ambientBg.GetWidth() == w && s_ambientBg.GetHeight() == h) return;
    RenderAmbientBitmap(s_ambientBg, w, h);
}

inline void PaintBackdrop(wxGraphicsContext* gc, wxWindow* child, const wxRect& clipRect);

enum GlassElevation {
    ELEVATION_STAGE = 0,
    ELEVATION_CARD,
    ELEVATION_TOPBAR,
    ELEVATION_PREVIEW,
    ELEVATION_DOCK,
    ELEVATION_BUTTON,
    ELEVATION_INPUT,
    ELEVATION_PILL
};

inline void DrawGlass(wxGraphicsContext* gc, const wxRect2DDouble& rect, double radius, GlassElevation elevation = ELEVATION_CARD, bool hover = false, bool pressed = false, bool focused = false, bool disabled = false) {
    if (!gc || rect.m_width <= 0 || rect.m_height <= 0) return;
    const auto& th = CurrentTheme();

    double fillAlpha1 = th.fillAlphaTop;
    double fillAlpha2 = th.fillAlphaBottom;

    if (elevation == ELEVATION_BUTTON) {
        if (pressed) {
            fillAlpha1 = 0.06; fillAlpha2 = 0.04;
        } else if (hover) {
            fillAlpha1 = 0.18; fillAlpha2 = 0.10;
        } else {
            fillAlpha1 = 0.10; fillAlpha2 = 0.05;
        }
    } else if (elevation == ELEVATION_INPUT) {
        fillAlpha1 = 0.20; fillAlpha2 = 0.20;
    } else if (elevation == ELEVATION_PILL) {
        fillAlpha1 = 0.18; fillAlpha2 = 0.18;
    } else if (elevation == ELEVATION_DOCK) {
        fillAlpha1 = 0.16; fillAlpha2 = 0.10;
    }

    // Dark under-tint for dock so icons hold contrast over any video frame
    if (elevation == ELEVATION_DOCK && th.isGlass) {
        gc->SetBrush(wxBrush(wxColour(10, 12, 30, int(255 * 0.35))));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(rect.m_x, rect.m_y, rect.m_width, rect.m_height, radius);
    }

    wxColour col1(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(fillAlpha1 * 255.0, 0.0, 255.0)));
    wxColour col2(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(fillAlpha2 * 255.0, 0.0, 255.0)));

    if (elevation == ELEVATION_INPUT) {
        col1 = th.inputFill;
        col2 = th.inputFill;
    }

    if (th.isGlass) {
        gc->SetBrush(gc->CreateLinearGradientBrush(rect.m_x, rect.m_y, rect.m_x + rect.m_width, rect.m_y + rect.m_height, col1, col2));
    } else {
        // Dark: white tint; Light: dark tint for contrast
        int base = th.isDark ? 255 : 0;
        int dAlpha = pressed ? 30 : (hover ? 45 : 22);
        gc->SetBrush(wxBrush(wxColour(base, base, base, dAlpha)));
    }
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRoundedRectangle(rect.m_x, rect.m_y, rect.m_width, rect.m_height, radius);

    double borderAlpha = th.borderAlpha;
    if (elevation == ELEVATION_BUTTON) {
        borderAlpha = hover ? 0.26 : 0.18;
    } else if (elevation == ELEVATION_INPUT) {
        borderAlpha = 0.12;
    } else if (elevation == ELEVATION_PILL) {
        borderAlpha = 0.22;
    }
    // Light theme: dark border for contrast; Glass/Dark: white border
    wxColour borderBase = (!th.isGlass && !th.isDark) ? wxColour(0, 0, 40) : th.glassRgb;
    wxColour borderCol(borderBase.Red(), borderBase.Green(), borderBase.Blue(), int(std::clamp(borderAlpha * 255.0, 0.0, 255.0)));
    gc->SetPen(wxPen(borderCol, 1.0));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRoundedRectangle(rect.m_x + 0.5, rect.m_y + 0.5, rect.m_width - 1.0, rect.m_height - 1.0, std::max(0.0, radius - 0.5));

    if (th.isGlass && radius > 0.0) {
        double hlAlpha = th.highlightAlpha;
        if (elevation == ELEVATION_BUTTON && hover) hlAlpha = 0.30;
        wxColour hlCol(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(hlAlpha * 255.0, 0.0, 255.0)));
        auto path = gc->CreatePath();
        double r = std::max(0.0, radius - 0.5);
        double bx = rect.m_x + 0.5, by = rect.m_y + 0.5, bw = rect.m_width - 1.0;
        path.AddArc(bx + r, by + r, r, 3.14159265, 3.14159265 * 1.5, true);
        path.AddLineToPoint(bx + bw - r, by);
        path.AddArc(bx + bw - r, by + r, r, 3.14159265 * 1.5, 0, true);
        gc->SetPen(wxPen(hlCol, 1.0));
        gc->StrokePath(path);
    }

    if (focused) {
        gc->SetPen(wxPen(th.accent, 2.0));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(rect.m_x + 1.5, rect.m_y + 1.5, rect.m_width - 3.0, rect.m_height - 3.0, std::max(1.0, radius - 1.5));
    }
}

inline void PaintGlassBackground(wxWindow* win, wxAutoBufferedPaintDC& dc, const wxSize& size, double radius, bool isRail = false, bool isDock = false) {
    std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
    if (!g) return;
    g->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    PaintBackdrop(g.get(), win, wxRect(0, 0, size.x, size.y));
    double r = radius > 0 ? radius : (isRail ? 0 : (isDock ? win->FromDIP(16) : win->FromDIP(20)));
    GlassElevation el = isDock ? ELEVATION_DOCK : ELEVATION_CARD;
    DrawGlass(g.get(), wxRect2DDouble(0.5, 0.5, size.x - 1.0, size.y - 1.0), r, el);
    if (isRail) {
        const auto& th = CurrentTheme();
        g->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(th.borderAlpha * 255.0, 0.0, 255.0))), 1.0));
        g->StrokeLine(size.x - 0.5, 0, size.x - 0.5, size.y);
    }
}

inline void Icon(wxGraphicsContext& g, const wxString& kind, double x, double y, double size, wxColour color) {
    g.PushState();
    g.Translate(x, y);
    g.Scale(size / 24.0, size / 24.0);
    wxPen pen(color, 1.75);
    pen.SetCap(wxCAP_ROUND);
    pen.SetJoin(wxJOIN_ROUND);
    g.SetPen(pen);
    g.SetBrush(*wxTRANSPARENT_BRUSH);
    auto line = [&](double x1, double y1, double x2, double y2) { g.StrokeLine(x1, y1, x2, y2); };

    if (kind == "camera" || kind == "snapshot") {
        g.DrawRoundedRectangle(3, 6, 18, 13, 3);
        g.DrawEllipse(8, 9, 8, 8);
        if (kind == "snapshot") {
            line(7, 6, 9, 3); line(9, 3, 15, 3); line(15, 3, 17, 6);
        }
    } else if (kind == "phone") {
        g.DrawRoundedRectangle(7, 2, 10, 20, 2.5);
        line(11, 18, 13, 18);
    } else if (kind == "sliders") {
        line(3, 7, 13, 7); line(18, 7, 21, 7); g.DrawEllipse(13, 4.5, 5, 5);
        line(3, 17, 6, 17); line(11, 17, 21, 17); g.DrawEllipse(6, 14.5, 5, 5);
    } else if (kind == "theme") {
        g.DrawEllipse(3, 3, 18, 18);
        g.SetBrush(wxBrush(color));
        auto p = g.CreatePath();
        p.MoveToPoint(12, 3);
        p.AddArc(12, 12, 9, -1.5708, 1.5708, true);
        p.CloseSubpath();
        g.FillPath(p);
    } else if (kind == "help") {
        g.DrawEllipse(3, 3, 18, 18);
        auto p = g.CreatePath();
        p.MoveToPoint(9.6, 9.5);
        p.AddCurveToPoint(9.6, 6.0, 16.0, 6.0, 15.0, 10.0);
        p.AddCurveToPoint(14.0, 12.0, 12.0, 12.0, 12.0, 15.0);
        g.StrokePath(p);
        g.DrawEllipse(11.5, 17, 1, 1);
    } else if (kind == "switch") {
        auto p = g.CreatePath();
        p.MoveToPoint(4, 8); p.AddLineToPoint(16, 8);
        p.AddLineToPoint(13, 5);
        p.MoveToPoint(16, 8); p.AddLineToPoint(13, 11);
        p.MoveToPoint(20, 16); p.AddLineToPoint(8, 16);
        p.AddLineToPoint(11, 13);
        p.MoveToPoint(8, 16); p.AddLineToPoint(11, 19);
        g.StrokePath(p);
    } else if (kind == "flash") {
        auto p = g.CreatePath();
        p.MoveToPoint(13, 2);
        p.AddLineToPoint(6, 12);
        p.AddLineToPoint(12, 12);
        p.AddLineToPoint(11, 22);
        p.AddLineToPoint(18, 10);
        p.AddLineToPoint(12, 10);
        p.CloseSubpath();
        g.StrokePath(p);
    } else if (kind == "mirror") {
        line(12, 3, 12, 21);
        auto p = g.CreatePath();
        p.MoveToPoint(8, 7); p.AddLineToPoint(3, 17); p.AddLineToPoint(8, 17); p.CloseSubpath();
        p.MoveToPoint(16, 7); p.AddLineToPoint(21, 17); p.AddLineToPoint(16, 17); p.CloseSubpath();
        g.StrokePath(p);
    } else if (kind == "flip-v") {
        line(3, 12, 21, 12);
        auto p = g.CreatePath();
        p.MoveToPoint(7, 8); p.AddLineToPoint(17, 3); p.AddLineToPoint(17, 8); p.CloseSubpath();
        p.MoveToPoint(7, 16); p.AddLineToPoint(17, 21); p.AddLineToPoint(17, 16); p.CloseSubpath();
        g.StrokePath(p);
    } else if (kind == "left" || kind == "right") {
        if (kind == "right") { g.Translate(24, 0); g.Scale(-1, 1); }
        auto p = g.CreatePath();
        p.AddArc(12, 12, 8, -2.5, 2.6, true);
        g.StrokePath(p);
        line(4, 9, 4, 3); line(4, 9, 10, 9);
    } else if (kind == "plus" || kind == "minus") {
        line(6, 12, 18, 12);
        if (kind == "plus") line(12, 6, 12, 18);
    } else if (kind == "folder") {
        auto p = g.CreatePath();
        p.MoveToPoint(3, 6); p.AddLineToPoint(10, 6); p.AddLineToPoint(12, 9);
        p.AddLineToPoint(21, 9); p.AddLineToPoint(19, 20); p.AddLineToPoint(3, 20); p.CloseSubpath();
        g.StrokePath(p);
    } else if (kind == "search") {
        g.DrawEllipse(4, 3, 12, 12);
        line(14, 14, 21, 21);
    }
    g.PopState();
}

class Card;
class SegmentedControl;

class SmoothButton : public wxButton {
public:
    bool primary = false, ghost = false, selected = false, leading = false;
    bool disclosure = false, expanded = false, pill = false;
    wxString icon;
    wxString valueTag;

    SmoothButton(wxWindow* parent, int id, const wxString& label, bool p = false)
        : wxButton(parent, id, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT), primary(p) {
        SetFont(wxFontInfo(10).FaceName("Segoe UI"));
        wxColour bg = parent ? parent->GetBackgroundColour() : Surface();
        if (!bg.IsOk() || (bg == *wxWHITE && Dark())) bg = Surface();
        SetBackgroundColour(bg);
        MakeOwnerDrawn();
        SetMinSize(FromDIP(wxSize(-1, 38)));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) { /* prevent default GDI erasure */ });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover = true; Refresh(false); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover = false; Refresh(false); e.Skip(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });
    }

    bool MSWOnDraw(WXDRAWITEMSTRUCT* raw) override;

    wxSize DoGetBestSize() const override {
        if (!icon.empty()) return FromDIP(wxSize(38, 38));
        wxClientDC dc(const_cast<SmoothButton*>(this));
        dc.SetFont(GetFont());
        auto sz = dc.GetTextExtent(wxControl::RemoveMnemonics(GetLabel()));
        int extra = disclosure ? 56 : 32; // 2 x 16px padding
        if (!valueTag.empty()) {
            auto vs = dc.GetTextExtent(valueTag);
            extra += vs.x + 12;
        }
        return wxSize(sz.x + FromDIP(extra), std::max(sz.y + FromDIP(18), FromDIP(38)));
    }

private:
    bool hover = false;
};

inline wxButton* Button(wxWindow* parent, const wxString& label, const wxString& help, bool primary = false, int id = wxID_ANY) {
    auto* b = new SmoothButton(parent, id, label, primary);
    b->SetName(label);
    b->SetToolTip(help);
    b->SetClientObject(new Role(false, primary));
    return b;
}

inline SmoothButton* IconButton(wxWindow* parent, const wxString& kind, const wxString& label, const wxString& help) {
    auto* b = new SmoothButton(parent, wxID_ANY, label);
    b->icon = kind;
    b->ghost = true;
    b->SetMinSize(parent->FromDIP(wxSize(38, 38)));
    b->SetName(label);
    b->SetToolTip(help);
    return b;
}

class GlassLabel : public wxControl {
public:
    GlassLabel(wxWindow* parent, wxWindowID id, const wxString& label, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = 0)
        : wxControl(parent, id, pos, size, wxNO_BORDER | style), m_label(label), m_style(style) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(wxFontInfo(10).FaceName("Segoe UI"));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassLabel::OnPaint, this);
    }

    void SetLabel(const wxString& label) override {
        if (m_label != label) {
            m_label = label;
            InvalidateBestSize();
            Refresh(false);
        }
    }
    void SetLabelText(const wxString& text) {
        SetLabel(text);
    }
    wxString GetLabel() const override { return m_label; }

    void SetMuted(bool m) { m_muted = m; Refresh(false); }
    void SetBold(bool b) {
        auto f = GetFont();
        f.SetWeight(b ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        SetFont(f);
        InvalidateBestSize();
        Refresh(false);
    }

    void Wrap(int width) {
        m_wrapWidth = width;
        InvalidateBestSize();
        Refresh(false);
    }

protected:
    wxSize DoGetBestSize() const override {
        if (m_label.empty()) return wxSize(0, FromDIP(16));
        wxClientDC dc(const_cast<GlassLabel*>(this));
        dc.SetFont(GetFont());
        bool isMultiline = (m_wrapWidth > 0) || m_label.Contains("\n");
        if (isMultiline) {
            int wLimit = m_wrapWidth > 0 ? m_wrapWidth : 0;
            auto lines = SplitLines(dc, m_label, wLimit);
            wxCoord lineH = 0, charW = 0;
            dc.GetTextExtent("Ag", &charW, &lineH);
            lineH = std::max(lineH, (wxCoord)FromDIP(14));

            wxCoord maxW = 0;
            wxCoord totalH = 0;
            for (const auto& line : lines) {
                wxCoord lw = 0, lh = 0;
                dc.GetTextExtent(line, &lw, &lh);
                if (lw > maxW) maxW = lw;
                totalH += std::max(lh, lineH) + FromDIP(2);
            }
            int bestW = m_wrapWidth > 0 ? m_wrapWidth : (maxW + FromDIP(8));
            return wxSize(bestW, std::max(totalH, (wxCoord)FromDIP(16)));
        } else {
            auto sz = dc.GetTextExtent(m_label);
            // +8dp safety margin: GDI vs Direct2D text measurement can differ
            // by 1-3px at 100% DPI and up to 5px at 150%; the extra pixels
            // prevent the last character from being clipped at the label edge.
            return wxSize(sz.x + FromDIP(8), std::max(sz.y + FromDIP(2), FromDIP(16)));
        }
    }

private:
    wxString m_label;
    long m_style = 0;
    bool m_muted = false;
    int m_wrapWidth = 0;

    std::vector<wxString> SplitLines(wxDC& dc, const wxString& text, int width) const {
        std::vector<wxString> result;
        wxStringTokenizer lineTok(text, "\n\r", wxTOKEN_RET_EMPTY_ALL);
        while (lineTok.HasMoreTokens()) {
            wxString paragraph = lineTok.GetNextToken();
            if (width <= 0) {
                result.push_back(paragraph);
                continue;
            }
            wxStringTokenizer wordTok(paragraph, " \t", wxTOKEN_RET_DELIMS);
            wxString current;
            while (wordTok.HasMoreTokens()) {
                wxString token = wordTok.GetNextToken();
                wxString candidate = current.empty() ? token : current + token;
                wxCoord w = 0, h = 0;
                dc.GetTextExtent(candidate, &w, &h);
                if (w > width && !current.empty()) {
                    result.push_back(current);
                    current = token.Trim(false);
                } else {
                    current = candidate;
                }
            }
            if (!current.empty() || paragraph.empty()) {
                result.push_back(current);
            }
        }
        if (result.empty()) result.push_back(text);
        return result;
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // Clip to client rect — prevents D2D text from bleeding past the
        // allocated sizer rect (GDI vs D2D measurement can differ by ~2-5px)
        gc->Clip(0, 0, s.x, s.y);

        // 1. Paint real backdrop behind this label!
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        // 2. Draw text
        const auto& th = CurrentTheme();
        wxColour txtCol = m_muted ? th.muted : th.text;
        auto* role = dynamic_cast<Role*>(GetClientObject());
        if (role && role->muted) txtCol = th.muted;
        gc->SetFont(GetFont(), txtCol);

        bool isMultiline = (m_wrapWidth > 0) || m_label.Contains("\n");
        if (isMultiline) {
            wxClientDC cdc(this);
            cdc.SetFont(GetFont());
            int wLimit = m_wrapWidth > 0 ? m_wrapWidth : s.x;
            auto lines = SplitLines(cdc, m_label, wLimit);
            double y = 0;
            for (const auto& line : lines) {
                double lw, lh;
                gc->GetTextExtent(line, &lw, &lh);
                double tx = 0;
                if (m_style & wxALIGN_RIGHT) {
                    tx = s.x - lw;
                } else if (m_style & wxALIGN_CENTER_HORIZONTAL) {
                    tx = (s.x - lw) / 2.0;
                }
                if (tx < 0) tx = 0;
                gc->DrawText(line, tx, y);
                y += lh + FromDIP(2);
            }
        } else {
            double tw, th_ext;
            gc->GetTextExtent(m_label, &tw, &th_ext);
            double ty = (s.y - th_ext) / 2.0;
            double tx = 0;
            if (m_style & wxALIGN_RIGHT) {
                tx = s.x - tw;
            } else if (m_style & wxALIGN_CENTER_HORIZONTAL) {
                tx = (s.x - tw) / 2.0;
            }
            if (tx < 0) tx = 0;
            gc->DrawText(m_label, tx, ty);
        }
    }
};

inline GlassLabel* Label(wxWindow* parent, const wxString& text, int size = 10, bool muted = false, bool bold = false, long style = 0) {
    auto* label = new GlassLabel(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
    label->SetFont(wxFontInfo(size).FaceName("Segoe UI").Weight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL));
    label->SetMuted(muted);
    label->SetClientObject(new Role(muted, false));
    return label;
}

inline void Help(wxWindow* w, const wxString& name, const wxString& text) {
    w->SetName(name);
    w->SetToolTip(text);
}

class Card : public wxPanel {
public:
    bool rail = false;
    bool isDock = false;
    double customRadius = -1.0;

    Card(wxWindow* parent, bool isRail = false, bool dock = false)
        : wxPanel(parent), rail(isRail), isDock(dock) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            auto s = GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

            // 1. Paint real backdrop behind this card
            PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

            // 2. Draw Glass
            double r = customRadius >= 0 ? customRadius : (rail ? 0 : (isDock ? FromDIP(16) : FromDIP(20)));
            GlassElevation el = isDock ? ELEVATION_DOCK : (rail ? ELEVATION_CARD : ELEVATION_CARD);
            DrawGlass(gc.get(), wxRect2DDouble(0.5, 0.5, s.x - 1.0, s.y - 1.0), r, el);

            if (rail) {
                const auto& th = CurrentTheme();
                gc->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(th.borderAlpha * 255.0, 0.0, 255.0))), 1.0));
                gc->StrokeLine(s.x - 0.5, 0, s.x - 0.5, s.y);
            }
        });
    }
};

class LiveChip : public wxPanel {
public:
    bool isLive = false;
    wxString text = "Not connected";
    // Auto-size: no fixed constructor width — DoGetBestSize() measures content
    LiveChip(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(wxFontInfo(9).FaceName("Segoe UI").Weight(wxFONTWEIGHT_MEDIUM));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            auto s = GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            g->SetAntialiasMode(wxANTIALIAS_DEFAULT);

            // 1. Backdrop
            PaintBackdrop(g.get(), this, wxRect(0, 0, s.x, s.y));

            // 2. Glass pill
            DrawGlass(g.get(), wxRect2DDouble(0.5, 0.5, s.x - 1.0, s.y - 1.0), s.y / 2.0, ELEVATION_PILL);

            const auto& th = CurrentTheme();
            wxColour dotCol = isLive ? th.live : th.muted;
            if (isLive) {
                g->SetBrush(wxBrush(wxColour(dotCol.Red(), dotCol.Green(), dotCol.Blue(), 70)));
                g->SetPen(*wxTRANSPARENT_PEN);
                g->DrawEllipse(FromDIP(7), (s.y - FromDIP(12)) / 2.0, FromDIP(12), FromDIP(12));
            }
            g->SetBrush(wxBrush(dotCol));
            g->SetPen(*wxTRANSPARENT_PEN);
            g->DrawEllipse(FromDIP(10), (s.y - FromDIP(6)) / 2.0, FromDIP(6), FromDIP(6));

            wxColour txtCol = isLive ? th.live : th.muted;
            g->SetFont(GetFont(), txtCol);
            double tw, th_ext;
            g->GetTextExtent(text, &tw, &th_ext);
            g->DrawText(text, FromDIP(22), (s.y - th_ext) / 2.0);
        });
    }
    void SetLive(bool live, const wxString& t = "") {
        isLive = live;
        if (!t.empty()) text = t;
        InvalidateBestSize();       // force sizer to re-query DoGetBestSize
        if (GetParent()) GetParent()->Layout();
        Refresh(false);
    }

protected:
    wxSize DoGetBestSize() const override {
        wxClientDC dc(const_cast<LiveChip*>(this));
        dc.SetFont(GetFont());
        // dot area = 22dp, right padding = 12dp
        auto sz = dc.GetTextExtent(text.empty() ? wxString("Not connected") : text);
        return wxSize(FromDIP(22) + sz.x + FromDIP(12), FromDIP(26));
    }
};

class StatusDot : public wxPanel {
public:
    bool isLive = false;
    StatusDot(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(16, 16))) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(parent->FromDIP(wxSize(16, 16)));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            auto s = GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            g->SetAntialiasMode(wxANTIALIAS_DEFAULT);

            // 1. Backdrop
            PaintBackdrop(g.get(), this, wxRect(0, 0, s.x, s.y));

            const auto& th = CurrentTheme();
            wxColour col = isLive ? th.live : th.muted;
            if (isLive) {
                g->SetBrush(wxBrush(wxColour(col.Red(), col.Green(), col.Blue(), 70)));
                g->SetPen(*wxTRANSPARENT_PEN);
                g->DrawEllipse((s.x - FromDIP(14)) / 2.0, (s.y - FromDIP(14)) / 2.0, FromDIP(14), FromDIP(14));
            }
            g->SetBrush(wxBrush(col));
            g->SetPen(*wxTRANSPARENT_PEN);
            g->DrawEllipse((s.x - FromDIP(8)) / 2.0, (s.y - FromDIP(8)) / 2.0, FromDIP(8), FromDIP(8));
        });
    }
    void SetLive(bool live) {
        isLive = live;
        Refresh(false);
    }
};

class Separator : public wxWindow {
public:
    Separator(wxWindow* parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(1, 20))) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(parent->FromDIP(wxSize(9, 20)));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            auto s = GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

            const auto& th = CurrentTheme();
            gc->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(std::clamp(th.borderAlpha * 255.0, 0.0, 255.0))), 1));
            gc->StrokeLine(s.x / 2.0, FromDIP(2), s.x / 2.0, s.y - FromDIP(2));
        });
    }
};

class GlassInspectorPanel;
using GlassScrolledPanel = GlassInspectorPanel;

class SegmentedControl : public wxPanel {
public:
    std::vector<SmoothButton*> buttons;
    std::function<void(int index, const wxString& label)> onSelect;
    int selectedIndex = 0;

    SegmentedControl(wxWindow* parent, const std::vector<wxString>& items)
        : wxPanel(parent, wxID_ANY) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);

        for (size_t i = 0; i < items.size(); ++i) {
            auto* b = new SmoothButton(this, wxID_ANY, items[i]);
            b->ghost = (i != 0);
            b->selected = (i == 0);
            b->pill = false;
            b->SetMinSize(FromDIP(wxSize(-1, 30)));
            b->SetFont(wxFontInfo(9).FaceName("Segoe UI").Weight(wxFONTWEIGHT_MEDIUM));
            b->Bind(wxEVT_BUTTON, [this, i, label = items[i]](wxCommandEvent&) {
                SetSelection((int)i);
                if (onSelect) onSelect((int)i, label);
            });
            buttons.push_back(b);
            sizer->Add(b, 1, wxEXPAND | wxRIGHT, (i + 1 < items.size()) ? FromDIP(2) : 0);
        }
        outerSizer->Add(sizer, 1, wxEXPAND | wxALL, FromDIP(3));
        SetSizer(outerSizer);

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            auto s = GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            g->SetAntialiasMode(wxANTIALIAS_DEFAULT);

            // 1. Paint real backdrop behind segmented control
            PaintBackdrop(g.get(), this, wxRect(0, 0, s.x, s.y));

            // 2. Container: fill from inputFill, border contrasts with background
            const auto& th = CurrentTheme();
            g->SetBrush(wxBrush(th.inputFill));
            wxColour borderBase = (!th.isGlass && !th.isDark) ? wxColour(0, 0, 40) : th.glassRgb;
            wxColour bCol(borderBase.Red(), borderBase.Green(), borderBase.Blue(), int(255 * 0.12));
            g->SetPen(wxPen(bCol, 1.0));
            g->DrawRoundedRectangle(0.5, 0.5, s.x - 1.0, s.y - 1.0, FromDIP(12));
        });
    }

    void SetSelection(int index) {
        if (index < 0 || index >= (int)buttons.size()) return;
        selectedIndex = index;
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i]->selected = (int(i) == index);
            buttons[i]->ghost = (int(i) != index);
            buttons[i]->Refresh();
        }
    }

    int GetSelection() const { return selectedIndex; }
};

class GlassDialogBase : public wxDialog {
public:
    GlassDialogBase(wxWindow* parent, wxWindowID id, const wxString& title,
                    const wxPoint& pos = wxDefaultPosition,
                    const wxSize& size = wxDefaultSize,
                    long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER,
                    const wxString& name = wxDialogNameStr)
        : wxDialog(parent, id, title, pos, size, style, name)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassDialogBase::OnPaint, this);
        Bind(wxEVT_SIZE, &GlassDialogBase::OnSize, this);
        Bind(wxEVT_DPI_CHANGED, &GlassDialogBase::OnDPIChanged, this);
        Bind(wxEVT_CHAR_HOOK, &GlassDialogBase::OnCharHook, this);
        UpdateTitleBarTheme();
    }

    virtual ~GlassDialogBase() = default;

    wxBitmap& GetBackdropBitmap() { return m_ambientBg; }

    void UpdateTitleBarTheme() {
        ApplyImmersiveDarkMode((HWND)GetHWND(), CurrentTheme().isDark);
    }

    void EnsureBackdrop() {
        auto sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;
        if (!m_ambientBg.IsOk() || m_ambientBg.GetWidth() != sz.x || m_ambientBg.GetHeight() != sz.y || m_themeMode != Studio::appearance) {
            m_themeMode = Studio::appearance;
            UpdateTitleBarTheme();
            RenderDialogBackdrop(sz.x, sz.y);
        }
    }

    void InvalidateBackdrop() {
        m_ambientBg = wxNullBitmap;
        Refresh();
    }

protected:
    wxBitmap m_ambientBg;
    int m_themeMode = -1;

    virtual void RenderDialogBackdrop(int w, int h) {
        if (w <= 0 || h <= 0) return;
        m_ambientBg.Create(w, h, 32);
        wxMemoryDC memDC(m_ambientBg);
        std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(memDC));
        if (!g) return;
        g->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Ambient gradient and orbs (100% opaque base)
        RenderAmbientToContext(g.get(), w, h);

        // 2. Draw one glass sheet across the dialog (radius 20, standard DrawGlass recipe)
        const auto& th = CurrentTheme();
        int m = FromDIP(8);
        double rad = FromDIP(20);
        if (th.isGlass) {
            DrawGlass(g.get(), wxRect2DDouble(m + 0.5, m + 0.5, w - 2 * m - 1.0, h - 2 * m - 1.0), rad, ELEVATION_CARD);
        } else {
            g->SetBrush(wxBrush(Surface()));
            wxColour bCol = (!th.isGlass && !th.isDark) ? wxColour(0, 0, 40, int(255 * 0.12)) : wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.12));
            g->SetPen(wxPen(bCol, 1.0));
            g->DrawRoundedRectangle(m + 0.5, m + 0.5, w - 2 * m - 1.0, h - 2 * m - 1.0, rad);
        }
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;
        EnsureBackdrop();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        if (m_ambientBg.IsOk()) {
            gc->DrawBitmap(m_ambientBg, 0, 0, sz.x, sz.y);
        }
    }

    void OnSize(wxSizeEvent& e) {
        m_ambientBg = wxNullBitmap;
        Refresh(false);
        e.Skip();
    }

    void OnDPIChanged(wxDPIChangedEvent& e) {
        m_ambientBg = wxNullBitmap;
        UpdateTitleBarTheme();
        Refresh(false);
        e.Skip();
    }

    virtual void OnCharHook(wxKeyEvent& evt) {
        if (evt.GetKeyCode() == WXK_ESCAPE) {
            EndModal(wxID_CANCEL);
        } else if (evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_NUMPAD_ENTER) {
            wxWindow* focused = FindFocus();
            if (auto* btn = dynamic_cast<wxButton*>(focused)) {
                wxCommandEvent cmd(wxEVT_BUTTON, btn->GetId());
                cmd.SetEventObject(btn);
                btn->ProcessWindowEvent(cmd);
            } else {
                EndModal(wxID_OK);
            }
        } else {
            evt.Skip();
        }
    }
};

inline void PaintBackdrop(wxGraphicsContext* gc, wxWindow* child, const wxRect& clipRect) {
    if (!gc || !child) return;
    wxWindow* top = child;
    while (top && !top->IsTopLevel() && top->GetParent()) top = top->GetParent();

    wxBitmap* sourceBmp = nullptr;
    if (auto* dlg = dynamic_cast<GlassDialogBase*>(top)) {
        dlg->EnsureBackdrop();
        sourceBmp = &dlg->GetBackdropBitmap();
    } else {
        if (top) {
            auto sz = top->GetClientSize();
            if (sz.x > 0 && sz.y > 0) {
                if (!s_ambientBg.IsOk() || s_ambientBg.GetWidth() != sz.x || s_ambientBg.GetHeight() != sz.y) {
                    RenderAmbientBackground(sz.x, sz.y);
                }
            }
        }
        sourceBmp = &s_ambientBg;
    }

    if (!sourceBmp || !sourceBmp->IsOk() || !top) return;

    // 1. Draw matching source rect from ambient background cache
    wxPoint originInTop;
    if (child == top) {
        originInTop = wxPoint(0, 0);
    } else {
        wxPoint screenPt = child->ClientToScreen(wxPoint(0, 0));
        originInTop = top->ScreenToClient(screenPt);
    }

    gc->PushState();
    gc->Clip(clipRect.x, clipRect.y, clipRect.width, clipRect.height);
    gc->DrawBitmap(*sourceBmp, -originInTop.x, -originInTop.y, sourceBmp->GetWidth(), sourceBmp->GetHeight());

    // 2. Composite intermediate glass surfaces (Card, Dock, SegmentedControl) between child and top
    if (child != top) {
        std::vector<wxWindow*> ancestors;
        wxWindow* p = child->GetParent();
        while (p && p != top) {
            ancestors.push_back(p);
            p = p->GetParent();
        }
        std::reverse(ancestors.begin(), ancestors.end());

        for (wxWindow* anc : ancestors) {
            if (auto* card = dynamic_cast<Card*>(anc)) {
                wxPoint cardOrigin = child->ScreenToClient(card->ClientToScreen(wxPoint(0, 0)));
                auto cardSize = card->GetClientSize();
                double r = card->customRadius >= 0 ? card->customRadius : (card->rail ? 0 : (card->isDock ? card->FromDIP(16) : card->FromDIP(20)));
                GlassElevation el = card->isDock ? ELEVATION_DOCK : ELEVATION_CARD;
                DrawGlass(gc, wxRect2DDouble(cardOrigin.x + 0.5, cardOrigin.y + 0.5, cardSize.x - 1.0, cardSize.y - 1.0), r, el);
            } else if (dynamic_cast<SegmentedControl*>(anc)) {
                wxPoint segOrigin = child->ScreenToClient(anc->ClientToScreen(wxPoint(0, 0)));
                auto segSize = anc->GetClientSize();
                const auto& th = CurrentTheme();
                gc->SetBrush(wxBrush(th.inputFill));
                wxColour borderBase = (!th.isGlass && !th.isDark) ? wxColour(0, 0, 40) : th.glassRgb;
                wxColour bCol(borderBase.Red(), borderBase.Green(), borderBase.Blue(), int(255 * 0.12));
                gc->SetPen(wxPen(bCol, 1.0));
                gc->DrawRoundedRectangle(segOrigin.x + 0.5, segOrigin.y + 0.5, segSize.x - 1.0, segSize.y - 1.0, anc->FromDIP(12));
            }
        }
    }
    gc->PopState();
}

class QRCodeCard : public wxControl {
public:
    QRCodeCard(wxWindow* parent, const wxImage& qrImg, int cardSizeDip = 240)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER),
          m_qrImage(qrImg), m_cardSizeDip(cardSizeDip)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &QRCodeCard::OnPaint, this);
    }

    wxSize DoGetBestSize() const override {
        int sz = FromDIP(m_cardSizeDip);
        return wxSize(sz, sz);
    }

private:
    wxImage m_qrImage;
    int m_cardSizeDip;

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Paint backdrop under rounded card corners
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        // 2. White rounded card (radius 16)
        double rad = FromDIP(16);
        gc->SetBrush(*wxWHITE_BRUSH);
        gc->SetPen(wxPen(wxColour(0, 0, 0, 20), 1.0)); // subtle border
        gc->DrawRoundedRectangle(0.5, 0.5, s.x - 1.0, s.y - 1.0, rad);

        // 3. Render crisp QR code centered inside white card (no tint, no overlay)
        if (m_qrImage.IsOk()) {
            int quietPadding = FromDIP(14);
            int availableSize = std::min(s.x, s.y) - 2 * quietPadding;
            if (availableSize > 0 && m_qrImage.GetWidth() > 0) {
                int scale = std::max(1, availableSize / m_qrImage.GetWidth());
                int targetW = m_qrImage.GetWidth() * scale;
                int targetH = m_qrImage.GetHeight() * scale;
                if (targetW < availableSize * 0.85) {
                    targetW = availableSize;
                    targetH = availableSize;
                }
                wxBitmap qrBmp(m_qrImage.Scale(targetW, targetH, wxIMAGE_QUALITY_NEAREST));
                double qrX = (s.x - targetW) / 2.0;
                double qrY = (s.y - targetH) / 2.0;
                gc->DrawBitmap(qrBmp, qrX, qrY, targetW, targetH);
            }
        }
    }
};

class GlassAddressChip : public wxControl {
public:
    GlassAddressChip(wxWindow* parent, const wxString& text)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER | wxWANTS_CHARS),
          m_text(text)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(wxFontInfo(10).FaceName("Segoe UI Semibold"));
        SetCursor(wxCursor(wxCURSOR_IBEAM));

        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassAddressChip::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &GlassAddressChip::OnLeftDown, this);
        Bind(wxEVT_LEFT_DCLICK, &GlassAddressChip::OnLeftDClick, this);
        Bind(wxEVT_MOTION, &GlassAddressChip::OnMotion, this);
        Bind(wxEVT_LEFT_UP, &GlassAddressChip::OnLeftUp, this);
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent&) { Refresh(false); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) { m_selStart = m_selEnd = -1; Refresh(false); });
        Bind(wxEVT_KEY_DOWN, &GlassAddressChip::OnKeyDown, this);
    }

    wxString GetText() const { return m_text; }

    wxSize DoGetBestSize() const override {
        wxClientDC dc(const_cast<GlassAddressChip*>(this));
        dc.SetFont(GetFont());
        auto sz = dc.GetTextExtent(m_text);
        return wxSize(sz.x + FromDIP(28), FromDIP(32));
    }

private:
    wxString m_text;
    int m_selStart = -1;
    int m_selEnd = -1;
    bool m_dragging = false;

    int GetCharIndexAt(int x) {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        auto sz = dc.GetTextExtent(m_text);
        int textX = (GetClientSize().x - sz.x) / 2;
        int relX = x - textX;
        if (relX <= 0) return 0;
        for (int i = 1; i <= (int)m_text.length(); ++i) {
            auto sub = m_text.SubString(0, i - 1);
            int w = dc.GetTextExtent(sub).x;
            if (w >= relX) return i - 1;
        }
        return (int)m_text.length();
    }

    void OnLeftDown(wxMouseEvent& e) {
        SetFocus();
        m_selStart = GetCharIndexAt(e.GetX());
        m_selEnd = m_selStart;
        m_dragging = true;
        Refresh(false);
    }

    void OnLeftDClick(wxMouseEvent&) {
        SetFocus();
        m_selStart = 0;
        m_selEnd = (int)m_text.length();
        Refresh(false);
    }

    void OnMotion(wxMouseEvent& e) {
        if (m_dragging && e.LeftIsDown()) {
            m_selEnd = GetCharIndexAt(e.GetX());
            Refresh(false);
        }
    }

    void OnLeftUp(wxMouseEvent&) {
        m_dragging = false;
    }

    void OnKeyDown(wxKeyEvent& e) {
        if (e.ControlDown()) {
            if (e.GetKeyCode() == 'C' || e.GetKeyCode() == 'c') {
                CopySelection();
                return;
            } else if (e.GetKeyCode() == 'A' || e.GetKeyCode() == 'a') {
                m_selStart = 0;
                m_selEnd = (int)m_text.length();
                Refresh(false);
                return;
            }
        }
        e.Skip();
    }

    void CopySelection() {
        wxString copyText = m_text;
        if (m_selStart >= 0 && m_selEnd >= 0 && m_selStart != m_selEnd) {
            int s = std::min(m_selStart, m_selEnd);
            int end = std::max(m_selStart, m_selEnd);
            copyText = m_text.SubString(s, end - 1);
        }
        if (!copyText.empty() && wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(copyText));
            wxTheClipboard->Close();
        }
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Paint dialog backdrop first
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        // 2. Chip: fill rgba(0,0,0,.20), radius 12, border rgba(255,255,255,.12)
        double rad = FromDIP(12);
        gc->SetBrush(wxBrush(wxColour(0, 0, 0, int(255 * 0.20))));
        gc->SetPen(wxPen(wxColour(255, 255, 255, int(255 * 0.12)), 1.0));
        gc->DrawRoundedRectangle(0.5, 0.5, s.x - 1.0, s.y - 1.0, rad);

        // 3. Draw text
        gc->SetFont(GetFont(), CurrentTheme().text);
        double tw, th;
        gc->GetTextExtent(m_text, &tw, &th);
        double textX = (s.x - tw) / 2.0;
        double textY = (s.y - th) / 2.0;

        // Selection highlight if any
        if (m_selStart >= 0 && m_selEnd >= 0 && m_selStart != m_selEnd) {
            int sIdx = std::min(m_selStart, m_selEnd);
            int eIdx = std::max(m_selStart, m_selEnd);
            wxClientDC cdc(this);
            cdc.SetFont(GetFont());
            double offX = sIdx > 0 ? cdc.GetTextExtent(m_text.SubString(0, sIdx - 1)).x : 0;
            double selW = cdc.GetTextExtent(m_text.SubString(sIdx, eIdx - 1)).x;
            gc->SetBrush(wxBrush(wxColour(CurrentTheme().accent.Red(), CurrentTheme().accent.Green(), CurrentTheme().accent.Blue(), 120)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(textX + offX, textY - 1, selW, th + 2);
        }

        gc->DrawText(m_text, textX, textY);
    }
};

class AboutDialog : public GlassDialogBase {
public:
    AboutDialog(wxWindow* parent)
        : GlassDialogBase(parent, wxID_ANY, "About PrismForge", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* title = Studio::Label(this, "PrismForge Studio", 16, false, true);
        auto* subtitle = Studio::Label(this, "Android phone as a high-performance webcam", 10, true);
        auto* version = Studio::Label(this, "Development build  -  Studio UI", 9, true);

        sizer->Add(title, 0, wxALL | wxALIGN_CENTER, FromDIP(20));
        sizer->Add(subtitle, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER, FromDIP(14));
        sizer->Add(version, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER, FromDIP(20));

        auto* okBtn = Studio::Button(this, "Close", "Close about dialog", true, wxID_OK);
        okBtn->SetDefault();
        sizer->Add(okBtn, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(20));

        SetSizerAndFit(sizer);
        SetMinSize(GetSize());
        CenterOnParent();
    }
};

class GlassSelect : public wxControl {
public:
    GlassSelect(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize)
        : wxControl(parent, id, pos, size, wxNO_BORDER | wxTAB_TRAVERSAL) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(wxFontInfo(10).FaceName("Segoe UI"));
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassSelect::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &GlassSelect::OnLeftDown, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_hover = true; Refresh(false); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_hover = false; Refresh(false); e.Skip(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });

        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            int code = e.GetKeyCode();
            if (code == WXK_DOWN) {
                int next = m_selection + 1;
                while (next < (int)m_items.size() && !m_selectable[next]) ++next;
                if (next < (int)m_items.size()) {
                    SetSelection(next);
                    wxCommandEvent evt(wxEVT_CHOICE, GetId());
                    evt.SetEventObject(this);
                    evt.SetInt(m_selection);
                    evt.SetString(GetStringSelection());
                    ProcessWindowEvent(evt);
                }
            } else if (code == WXK_UP) {
                int next = m_selection - 1;
                while (next >= 0 && !m_selectable[next]) --next;
                if (next >= 0) {
                    SetSelection(next);
                    wxCommandEvent evt(wxEVT_CHOICE, GetId());
                    evt.SetEventObject(this);
                    evt.SetInt(m_selection);
                    evt.SetString(GetStringSelection());
                    ProcessWindowEvent(evt);
                }
            } else if (code == WXK_SPACE || code == WXK_RETURN) {
                wxMouseEvent dummy;
                OnLeftDown(dummy);
            } else {
                e.Skip();
            }
        });
    }

    int Append(const wxString& item) {
        m_items.push_back(item);
        m_selectable.push_back(true);
        if (m_selection < 0) m_selection = (int)m_items.size()-1;
        Refresh(false);
        return (int)m_items.size() - 1;
    }

    int AppendHeading(const wxString& item) {
        int index = Append(item);
        m_selectable[index] = false;
        if (m_selection == index) m_selection = -1;
        return index;
    }
    bool IsSelectable(int index) const {
        return index >= 0 && index < (int)m_selectable.size() && m_selectable[index];
    }

    int Append(const wxString& item, void* clientData) {
        wxUnusedVar(clientData);
        return Append(item);
    }

    void Append(const wxArrayString& items) {
        for (const auto& it : items) Append(it);
    }

    void Set(const wxArrayString& items) {
        Clear();
        Append(items);
    }

    void Clear() {
        m_items.clear();
        m_selectable.clear();
        m_selection = -1;
        Refresh(false);
    }

    unsigned int GetCount() const { return (unsigned int)m_items.size(); }
    bool IsEmpty() const { return m_items.empty(); }

    void SetSelection(int n) {
        if (IsSelectable(n)) {
            m_selection = n;
            Refresh(false);
        } else if (m_items.empty()) {
            m_selection = -1;
            Refresh(false);
        }
    }

    void Select(int n) { SetSelection(n); }
    int GetSelection() const { return m_selection; }

    wxString GetStringSelection() const {
        if (m_selection >= 0 && m_selection < (int)m_items.size()) {
            return m_items[m_selection];
        }
        return "";
    }

    bool SetStringSelection(const wxString& s) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i] == s && m_selectable[i]) {
                SetSelection((int)i);
                return true;
            }
        }
        return false;
    }

    wxString GetString(int n) const {
        if (n >= 0 && n < (int)m_items.size()) return m_items[n];
        return "";
    }

    wxString GetString(unsigned int n) const {
        return GetString((int)n);
    }

    void SetString(int n, const wxString& s) {
        if (n >= 0 && n < (int)m_items.size()) {
            m_items[n] = s;
            Refresh(false);
        }
    }

    void SetString(unsigned int n, const wxString& s) {
        SetString((int)n, s);
    }

    int FindString(const wxString& s, bool bCase = false) const {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (bCase ? m_items[i] == s : m_items[i].IsSameAs(s, false)) {
                return (int)i;
            }
        }
        return wxNOT_FOUND;
    }

protected:
    wxSize DoGetBestSize() const override {
        return FromDIP(wxSize(160, 32));
    }

private:
    std::vector<wxString> m_items;
    std::vector<bool> m_selectable;
    int m_selection = -1;
    bool m_hover = false;

    void OnLeftDown(wxMouseEvent&) {
        if (!IsEnabled()) return;
        SetFocus();
        if (m_items.empty()) return;
        wxMenu menu;
        for (size_t i = 0; i < m_items.size(); ++i) {
            const int id = wxID_HIGHEST + 1000 + (int)i;
            if (!m_selectable[i]) { menu.Append(id, m_items[i])->Enable(false); continue; }
            auto* item = menu.AppendCheckItem(id, m_items[i]);
            if ((int)i == m_selection) item->Check(true);
            menu.Bind(wxEVT_MENU, [this, i](wxCommandEvent&) {
                SetSelection((int)i);
                wxCommandEvent evt(wxEVT_CHOICE, GetId());
                evt.SetEventObject(this);
                evt.SetInt((int)i);
                evt.SetString(m_items[i]);
                ProcessWindowEvent(evt);
            }, wxID_HIGHEST + 1000 + (int)i);
        }
        PopupMenu(&menu, 0, GetSize().y);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Paint real backdrop behind select field
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        const auto& th = CurrentTheme();
        const bool enabled = IsEnabled();
        const bool focused = (FindFocus() == this);
        const double rad = FromDIP(11);

        // 2. Rounded field — fill from theme inputFill; border is white on dark/glass, dark on light
        gc->SetBrush(wxBrush(th.inputFill));
        double borderAlpha = (m_hover && enabled) ? 0.22 : 0.12;
        wxColour borderBase = (!th.isGlass && !th.isDark) ? wxColour(0, 0, 40) : th.glassRgb;
        wxColour borderCol(borderBase.Red(), borderBase.Green(), borderBase.Blue(), int(255 * borderAlpha));
        gc->SetPen(wxPen(borderCol, 1.0));
        gc->DrawRoundedRectangle(0.5, 0.5, s.x - 1.0, s.y - 1.0, rad);

        // 3. Top highlight (if glass)
        if (th.isGlass) {
            auto path = gc->CreatePath();
            double r = rad - 0.5;
            path.AddArc(0.5 + r, 0.5 + r, r, 3.14159265, 3.14159265 * 1.5, true);
            path.AddLineToPoint(s.x - 0.5 - r, 0.5);
            path.AddArc(s.x - 0.5 - r, 0.5 + r, r, 3.14159265 * 1.5, 0, true);
            gc->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.18)), 1.0));
            gc->StrokePath(path);
        }

        // 4. Focus ring
        if (focused && enabled) {
            gc->SetPen(wxPen(th.accent, FromDIP(2)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawRoundedRectangle(1.5, 1.5, s.x - 3.0, s.y - 3.0, rad - 1.0);
        }

        // 5. Text label
        wxString txt = GetStringSelection();
        if (txt.empty() && !m_items.empty()) txt = m_items[0];
        wxColour textCol = enabled ? th.text : wxColour(th.text.Red(), th.text.Green(), th.text.Blue(), int(255 * 0.40));
        gc->SetFont(GetFont(), textCol);
        double tw, th_ext;
        gc->GetTextExtent(txt, &tw, &th_ext);
        gc->DrawText(txt, FromDIP(10), (s.y - th_ext) / 2.0);

        // 6. Chevron
        double cx = s.x - FromDIP(14), cy = s.y / 2.0;
        wxPen chevPen(enabled ? (m_hover ? th.text : th.muted) : wxColour(th.muted.Red(), th.muted.Green(), th.muted.Blue(), int(255 * 0.40)), 1.6);
        chevPen.SetCap(wxCAP_ROUND);
        chevPen.SetJoin(wxJOIN_ROUND);
        gc->SetPen(chevPen);
        gc->StrokeLine(cx - FromDIP(4), cy - FromDIP(2), cx, cy + FromDIP(2));
        gc->StrokeLine(cx, cy + FromDIP(2), cx + FromDIP(4), cy - FromDIP(2));
    }
};

class GlassSlider : public wxControl {
public:
    GlassSlider(wxWindow* parent, wxWindowID id = wxID_ANY, int value = 0, int minValue = 0, int maxValue = 100, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxSL_HORIZONTAL)
        : wxControl(parent, id, pos, size, wxNO_BORDER | wxTAB_TRAVERSAL | style), m_val(value), m_min(minValue), m_max(maxValue) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassSlider::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &GlassSlider::OnLeftDown, this);
        Bind(wxEVT_MOTION, &GlassSlider::OnMotion, this);
        Bind(wxEVT_LEFT_UP, &GlassSlider::OnLeftUp, this);
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(false); e.Skip(); });

        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            int code = e.GetKeyCode();
            int step = std::max(1, (m_max - m_min) / 20);
            if (code == WXK_LEFT || code == WXK_DOWN) {
                SetValue(m_val - step);
                wxCommandEvent evt(wxEVT_SLIDER, GetId());
                evt.SetEventObject(this);
                evt.SetInt(m_val);
                ProcessWindowEvent(evt);
            } else if (code == WXK_RIGHT || code == WXK_UP) {
                SetValue(m_val + step);
                wxCommandEvent evt(wxEVT_SLIDER, GetId());
                evt.SetEventObject(this);
                evt.SetInt(m_val);
                ProcessWindowEvent(evt);
            } else {
                e.Skip();
            }
        });
    }

    int GetValue() const { return m_val; }
    void SetValue(int v) {
        int clamped = std::clamp(v, m_min, m_max);
        if (m_val != clamped) {
            m_val = clamped;
            Refresh(false);
        }
    }

    int GetMin() const { return m_min; }
    int GetMax() const { return m_max; }
    void SetRange(int minValue, int maxValue) {
        m_min = minValue;
        m_max = maxValue;
        m_val = std::clamp(m_val, m_min, m_max);
        Refresh(false);
    }

protected:
    wxSize DoGetBestSize() const override {
        return FromDIP(wxSize(140, 28));
    }

private:
    int m_val = 0;
    int m_min = 0;
    int m_max = 100;
    bool m_dragging = false;

    double ValueToPos(int val, double width) const {
        if (m_max <= m_min) return 0;
        double pad = FromDIP(8);
        double avail = width - pad * 2.0;
        double ratio = double(val - m_min) / (m_max - m_min);
        return pad + ratio * avail;
    }

    int PosToValue(double x, double width) const {
        if (m_max <= m_min) return m_min;
        double pad = FromDIP(8);
        double avail = width - pad * 2.0;
        if (avail <= 0) return m_min;
        double ratio = std::clamp((x - pad) / avail, 0.0, 1.0);
        return m_min + int(std::round(ratio * (m_max - m_min)));
    }

    void OnLeftDown(wxMouseEvent& e) {
        if (!IsEnabled()) return;
        SetFocus();
        m_dragging = true;
        CaptureMouse();
        SetValue(PosToValue(e.GetPosition().x, GetClientSize().x));
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetEventObject(this);
        evt.SetInt(m_val);
        ProcessWindowEvent(evt);
    }

    void OnMotion(wxMouseEvent& e) {
        if (!IsEnabled() || !m_dragging) return;
        SetValue(PosToValue(e.GetPosition().x, GetClientSize().x));
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetEventObject(this);
        evt.SetInt(m_val);
        ProcessWindowEvent(evt);
    }

    void OnLeftUp(wxMouseEvent&) {
        if (!m_dragging) return;
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
        wxScrollEvent evt(wxEVT_SCROLL_CHANGED, GetId(), m_val);
        evt.SetEventObject(this);
        ProcessWindowEvent(evt);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Paint real backdrop behind slider
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        const auto& th = CurrentTheme();
        const bool enabled = IsEnabled();
        const double trackH = FromDIP(3);
        const double trackY = (s.y - trackH) / 2.0;
        const double pad = FromDIP(8);
        const double trackW = s.x - pad * 2.0;
        const double thumbX = ValueToPos(m_val, s.x);
        const double thumbR = FromDIP(7); // 14px thumb diameter
        const double thumbY = s.y / 2.0;

        // 2. Base track: 3px rgba(255,255,255,.18)
        wxColour trackBg(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.18));
        gc->SetBrush(wxBrush(trackBg));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(pad, trackY, trackW, trackH, trackH / 2.0);

        // 3. Active track portion: accent fill
        double filledW = thumbX - pad;
        if (filledW > 0) {
            wxColour fillCol = enabled ? th.accent : wxColour(th.accent.Red(), th.accent.Green(), th.accent.Blue(), int(255 * 0.40));
            gc->SetBrush(wxBrush(fillCol));
            gc->DrawRoundedRectangle(pad, trackY, filledW, trackH, trackH / 2.0);
        }

        // 4. Thumb with soft glow
        if (enabled) {
            wxColour glowCol(th.accent.Red(), th.accent.Green(), th.accent.Blue(), 45);
            gc->SetBrush(wxBrush(glowCol));
            gc->DrawEllipse(thumbX - FromDIP(10), thumbY - FromDIP(10), FromDIP(20), FromDIP(20));
        }

        // Solid thumb circle (14px):
        wxColour thumbCol = enabled ? *wxWHITE : wxColour(200, 200, 200, 150);
        gc->SetBrush(wxBrush(thumbCol));
        gc->SetPen(wxPen(th.accent, 1.0));
        gc->DrawEllipse(thumbX - thumbR, thumbY - thumbR, thumbR * 2.0, thumbR * 2.0);

        // Focus ring:
        if (FindFocus() == this && enabled) {
            gc->SetPen(wxPen(th.accent, FromDIP(2)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawEllipse(thumbX - thumbR - 2, thumbY - thumbR - 2, (thumbR + 2) * 2.0, (thumbR + 2) * 2.0);
        }
    }
};

inline bool SmoothButton::MSWOnDraw(WXDRAWITEMSTRUCT* raw) {
    auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(raw);
    auto size = GetClientSize();
    if (size.x <= 0 || size.y <= 0) return true;

    std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::CreateFromNativeHDC((WXHDC)item->hDC));
    if (!g) return false;
    g->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    // 1. Paint real backdrop (including parent glass layers) behind this button!
    PaintBackdrop(g.get(), this, wxRect(0, 0, size.x, size.y));

    bool onDock = false, onSegment = false;
    wxWindow* p = GetParent();
    while (p) {
        if (auto* c = dynamic_cast<Card*>(p)) {
            if (c->isDock) { onDock = true; break; }
            break;
        }
        if (dynamic_cast<SegmentedControl*>(p)) { onSegment = true; break; }
        p = p->GetParent();
    }

    const bool enabled = IsEnabled();
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;
    const double rad = pill ? (size.y / 2.0) : (onDock || onSegment ? FromDIP(9) : FromDIP(12));

    const auto& th = CurrentTheme();
    wxColour ink;

    if (disclosure) {
        if (hover && enabled) {
            g->SetBrush(wxBrush(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * (th.isGlass ? 0.08 : 0.05)))));
            g->SetPen(*wxTRANSPARENT_PEN);
            g->DrawRoundedRectangle(0.5, 0.5, size.x - 1, size.y - 1, FromDIP(8));
        }
        ink = enabled ? th.text : wxColour(th.muted.Red(), th.muted.Green(), th.muted.Blue(), int(255 * 0.40));
    } else if (primary) {
        // Solid Accent button (Connect phone / Disconnect): solid accent #8b93ff, text #0d1030
        wxColour c1 = th.accent;
        if (enabled && hover) c1 = Mix(c1, *wxWHITE, 0.12);
        if (enabled && pressed) c1 = Mix(c1, *wxBLACK, 0.15);
        g->SetBrush(wxBrush(c1));
        g->SetPen(*wxTRANSPARENT_PEN);
        double btnY = (pressed && enabled) ? 1.5 : 0.5;
        g->DrawRoundedRectangle(0.5, btnY, size.x - 1.0, size.y - 1.0, rad);
        ink = enabled ? th.accentInk : wxColour(th.accentInk.Red(), th.accentInk.Green(), th.accentInk.Blue(), int(255 * 0.40));
    } else if (onSegment) {
        // Segmented control pill: Selected: rgba(255,255,255,.18) with 1px inset border rgba(255,255,255,.22). Unselected: transparent, muted text.
        if (selected) {
            wxColour pillFill(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.18));
            g->SetBrush(wxBrush(pillFill));
            wxColour pillBorder(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.22));
            g->SetPen(wxPen(pillBorder, 1.0));
            g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
            ink = enabled ? th.text : wxColour(th.text.Red(), th.text.Green(), th.text.Blue(), int(255 * 0.40));
        } else {
            if (hover && enabled) {
                wxColour hovFill(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.08));
                g->SetBrush(wxBrush(hovFill));
                g->SetPen(*wxTRANSPARENT_PEN);
                g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
            }
            ink = enabled ? (hover ? th.text : th.muted) : wxColour(th.muted.Red(), th.muted.Green(), th.muted.Blue(), int(255 * 0.40));
        }
    } else if (onDock) {
        // Dock button: 38px, transparent until hover. Active states: torch amber rgba(251,191,36,.25) with a #fbbf24 stroke; mirror and flip use accent tint.
        if (selected) {
            if (icon == "flash") {
                g->SetBrush(wxBrush(wxColour(251, 191, 36, int(255 * 0.25))));
                g->SetPen(wxPen(wxColour(251, 191, 36), 1.0));
                ink = wxColour(251, 191, 36);
            } else {
                g->SetBrush(wxBrush(wxColour(th.accent.Red(), th.accent.Green(), th.accent.Blue(), int(255 * 0.25))));
                g->SetPen(wxPen(th.accent, 1.0));
                ink = th.accent;
            }
            g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
        } else {
            if (hover && enabled) {
                g->SetBrush(wxBrush(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.18))));
                g->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.22)), 1.0));
                g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
            }
            ink = enabled ? (hover ? th.text : th.muted) : wxColour(th.muted.Red(), th.muted.Green(), th.muted.Blue(), int(255 * 0.40));
        }
        if (!enabled) {
            ink = wxColour(ink.Red(), ink.Green(), ink.Blue(), int(255 * 0.40));
        }
    } else if (ghost) {
        if (selected) {
            g->SetBrush(wxBrush(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.18))));
            g->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.22)), 1.0));
            g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
            ink = th.text;
        } else if (hover && enabled) {
            g->SetBrush(wxBrush(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.12))));
            g->SetPen(wxPen(wxColour(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.16)), 1.0));
            g->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, rad);
            ink = th.text;
        } else {
            ink = enabled ? th.muted : wxColour(th.muted.Red(), th.muted.Green(), th.muted.Blue(), int(255 * 0.40));
        }
    } else {
        // Standard Glass Button (Save, Import, Export, Delete, Search actions, Advanced settings, Pin, Controls)
        // glass fill rgba(255,255,255,.10), border rgba(255,255,255,.18), hover fill .18, pressed .06 plus 1px inset. NEVER solid white.
        double btnY = (pressed && enabled) ? 1.5 : 0.5;
        double btnH = size.y - 1.0;
        DrawGlass(g.get(), wxRect2DDouble(0.5, btnY, size.x - 1.0, btnH), rad, ELEVATION_BUTTON, hover && enabled, pressed && enabled, false, !enabled);

        ink = enabled ? th.text : wxColour(th.text.Red(), th.text.Green(), th.text.Blue(), int(255 * 0.40));
    }

    // Keyboard focus: 2px accent ring on every interactive control
    if (focused && enabled && !disclosure) {
        g->SetPen(wxPen(th.accent, FromDIP(2)));
        g->SetBrush(*wxTRANSPARENT_BRUSH);
        g->DrawRoundedRectangle(1.5, 1.5, size.x - 3.0, size.y - 3.0, std::max(2.0, rad - 1.0));
    }

    // Render Icon or Text
    if (!icon.empty()) {
        Icon(*g, icon, (size.x - FromDIP(20)) / 2.0, (size.y - FromDIP(20)) / 2.0, FromDIP(20), ink);
    } else {
        auto label = wxControl::RemoveMnemonics(GetLabel());
        g->SetFont(GetFont(), ink);
        double w, h;
        g->GetTextExtent(label, &w, &h);
        double textX = leading ? FromDIP(14) : (size.x - w) / 2.0;
        g->DrawText(label, textX, (size.y - h) / 2.0);

        if (disclosure) {
            if (!valueTag.empty()) {
                g->SetFont(wxFontInfo(FromDIP(9)).FaceName("Segoe UI"), Muted());
                double vw, vh;
                g->GetTextExtent(valueTag, &vw, &vh);
                g->DrawText(valueTag, size.x - FromDIP(36) - vw, (size.y - vh) / 2.0);
            }
            g->SetPen(wxPen(Muted(), FromDIP(1.6)));
            double ax = size.x - FromDIP(18), ay = size.y / 2.0;
            if (expanded) {
                g->StrokeLine(ax - 4, ay + 2, ax, ay - 2);
                g->StrokeLine(ax, ay - 2, ax + 4, ay + 2);
            } else {
                g->StrokeLine(ax - 4, ay - 2, ax, ay + 2);
                g->StrokeLine(ax, ay + 2, ax + 4, ay - 2);
            }
        }
    }
    return true;
}

class Section : public Card {
public:
    wxPanel* body = nullptr;
    SmoothButton* header = nullptr;
    std::function<void()> onToggle;

    Section(wxWindow* parent, const wxString& title, bool open = true) : Card(parent) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        header = new SmoothButton(this, wxID_ANY, title);
        header->ghost = true;
        header->leading = true;
        header->disclosure = true;
        header->expanded = open;
        header->SetMinSize(FromDIP(wxSize(-1, 46)));
        header->SetToolTip("Expand or collapse " + title);
        root->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        body = new wxPanel(this);
        body->SetBackgroundStyle(wxBG_STYLE_PAINT);
        body->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        body->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(body);
            auto s = body->GetClientSize();
            if (s.x <= 0 || s.y <= 0) return;
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            PaintBackdrop(gc.get(), body, wxRect(0, 0, s.x, s.y));
        });
        body->Show(open);
        root->Add(body, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
        SetSizer(root);

        header->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            Toggle();
        });

        UpdateSectionMinHeight();
    }

    void Toggle() {
        SetExpanded(!header->expanded);
    }

    void SetExpanded(bool exp) {
        header->expanded = exp;
        header->Refresh();
        body->Show(exp);
        UpdateSectionMinHeight();
        if (onToggle) onToggle();
    }

    void UpdateSectionMinHeight() {
        int w = GetClientSize().x;
        if (w <= 0 && GetParent()) w = GetParent()->GetClientSize().x - FromDIP(8);
        if (w <= 0) w = FromDIP(302);
        int innerW = std::max(FromDIP(100), w - FromDIP(20));

        int totalH = FromDIP(10) + header->GetMinSize().y;
        if (header->expanded) {
            if (body->GetSizer()) {
                body->SetSize(innerW, -1);
                body->GetSizer()->SetDimension(0, 0, innerW, 10000);
                body->GetSizer()->Layout();
                int bodyH = body->GetSizer()->ComputeFittingClientSize(body).y;
                if (bodyH <= 0) bodyH = body->GetSizer()->GetMinSize().y;
                body->SetMinSize(wxSize(innerW, bodyH));
                body->SetSize(innerW, bodyH);
                totalH += bodyH + FromDIP(14);
            } else {
                totalH += body->GetBestSize().y + FromDIP(14);
            }
        } else {
            totalH += FromDIP(10);
        }

        SetMinSize(wxSize(w, totalH));
        SetSize(w, totalH);
        if (GetSizer()) {
            GetSizer()->SetDimension(0, 0, w, totalH);
            GetSizer()->Layout();
        }
    }

    void SetValue(const wxString& val) {
        if (header->valueTag == val) return;
        header->valueTag = val;
        header->Refresh();
        UpdateSectionMinHeight();
        if (onToggle) onToggle();
    }
};

class GlassInspectorPanel : public wxPanel {
public:
    GlassInspectorPanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize)
        : wxPanel(parent, id, pos, size, wxBORDER_NONE | wxTAB_TRAVERSAL) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, &GlassInspectorPanel::OnPaint, this);

        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            UpdateInspectorLayout();
            e.Skip();
        });

        m_fadeTimer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (m_draggingThumb) return;
            if (m_thumbAlpha > 0.05) {
                m_thumbAlpha -= 0.04;
                InvalidateScrollbarStrip();
            } else {
                m_thumbAlpha = 0.0;
                InvalidateScrollbarStrip();
                m_fadeTimer.Stop();
            }
        });

        Bind(wxEVT_MOUSEWHEEL, &GlassInspectorPanel::OnMouseWheel, this);
        Bind(wxEVT_LEFT_DOWN, &GlassInspectorPanel::OnLeftDown, this);
        Bind(wxEVT_MOTION, &GlassInspectorPanel::OnMotion, this);
        Bind(wxEVT_LEFT_UP, &GlassInspectorPanel::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_draggingThumb = false;
            InvalidateScrollbarStrip();
            if (!m_fadeTimer.IsRunning()) m_fadeTimer.Start(35, wxTIMER_CONTINUOUS);
        });
    }

    void AddCard(wxWindow* card) {
        if (!card) return;
        m_cards.push_back(card);
        ForwardWheel(card);
    }

    void FinalizeCards() {
        for (auto* card : m_cards) {
            ForwardWheel(card);
        }
    }

    void ForwardWheel(wxWindow* win) {
        if (!win || win == this) return;
        win->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
            this->OnMouseWheel(e);
        });
        for (auto* c : win->GetChildren()) {
            ForwardWheel(c);
        }
    }


    void InvalidateScrollbarStrip() {
        auto s = GetClientSize();
        if (s.x > 0 && s.y > 0) {
            int stripW = FromDIP(12);
            wxRect strip(s.x - stripW, 0, stripW, s.y);
            RefreshRect(strip, true);
        }
    }

    // Native layout/movement/repaint flow reused from working VCamdroid Studio
    // 50792433f1643dbd4a2955a4e77c34c753f44567. Keep normal MSW moves;
    // precision wheel, real -1 positions and capture/thumbnail guards are additive.
    void UpdateInspectorLayout() {
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0 || m_inLayout) return;
        m_inLayout = true;
        struct ResetLayoutFlag { bool& flag; ~ResetLayoutFlag() { flag = false; } } reset{m_inLayout};

        int cardGap = FromDIP(12);
        int cardWidth = CardWidth();

        // a. sizer->Layout() and recompute total content height (sum of card heights + gaps + bottom padding equal to the card gap)
        if (GetSizer()) {
            GetSizer()->SetDimension(0, 0, cardWidth, 100000);
            GetSizer()->Layout();
        }

        m_unscrolledY.clear();
        m_cardHeights.clear();
        int currentY = 0;

        for (size_t i = 0; i < m_cards.size(); ++i) {
            auto* card = m_cards[i];
            if (!card || !card->IsShown()) continue;

            if (auto* sec = dynamic_cast<Section*>(card)) {
                sec->UpdateSectionMinHeight();
            } else if (card->GetSizer()) {
                card->GetSizer()->SetDimension(0, 0, cardWidth, 10000);
                card->GetSizer()->Layout();
            }

            int h = 0;
            if (auto* sec = dynamic_cast<Section*>(card)) {
                h = sec->GetMinSize().y;
                if (h <= 0) h = sec->GetBestSize().y;
            } else if (card->GetSizer()) {
                h = card->GetSizer()->ComputeFittingClientSize(card).y;
                if (h <= 0) h = card->GetBestSize().y;
            } else {
                h = card->GetBestSize().y;
            }
            if (h <= 0) h = card->GetMinSize().y;

            m_unscrolledY.push_back(currentY);
            m_cardHeights.push_back(h);
            currentY += h + cardGap;
        }

        m_contentHeight = currentY;

        // b. clamp scrollOffset to [0, max(0, contentHeight - clientHeight)]
        int maxScroll = std::max(0, m_contentHeight - s.y);
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

        // c. reposition children with the clamped offset
        size_t activeIdx = 0;
        for (size_t i = 0; i < m_cards.size(); ++i) {
            auto* card = m_cards[i];
            if (!card || !card->IsShown()) continue;
            if (activeIdx < m_unscrolledY.size() && activeIdx < m_cardHeights.size()) {
                int y = m_unscrolledY[activeIdx] - m_scrollOffset;
                int h = m_cardHeights[activeIdx];
                card->SetSize(0, y, cardWidth, h, wxSIZE_ALLOW_MINUS_ONE);
                card->Layout();
            }
            activeIdx++;
        }

        // d. Repaint the inspector and its backdrop without invalidating the live preview.
        RepaintViewport();
    }

    void ScrollTo(int newOffset) {
        auto s = GetClientSize();
        int maxScroll = std::max(0, m_contentHeight - s.y);
        int clamped = std::clamp(newOffset, 0, maxScroll);
        if (clamped != m_scrollOffset) {
            m_scrollOffset = clamped;
            m_thumbAlpha = 0.40;

            int cardWidth = CardWidth();

            size_t activeIdx = 0;
            for (size_t i = 0; i < m_cards.size(); ++i) {
                auto* card = m_cards[i];
                if (!card || !card->IsShown()) continue;
                if (activeIdx < m_unscrolledY.size() && activeIdx < m_cardHeights.size()) {
                    int y = m_unscrolledY[activeIdx] - m_scrollOffset;
                    int h = m_cardHeights[activeIdx];
                    card->SetSize(0, y, cardWidth, h, wxSIZE_ALLOW_MINUS_ONE);
                }
                activeIdx++;
            }

            RepaintViewport();
            if (!m_fadeTimer.IsRunning()) m_fadeTimer.Start(35, wxTIMER_CONTINUOUS);
        }
    }

    int GetScrollOffset() const { return m_scrollOffset; }
    int GetContentHeight() const { return m_contentHeight; }

private:
    std::vector<wxWindow*> m_cards;
    std::vector<int> m_cardHeights;
    std::vector<int> m_unscrolledY;
    int m_scrollOffset = 0;
    bool m_inLayout = false;
    double m_wheelRemainder = 0.0;
    int m_contentHeight = 0;
    double m_thumbAlpha = 0.0;
    wxTimer m_fadeTimer;
    bool m_draggingThumb = false;
    int m_dragStartY = 0;
    int m_dragStartScroll = 0;

    void RepaintViewport() {
        // A live preview keeps posting frames. Complete the moved native child
        // paints in this viewport before returning from a scroll/layout event,
        // instead of invalidating the entire shell and its 60 Hz canvas.
        // The cached ambient backdrop is independent of the camera image.
        if (GetParent()) GetParent()->RefreshRect(GetRect(), false);
        Refresh(true);
#ifdef __WXMSW__
        ::RedrawWindow(static_cast<HWND>(GetHandle()), nullptr, nullptr,
            RDW_UPDATENOW | RDW_ALLCHILDREN);
#endif
    }

    int CardWidth() const {
        const int available = GetClientSize().x;
        const int width = available - FromDIP(8);
        return width < FromDIP(100) ? available : width;
    }

    struct ThumbMetrics { double height = 0, track = 0, y = 0; int maxScroll = 0; };
    ThumbMetrics GetThumbMetrics() const {
        ThumbMetrics result;
        const int height = std::max(0, GetClientSize().y);
        result.maxScroll = std::max(0, m_contentHeight - height);
        if (height == 0 || m_contentHeight <= 0 || result.maxScroll == 0) return result;
        result.height = std::min(double(height), std::max(double(FromDIP(28)), double(height) * height / m_contentHeight));
        result.track = std::max(0.0, height - result.height);
        result.y = double(m_scrollOffset) / result.maxScroll * result.track;
        return result;
    }

    void OnMouseWheel(wxMouseEvent& e) {
        if (e.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) { e.Skip(); return; }
        if (e.GetWheelRotation() == 0) return;
        const int delta = e.GetWheelDelta() > 0 ? e.GetWheelDelta() : 120;
        const double limit = std::max(1, m_contentHeight);
        const double pixels = std::clamp(m_wheelRemainder + double(FromDIP(44)) * e.GetWheelRotation() / delta, -limit, limit);
        const int step = int(std::round(pixels));
        m_wheelRemainder = pixels - step;
        ScrollTo(m_scrollOffset - step);
    }

    void OnLeftDown(wxMouseEvent& e) {
        auto s = GetClientSize();
        int stripX = s.x - FromDIP(12);
        if (e.GetX() >= stripX && m_contentHeight > s.y) {
            const auto thumb = GetThumbMetrics();
            const double thH = thumb.height;
            const double ty = thumb.y;

            if (e.GetY() >= ty && e.GetY() <= ty + thH) {
                m_draggingThumb = true;
                m_dragStartY = e.GetY();
                m_dragStartScroll = m_scrollOffset;
                CaptureMouse();
            } else if (e.GetY() < ty) {
                ScrollTo(m_scrollOffset - s.y);
            } else {
                ScrollTo(m_scrollOffset + s.y);
            }
            m_thumbAlpha = 0.50;
            InvalidateScrollbarStrip();
        } else {
            e.Skip();
        }
    }

    void OnMotion(wxMouseEvent& e) {
        if (m_draggingThumb && HasCapture()) {
            const auto thumb = GetThumbMetrics();
            const double maxScroll = thumb.maxScroll;
            const double trackH = thumb.track;
            if (trackH > 0) {
                int deltaY = e.GetY() - m_dragStartY;
                int newOffset = m_dragStartScroll + int(double(deltaY) / trackH * maxScroll);
                ScrollTo(newOffset);
            }
        }
    }

    void OnLeftUp(wxMouseEvent& e) {
        if (m_draggingThumb) {
            m_draggingThumb = false;
            if (HasCapture()) ReleaseMouse();
            if (!m_fadeTimer.IsRunning()) m_fadeTimer.Start(35, wxTIMER_CONTINUOUS);
        }
        e.Skip();
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        auto s = GetClientSize();
        if (s.x <= 0 || s.y <= 0) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 1. Paint real backdrop behind this inspector panel
        PaintBackdrop(gc.get(), this, wxRect(0, 0, s.x, s.y));

        // 2. Draw overlay scrollbar thumb (fades when idle)
        if (m_contentHeight > s.y && s.y > 0 && m_thumbAlpha > 0.01) {
            const auto thumb = GetThumbMetrics();
            const double thH = thumb.height;
            const double ty = thumb.y;
            double tx = s.x - FromDIP(6);
            double tw = FromDIP(4);
            gc->SetBrush(wxBrush(wxColour(255, 255, 255, int(std::clamp(m_thumbAlpha * 255.0, 0.0, 255.0)))));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(tx, ty, tw, thH, FromDIP(2));
        }
    }
};

inline void SyncLabels(wxWindow* w) {
    if (auto* st = dynamic_cast<wxStaticText*>(w)) {
        // Make any remaining wxStaticText transparent so it doesn't paint an
        // opaque GDI box over the glass backdrop.
        st->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
        auto* role = dynamic_cast<Role*>(st->GetClientObject());
        st->SetForegroundColour(role && role->muted ? Muted() : Text());
        st->Refresh(false);
    }
    if (auto* ch = dynamic_cast<wxChoice*>(w)) {
        // Any leftover native wxChoice should at least use the theme colours.
        ch->SetBackgroundColour(Dark() ? wxColour(15, 20, 36) : *wxWHITE);
        ch->SetForegroundColour(Text());
    }
    if (auto* sl = dynamic_cast<wxSlider*>(w)) {
        sl->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
        sl->Refresh(false);
    }
    for (auto* c : w->GetChildren()) SyncLabels(c);
}

inline void RefreshAll(wxWindow* w) {
    if (!w) return;
    w->Refresh(false);
    for (auto* c : w->GetChildren()) RefreshAll(c);
}

inline void Dialog(wxWindow* w) {
    if (!w) return;
    SyncLabels(w);
    w->Refresh(false);
}

}
