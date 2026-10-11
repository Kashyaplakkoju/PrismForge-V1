#pragma once

#include <wx/wx.h>
#include "video/guipreviewscaler.h"

class Canvas : public wxPanel
{
public:
	static const int WIDTH = 400;
	static const int HEIGHT = 300;

	Canvas(wxWindow* parent, wxPoint position, wxSize size);

	void ProcessRawFrameAsync(const AVFrame* frame);
	void ClearBeforeNextRender();
	void Clear();
    void SetEmptyState(const wxString& title, const wxString& detail);
    void SetHudStats(const wxString& res, const wxString& fps);
private:
    wxString hudRes = "1080p";
    wxString hudFps = "30 fps";
    bool showHud = false;
	wxString emptyTitle = "Turn your phone into a webcam";
    wxString emptyDetail = "Plug in your Android phone with USB, or pair over Wi-Fi.\nChoose Connect phone to get started.";
    std::atomic<unsigned> generation{0};
    wxSize size;
	wxBitmap bitmap;

	bool shouldDraw = false;
	bool shouldClear = false;
	int drawX, drawY;

	std::atomic<bool> isRendering{ false };

	Video::GuiPreviewScaler scaler;

	void Render(const uint8_t* bytes, int width, int height);

	void OnPaint(wxPaintEvent& event);
	void OnEraseBackground(wxEraseEvent& event);
};