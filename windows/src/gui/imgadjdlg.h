#pragma once

#include <wx/wx.h>
#include <wx/notebook.h>
#include <map>
#include <vector>
#include <string>
#include "net/devicedescriptor.h"

#include "gui/studio.h"

// Events
wxDECLARE_EVENT(EVT_FILTER_PARAM_CHANGED, wxCommandEvent); // Int value
wxDECLARE_EVENT(EVT_FILTER_SWITCH_CHANGED, wxCommandEvent); // String name

class ImgAdjDlg : public Studio::GlassDialogBase
{
public:
	ImgAdjDlg(wxWindow* parent, const DeviceDescriptor& deviceDesc, const std::map<std::string, int>& filterValueCache, std::string selectedFilterCache);
	~ImgAdjDlg();

private:
	void AddSlider(wxWindow* parent, wxFlexGridSizer* sizer, const std::string& label, int initialValue);
	void OnReset(wxCommandEvent& event);

	std::map<std::string, wxSlider*> sliders;
	std::map<std::string, wxStaticText*> sliderLabels;
	std::map<int, wxChoice*> categoryChoices;
	bool updatingChoices{ false };
};