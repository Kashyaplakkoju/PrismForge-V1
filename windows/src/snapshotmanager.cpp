// Allow the use of unsafe marked functions 
// like fopen, strcpy, etc (C standard functions)
// used by wxWidgets internally (e.g. wxStandardPaths).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "snapshotmanager.h"
#include "settings.h"
#include "logger.h"

#include <thread>
#include <wx/app.h>
#include <wx/file.h>
#include <wx/wfstream.h>
#include <wx/utils.h>
#include <wx/image.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

SnapshotManager::SnapshotManager() : scaler(AV_PIX_FMT_RGB24), snapshotRequested(false)
{
}

void SnapshotManager::RequestSnapshot()
{
	snapshotRequested = true;
}

SnapshotManager::~SnapshotManager() { Wait(); }
void SnapshotManager::Wait() { if (saving.valid()) saving.wait(); }

void SnapshotManager::ProcessFrame(AVFrame* frame)
{
    if (!snapshotRequested.load()) return;
    if (saving.valid() && saving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (!snapshotRequested.exchange(false)) return;
    int width = 0, height = 0;
    const auto* frameData = scaler.Process(frame, width, height);
    if (!frameData || width <= 0 || height <= 0) return;
    std::vector<uint8_t> copy(frameData, frameData + size_t(width) * height * 3);
    auto callback = onSaved;
    saving = std::async(std::launch::async, [data = std::move(copy), width, height, callback] {
        bool ok = false;
        try { ok = SaveSnapshot(data, width, height); } catch (...) {}
        if (callback && wxTheApp) wxTheApp->CallAfter([callback, ok] { callback(ok); });
    });
}

bool SnapshotManager::SaveSnapshot(const std::vector<uint8_t>& data, int width, int height)
{
    wxImage image(width, height, const_cast<uint8_t*>(data.data()), true);
    wxFileName path = Settings::GetDocumentsDirectoryPath();
    path.AppendDir("Snapshots");
    if (!path.DirExists() && !path.Mkdir(0755, wxPATH_MKDIR_FULL)) return false;
    static std::atomic<unsigned> sequence{0};
    path.SetFullName(wxString::Format("Snapshot_%s_%lu_%u.jpg", wxDateTime::Now().Format("%Y%m%d_%H%M%S"), wxGetProcessId(), ++sequence));
    wxFile file;
    if (!file.Create(path.GetFullPath(), false)) return false;
    bool ok;
    { wxFileOutputStream output(file); ok = image.SaveFile(output, wxBITMAP_TYPE_JPEG) && output.IsOk(); }
    file.Close();
    if (!ok) wxRemoveFile(path.GetFullPath());
    return ok;
}
