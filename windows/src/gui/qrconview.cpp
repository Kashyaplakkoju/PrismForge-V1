#include "gui/qrconview.h"
#include "gui/studio.h"
#include <wx/clipbrd.h>

QrconView::QrconView(std::string name, std::string address, std::string port, wxSize displaySize, wxWindow* parent)
    : Studio::GlassDialogBase(parent ? parent : wxTheApp->GetTopWindow(), wxID_ANY, "Connect your phone", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    int wrapWidth = FromDIP(380);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // 1. Heading: 24px semibold, primary text colour, no background box
    auto* heading = Studio::Label(this, "A better camera. One connection.");
    heading->SetFont(wxFontInfo(FromDIP(wxSize(0, 24))).FaceName("Segoe UI").Weight(wxFONTWEIGHT_SEMIBOLD));
    heading->SetMuted(false);
    sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(24));

    // 2. Paragraph 1: body text muted colour, wrapped to available width
    auto* p1 = Studio::Label(this, "Open PrismForge on your phone and scan this code.\nKeep both devices on the same trusted Wi-Fi network.", 10, true);
    p1->Wrap(wrapWidth);
    sizer->Add(p1, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    // 3. QR code: crisp black on white on white rounded card (radius 16, quiet zone >= 4, ~240px)
    auto image = GenerateImageFromQR(GenerateQRCode(address + ":" + port));
    auto* qrCard = new Studio::QRCodeCard(this, image, 240);
    qrCard->SetName("Scan this pairing QR code with PrismForge on your phone");
    sizer->Add(qrCard, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(18));

    // 4. Address line: glass input-style chip with selectable text
    auto endpoint = wxString::FromUTF8(address + ":" + port);
    auto chipText = wxString::FromUTF8(name) + "  /  " + endpoint;
    auto* addressChip = new Studio::GlassAddressChip(this, chipText);
    sizer->Add(addressChip, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(14));

    // 5. "Copy connection address": secondary glass button
    auto* copy = Studio::Button(this, "Copy connection address", "Copy the local address for Enter address on your phone.", false);
    copy->Bind(wxEVT_BUTTON, [copy, endpoint](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            bool ok = wxTheClipboard->SetData(new wxTextDataObject(endpoint));
            wxTheClipboard->Close();
            copy->SetLabel(ok ? "Address copied" : "Could not copy address");
        }
    });
    sizer->Add(copy, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(18));

    // 6. Paragraph 2: body text muted colour, wrapped to available width
    auto* p2 = Studio::Label(this, "Prefer a cable? Enable USB debugging on your phone,\nconnect a USB cable, then tap USB cable in the phone app.", 10, true);
    p2->Wrap(wrapWidth);
    sizer->Add(p2, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(24));

    // 7. "Done": solid accent primary button
    auto* done = Studio::Button(this, "Done", "Close connection help", true, wxID_OK);
    done->SetDefault();
    done->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    sizer->Add(done, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(24));

    SetSizerAndFit(sizer);
    SetMinSize(GetSize());
    CenterOnParent();
}
qrcodegen::QrCode QrconView::GenerateQRCode(std::string data)
{
    return qrcodegen::QrCode::encodeText(data.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
}
wxImage QrconView::GenerateImageFromQR(const qrcodegen::QrCode& qrcode)
{
    const int border = 4, size = qrcode.getSize() + border * 2;
    wxImage image(size, size); auto* bytes = image.GetData();
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        auto value = qrcode.getModule(x-border, y-border) ? 0 : 255;
        for (int c = 0; c < 3; ++c) bytes[(y*size+x)*3+c] = (unsigned char)value;
    }
    return image;
}
