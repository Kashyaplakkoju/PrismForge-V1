#pragma once

#include <wx/iconbndl.h>
#include <wx/settings.h>
#include <wx/msw/wrapwin.h>
#include <shobjidl_core.h>
#include <shellapi.h>
#include <propkey.h>
#include <cstring>
#include <cwchar>
#include <string>

namespace PrismForge::AppIcons {

inline constexpr wchar_t AppId[] = L"PrismForge.Studio";
inline constexpr int DisplayNameResourceId = 14001;

// Set wxApp::SetClassName(AppId) too: wxApp::Initialize applies that value after
// the application constructor. This call is safe to repeat in OnInit.
inline HRESULT InitializeProcessIdentity()
{
    return ::SetCurrentProcessExplicitAppUserModelID(AppId);
}

inline wxIconBundle Load()
{
    return wxIconBundle(wxS("id"),
        reinterpret_cast<WXHINSTANCE>(::GetModuleHandleW(nullptr)));
}

inline wxIcon ForTray(const wxIconBundle& icons, const wxWindow* window)
{
    const wxSize size(
        wxSystemSettings::GetMetric(wxSYS_SMALLICON_X, window),
        wxSystemSettings::GetMetric(wxSYS_SMALLICON_Y, window));
    return icons.GetIcon(size, wxIconBundle::FALLBACK_NEAREST_LARGER |
        wxIconBundle::FALLBACK_SYSTEM);
}

namespace Detail {

class ScopedVariant {
public:
    PROPVARIANT value{};
    ~ScopedVariant() { ::PropVariantClear(&value); }
    ScopedVariant() = default;
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    HRESULT Assign(const wchar_t* text)
    {
        const auto bytes = (std::wcslen(text) + 1) * sizeof(wchar_t);
        auto* owned = static_cast<wchar_t*>(::CoTaskMemAlloc(bytes));
        if (!owned) return E_OUTOFMEMORY;
        std::memcpy(owned, text, bytes);
        ::PropVariantClear(&value);
        value.vt = VT_LPWSTR;
        value.pwszVal = owned;
        return S_OK;
    }
};

class ScopedStore {
public:
    IPropertyStore* value = nullptr;
    ~ScopedStore() { if (value) value->Release(); }
    ScopedStore() = default;
    ScopedStore(const ScopedStore&) = delete;
    ScopedStore& operator=(const ScopedStore&) = delete;
};

struct ShellMetadata {
    std::wstring command;
    std::wstring displayName;
    std::wstring icon;
};

inline HRESULT MakeMetadata(ShellMetadata& metadata)
{
    wchar_t modulePath[32768];
    const auto length = ::GetModuleFileNameW(nullptr, modulePath, 32768);
    if (!length) return HRESULT_FROM_WIN32(::GetLastError());
    if (length >= 32768) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    const std::wstring executable(modulePath, length);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return E_UNEXPECTED;
    const auto iconPath = executable.substr(0, separator + 1) + L"res\\prismforge.ico";
    const auto attributes = ::GetFileAttributesW(iconPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(::GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    metadata.command = L"\"" + executable + L"\"";
    metadata.displayName = L"@" + executable + L",-" +
        std::to_wstring(DisplayNameResourceId);
    // The brand group is named, not a numeric resource ID. Use the shipped ICO
    // explicitly rather than guessing a negative executable resource number.
    metadata.icon = iconPath + L",0";
    return S_OK;
}

inline HRESULT SetString(IPropertyStore* store, REFPROPERTYKEY key,
    const wchar_t* text)
{
    ScopedVariant variant;
    const auto assigned = variant.Assign(text);
    return FAILED(assigned) ? assigned : store->SetValue(key, variant.value);
}

inline bool Matches(IPropertyStore* store, REFPROPERTYKEY key,
    const wchar_t* expected)
{
    ScopedVariant variant;
    return SUCCEEDED(store->GetValue(key, &variant.value)) &&
        variant.value.vt == VT_LPWSTR && variant.value.pwszVal &&
        std::wcscmp(variant.value.pwszVal, expected) == 0;
}

} // namespace Detail

// Apply before Show(). Setting the explicit window ID LAST tells the taskbar to
// refresh after command/display/icon are all available (Microsoft property docs).
// This affects only this window; it neither edits shortcuts nor clears caches.
inline HRESULT InitializeWindowIdentity(HWND window)
{
    if (!window) return E_INVALIDARG;
    Detail::ShellMetadata metadata;
    const auto metadataResult = Detail::MakeMetadata(metadata);
    if (FAILED(metadataResult)) return metadataResult;
    Detail::ScopedStore store;
    auto result = ::SHGetPropertyStoreForWindow(window,
        IID_PPV_ARGS(&store.value));
    if (FAILED(result)) return result;
    result = Detail::SetString(store.value, PKEY_AppUserModel_RelaunchCommand,
        metadata.command.c_str());
    if (FAILED(result)) return result;
    result = Detail::SetString(store.value, PKEY_AppUserModel_RelaunchDisplayNameResource,
        metadata.displayName.c_str());
    if (FAILED(result)) return result;
    result = Detail::SetString(store.value, PKEY_AppUserModel_RelaunchIconResource,
        metadata.icon.c_str());
    if (FAILED(result)) return result;
    return Detail::SetString(store.value, PKEY_AppUserModel_ID, AppId);
}

// Checks the same native property store that Explorer reads, with no UI input.
inline bool VerifyWindowIdentity(HWND window)
{
    if (!window) return false;
    Detail::ShellMetadata metadata;
    if (FAILED(Detail::MakeMetadata(metadata))) return false;
    Detail::ScopedStore store;
    if (FAILED(::SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store.value))))
        return false;
    return Detail::Matches(store.value, PKEY_AppUserModel_ID, AppId) &&
        Detail::Matches(store.value, PKEY_AppUserModel_RelaunchCommand,
            metadata.command.c_str()) &&
        Detail::Matches(store.value, PKEY_AppUserModel_RelaunchDisplayNameResource,
            metadata.displayName.c_str()) &&
        Detail::Matches(store.value, PKEY_AppUserModel_RelaunchIconResource,
            metadata.icon.c_str());
}

// Window property-store values outlive their SetValue calls. Release them before
// HWND destruction as required by SHGetPropertyStoreForWindow's lifetime rules.
inline HRESULT ClearWindowIdentity(HWND window)
{
    if (!window) return E_INVALIDARG;
    Detail::ScopedStore store;
    const auto opened = ::SHGetPropertyStoreForWindow(window,
        IID_PPV_ARGS(&store.value));
    if (FAILED(opened)) return opened;
    const PROPVARIANT empty{};
    HRESULT firstError = S_OK;
    const PROPERTYKEY keys[] = {
        PKEY_AppUserModel_ID,
        PKEY_AppUserModel_RelaunchCommand,
        PKEY_AppUserModel_RelaunchDisplayNameResource,
        PKEY_AppUserModel_RelaunchIconResource
    };
    for (const auto& key : keys) {
        const auto cleared = store.value->SetValue(key, empty);
        if (FAILED(cleared) && SUCCEEDED(firstError)) firstError = cleared;
    }
    return firstError;
}

} // namespace PrismForge::AppIcons
