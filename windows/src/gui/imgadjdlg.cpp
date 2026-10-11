#include "gui/imgadjdlg.h"
#include <wx/statline.h>
#include "gui/studio.h"

wxDEFINE_EVENT(EVT_FILTER_PARAM_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_FILTER_SWITCH_CHANGED, wxCommandEvent);

ImgAdjDlg::ImgAdjDlg(wxWindow* parent,
    const DeviceDescriptor& deviceDesc,
    const std::map<std::string, int>& initialSliderValues,
    std::string initialActiveFilter)
    : Studio::GlassDialogBase(parent, wxID_ANY, "Image Adjustments & Filters", wxDefaultPosition, wxSize(460, 480), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetSize(FromDIP(wxSize(520, 520)));
    SetMinSize(FromDIP(wxSize(420, 380)));

    auto mainSizer = new wxBoxSizer(wxVERTICAL);

    auto notebook = new wxNotebook(this, wxID_ANY);

    // ---------------------------------------------------------
    // TAB 1: COLOR & EXPOSURE CORRECTIONS
    // ---------------------------------------------------------
    auto correctionsPanel = new wxScrolledWindow(notebook, wxID_ANY);
    correctionsPanel->SetScrollRate(5, 5);

    auto correctionsBoxSizer = new wxBoxSizer(wxVERTICAL);

    // 4 Columns: [Parameter Name] [Slider] [Value %] [Reset Btn]
    auto adjustmentsGrid = new wxFlexGridSizer(4, 8, 8);
    adjustmentsGrid->AddGrowableCol(1, 1);

    bool hasSliders = false;

    if (deviceDesc.filters().count(Video::Filter::Category::CORRECTION))
    {
        const auto& filterList = deviceDesc.filters().at(Video::Filter::Category::CORRECTION);
        for (const auto& filterName : filterList)
        {
            int startValue = 50;
            if (initialSliderValues.count(filterName))
            {
                startValue = initialSliderValues.at(filterName);
            }

            AddSlider(correctionsPanel, adjustmentsGrid, filterName, startValue);
            hasSliders = true;
        }
    }

    correctionsBoxSizer->Add(adjustmentsGrid, 1, wxEXPAND | wxALL, 12);
    correctionsPanel->SetSizer(correctionsBoxSizer);
    correctionsPanel->FitInside();

    notebook->AddPage(correctionsPanel, "Color & Exposure", true);

    // ---------------------------------------------------------
    // TAB 2: CREATIVE FILTERS & EFFECTS
    // ---------------------------------------------------------
    auto filtersPanel = new wxPanel(notebook, wxID_ANY);
    auto filtersBoxSizer = new wxBoxSizer(wxVERTICAL);

    auto filtersStaticBox = new wxStaticBoxSizer(wxVERTICAL, filtersPanel, "Active Effect (Only one active at a time)");
    auto filtersGrid = new wxFlexGridSizer(2, 10, 12);
    filtersGrid->AddGrowableCol(1, 1);

    bool hasDropdowns = false;

    for (const auto& [category, filterList] : deviceDesc.filters())
    {
        if (category == Video::Filter::Category::NONE || category == Video::Filter::Category::CORRECTION)
            continue;

        std::string catName;
        switch (category) {
        case Video::Filter::Category::DISTORTION: catName = "Distortion"; break;
        case Video::Filter::Category::ARTISTIC: catName = "Artistic"; break;
        case Video::Filter::Category::EFFECT: catName = "Classic Effects"; break;
        default: catName = "Other"; break;
        }

        auto label = new wxStaticText(filtersPanel, wxID_ANY, catName + ":");
        wxFont boldFont = label->GetFont();
        boldFont.SetWeight(wxFONTWEIGHT_MEDIUM);
        label->SetFont(boldFont);

        wxArrayString choices;
        choices.Add("None");

        int selectionIndex = 0;
        for (size_t i = 0; i < filterList.size(); i++)
        {
            const auto& fName = filterList[i];
            choices.Add(fName);
            if (fName == initialActiveFilter)
            {
                selectionIndex = (int)i + 1;
            }
        }

        auto choice = new wxChoice(filtersPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
        choice->SetSelection(selectionIndex);

        choice->Bind(wxEVT_CHOICE, [this, category, choice](wxCommandEvent& e) {
            if (updatingChoices) return;

            auto selectedStr = choice->GetStringSelection();

            // Enforce single active effect across all categories
            if (selectedStr != "None")
            {
                updatingChoices = true;
                for (auto& [cat, otherChoice] : categoryChoices)
                {
                    if (cat != static_cast<int>(category))
                    {
                        otherChoice->SetSelection(0);
                    }
                }
                updatingChoices = false;
            }

            wxCommandEvent evt(EVT_FILTER_SWITCH_CHANGED);
            evt.SetEventObject(this);
            evt.SetInt(static_cast<int>(category));
            evt.SetString(selectedStr);
            ProcessWindowEvent(evt);
        });

        categoryChoices[static_cast<int>(category)] = choice;

        filtersGrid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        filtersGrid->Add(choice, 1, wxEXPAND);
        hasDropdowns = true;
    }

    filtersStaticBox->Add(filtersGrid, 1, wxEXPAND | wxALL, 10);
    filtersBoxSizer->Add(filtersStaticBox, 0, wxEXPAND | wxALL, 12);
    filtersPanel->SetSizer(filtersBoxSizer);

    notebook->AddPage(filtersPanel, "Creative Filters", false);

    mainSizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

    // ---------------------------------------------------------
    // BOTTOM BAR: RESET & CLOSE BUTTONS
    // ---------------------------------------------------------
    auto btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto resetBtn = Studio::Button(this, "Reset All to Default", "Reset adjustments to default", false);
    resetBtn->Bind(wxEVT_BUTTON, &ImgAdjDlg::OnReset, this);
    auto okBtn = Studio::Button(this, "Close", "Close adjustments", true, wxID_OK);
    okBtn->SetDefault();

    btnSizer->Add(resetBtn, 0, wxRIGHT, FromDIP(8));
    btnSizer->AddStretchSpacer(1);
    btnSizer->Add(okBtn, 0);

    mainSizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));

    SetSizer(mainSizer);
    Layout();
    CenterOnParent();
}

ImgAdjDlg::~ImgAdjDlg()
{
}

void ImgAdjDlg::AddSlider(wxWindow* parent, wxFlexGridSizer* sizer, const std::string& label, int initialValue)
{
    // Label with fixed minimum width for neat alignment
    auto text = new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxSize(95, -1));

    // Slider (0-100)
    auto slider = new wxSlider(parent, wxID_ANY, initialValue, 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);

    // Percentage Label with fixed width
    auto valLabel = new wxStaticText(parent, wxID_ANY, wxString::Format("%3d%%", initialValue), wxDefaultPosition, wxSize(42, -1), wxALIGN_RIGHT);

    // Quick reset button for single slider
    auto resetSingle = new wxButton(parent, wxID_ANY, "â†º", wxDefaultPosition, wxSize(26, 24));
    resetSingle->SetToolTip("Reset " + label + " to 50%");

    // Store references
    sliders[label] = slider;
    sliderLabels[label] = valLabel;

    // Slider Event
    slider->Bind(wxEVT_SLIDER, [this, label, slider, valLabel](wxCommandEvent& e) {
        int val = slider->GetValue();
        valLabel->SetLabel(wxString::Format("%3d%%", val));

        wxCommandEvent evt(EVT_FILTER_PARAM_CHANGED);
        evt.SetEventObject(this);
        evt.SetString(label);
        evt.SetInt(val);
        ProcessWindowEvent(evt);
    });

    // Single Reset Event
    resetSingle->Bind(wxEVT_BUTTON, [this, label, slider, valLabel](wxCommandEvent& e) {
        slider->SetValue(50);
        valLabel->SetLabel(" 50%");

        wxCommandEvent evt(EVT_FILTER_PARAM_CHANGED);
        evt.SetEventObject(this);
        evt.SetString(label);
        evt.SetInt(50);
        ProcessWindowEvent(evt);
    });

    sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    sizer->Add(valLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    sizer->Add(resetSingle, 0, wxALIGN_CENTER_VERTICAL);
}

void ImgAdjDlg::OnReset(wxCommandEvent& event)
{
    // 1. Reset Sliders to 50%
    for (auto& [name, slider] : sliders) {
        slider->SetValue(50);
        sliderLabels[name]->SetLabel(" 50%");

        wxCommandEvent evt(EVT_FILTER_PARAM_CHANGED);
        evt.SetString(name);
        evt.SetInt(50);
        ProcessWindowEvent(evt);
    }

    // 2. Reset Dropdowns to "None"
    updatingChoices = true;
    for (auto& [cat, choice] : categoryChoices) {
        if (choice->GetCount() > 0) {
            choice->SetSelection(0);
        }
    }
    updatingChoices = false;

    // Send single "None" effect event
    wxCommandEvent evt(EVT_FILTER_SWITCH_CHANGED);
    evt.SetInt(0);
    evt.SetString("None");
    ProcessWindowEvent(evt);
}