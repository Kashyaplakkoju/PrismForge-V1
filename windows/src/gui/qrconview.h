#pragma once

#include <wx/wx.h>
#include "canvas.h"
#include "gui/studio.h"
#include "qrcodegen.hpp"

class QrconView : public Studio::GlassDialogBase
{
public:
	QrconView(std::string name, std::string address, std::string port, wxSize displaySize = wxSize(240, 240), wxWindow* parent = nullptr);

    int ShowModal() override {
        Layout();
        return Studio::GlassDialogBase::ShowModal();
    }

private:
	qrcodegen::QrCode GenerateQRCode(std::string data);
	wxImage GenerateImageFromQR(const qrcodegen::QrCode& qrcode);
};