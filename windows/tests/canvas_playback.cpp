// Replay a captured decoded PNG through the actual Canvas/scaler without camera services or sockets.
#include "canvas.h"
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

class CanvasPlayback : public wxApp
{
    bool OnInit() override
    {
        wxInitAllImageHandlers();
        const wxString path = argc == 2 ? wxString(argv[1]) :
            wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath() + "/fixed-usb-back-off-3-decoded.png";
        wxImage image(path);
        if (!image.IsOk()) return false;
        auto* window = new wxFrame(nullptr, wxID_ANY, "PrismForge captured frame - Windows Canvas playback");
        window->SetClientSize(800, 940);
        auto* canvas = new Canvas(window, wxDefaultPosition, wxSize(800, 940));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(canvas, 1, wxEXPAND);
        window->SetSizer(sizer);
        window->Show();
        window->Hide();
        window->Show();
        SetTopWindow(window);
        AVFrame* frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_RGB24;
        frame->width = image.GetWidth();
        frame->height = image.GetHeight();
        frame->data[0] = image.GetData();
        frame->linesize[0] = frame->width * 3;
        canvas->ProcessRawFrameAsync(frame);
        frame->data[0] = nullptr; // wxImage owns the input; scaler has already copied it.
        av_frame_free(&frame);
        return true;
    }
};

wxIMPLEMENT_APP(CanvasPlayback);
