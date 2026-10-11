#include "canvas.h"

#include <wx/dcbuffer.h>
#include <wx/rawbmp.h>
#include <wx/graphics.h>
#include "gui/studio.h"

Canvas::Canvas(wxWindow* parent, wxPoint position, wxSize size)
	: wxPanel(parent, wxID_ANY, position, size),
	scaler(1280, 720),
	size(size),
	bitmap(size.x, size.y)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetDoubleBuffered(true);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });

	drawX = 0;
	drawY = 0;

	this->Bind(wxEVT_PAINT, &Canvas::OnPaint, this);
	this->Bind(wxEVT_ERASE_BACKGROUND, &Canvas::OnEraseBackground, this);
}

void Canvas::Render(const uint8_t* bytes, int width, int height)
{
	if (!bytes || width <= 0 || height <= 0)
		return;

	if (!bitmap.IsOk() || bitmap.GetWidth() != width || bitmap.GetHeight() != height)
	{
		drawX = (WIDTH - width) / 2;
		drawY = (HEIGHT - height) / 2;
		bitmap.Create(width, height, 24);
		shouldClear = true;
	}

	wxNativePixelData data(bitmap);
	if (!data)
		return;

	wxNativePixelData::Iterator p(data);
	int stride = width * 3;

	for (int y = 0; y < height; y++)
	{
		uint8_t* dstrow = (uint8_t*)p.m_ptr;
		std::memcpy(dstrow, bytes + (y * stride), stride);

		p.OffsetY(data, 1);
	}

	shouldDraw = true;
	this->Refresh(false);

}

void Canvas::ProcessRawFrameAsync(const AVFrame* frame)
{
    if (isRendering.exchange(true)) return;
    const auto frameGeneration = generation.load();

	int width, height;
	const uint8_t* data = scaler.Process(frame, width, height);

	// If scaler failed (nullptr) or returned invalid dimensions, abort immediately.
    // Do NOT try to construct the vector below, or it will segfault.
	if (!data || width <= 0 || height <= 0)
	{
		isRendering = false;
		return;
	}

	size_t size = width * height * 3;
	std::vector<uint8_t> safeFrame;
	try {
		safeFrame.assign(data, data + size);
	}
	catch (const std::exception&) {
		// Allocation failed (out of memory?)
		isRendering = false;
		return;
	}

	this->CallAfter([this, bytes = std::move(safeFrame), width, height, frameGeneration]() {
        if (frameGeneration == generation.load()) this->Render(bytes.data(), width, height);
		this->isRendering = false;
	});
}

void Canvas::ClearBeforeNextRender()
{
	shouldClear = true;
}

void Canvas::Clear()
{
	shouldDraw = false;
    ++generation;
	Refresh(false);

}

void Canvas::SetEmptyState(const wxString& title, const wxString& detail)
{
    emptyTitle = title; emptyDetail = detail; Refresh(false);
}

void Canvas::SetHudStats(const wxString& res, const wxString& fps)
{
    hudRes = res;
    hudFps = fps;
    showHud = !res.empty() || !fps.empty();
    Refresh(false);
}

void Canvas::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    auto client = GetClientSize();
    if (client.x <= 0 || client.y <= 0) return;
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    // 1. Paint real backdrop behind canvas
    Studio::PaintBackdrop(gc.get(), this, wxRect(0, 0, client.x, client.y));

    const auto& th = Studio::CurrentTheme();

    if (shouldDraw && bitmap.IsOk()) {
        const double scale = std::min(double(client.x) / bitmap.GetWidth(), double(client.y) / bitmap.GetHeight());
        const double w = bitmap.GetWidth() * scale, h = bitmap.GetHeight() * scale;
        const double bx = (client.x - w) / 2.0, by = (client.y - h) / 2.0;

        // Clip video to 16px rounded rect with 1px border rgba(255,255,255,.14)
        const double rad = FromDIP(16);
        HRGN hrgn = CreateRoundRectRgn((int)std::round(bx), (int)std::round(by), (int)std::round(bx + w) + 1, (int)std::round(by + h) + 1, (int)std::round(rad * 2), (int)std::round(rad * 2));
        wxRegion clipRegion(hrgn);
        gc->PushState();
        gc->Clip(clipRegion);
        gc->DrawBitmap(bitmap, bx, by, w, h);
        gc->PopState();

        wxColour borderCol(th.glassRgb.Red(), th.glassRgb.Green(), th.glassRgb.Blue(), int(255 * 0.14));
        gc->SetPen(wxPen(borderCol, 1.0));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(bx + 0.5, by + 0.5, w - 1.0, h - 1.0, rad);

        // Draw translucent HUD badge in top-left corner of the video feed
        if (showHud) {
            double hx = bx + FromDIP(14), hy = by + FromDIP(14);
            gc->SetFont(wxFontInfo(FromDIP(9)).FaceName("Segoe UI").Bold(), *wxWHITE);

            if (!hudRes.empty()) {
                double rw, rh;
                gc->GetTextExtent(hudRes, &rw, &rh);
                gc->SetBrush(wxBrush(wxColour(0, 0, 0, 175)));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawRoundedRectangle(hx, hy, rw + FromDIP(16), rh + FromDIP(8), FromDIP(99));
                gc->DrawText(hudRes, hx + FromDIP(8), hy + FromDIP(4));
                hx += rw + FromDIP(22);
            }
            if (!hudFps.empty()) {
                double fw, fh;
                gc->GetTextExtent(hudFps, &fw, &fh);
                gc->SetBrush(wxBrush(wxColour(0, 0, 0, 175)));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawRoundedRectangle(hx, hy, fw + FromDIP(16), fh + FromDIP(8), FromDIP(99));
                gc->DrawText(hudFps, hx + FromDIP(8), hy + FromDIP(4));
            }
        }
    } else {
        const int cx = client.x / 2, cy = (client.y - FromDIP(45)) / 2;
        if (client.y > FromDIP(170)) {
            const double boxSize = FromDIP(64);
            const double bx = cx - boxSize / 2.0, by = cy - FromDIP(85);
            Studio::DrawGlass(gc.get(), wxRect2DDouble(bx, by, boxSize, boxSize), FromDIP(18), Studio::ELEVATION_BUTTON);
            Studio::Icon(*gc, "camera", cx - FromDIP(14), by + (boxSize - FromDIP(28)) / 2.0, FromDIP(28), Studio::Accent());
        }

        gc->SetFont(wxFontInfo(client.x < FromDIP(400) ? 14 : 19).FaceName("Segoe UI").Bold(), Studio::Text());
        double tw, th_ext;
        gc->GetTextExtent(emptyTitle, &tw, &th_ext);
        gc->DrawText(emptyTitle, (client.x - tw) / 2.0, cy - FromDIP(18));

        if (client.y > FromDIP(140)) {
            gc->SetFont(wxFontInfo(10).FaceName("Segoe UI"), Studio::Muted());
            wxArrayString lines = wxSplit(emptyDetail, '\n');
            double curY = cy + FromDIP(20);
            for (const auto& line : lines) {
                double lw, lh;
                gc->GetTextExtent(line, &lw, &lh);
                gc->DrawText(line, (client.x - lw) / 2.0, curY);
                curY += lh + FromDIP(4);
            }
        }
    }
}

void Canvas::OnEraseBackground(wxEraseEvent& event)
{
	// Empty to remove flickering created by repainting the background
	// before actually rendering the video frame
}
