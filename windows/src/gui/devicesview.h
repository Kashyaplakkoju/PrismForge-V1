#pragma once

#include <wx/wx.h>
#include <vector>
#include "net/devicedescriptor.h"
#include "gui/studio.h"

class DevicesView : public Studio::GlassDialogBase
{
public:
    DevicesView(wxWindow* parent, const std::vector<DeviceDescriptor>& devices);
};