#pragma once
#include "face_filter_asset.h"
#include "model_performance.h"
#include <atomic>
#include <string>
namespace PrismForge::Effects::Overlays::Realistic {
struct LocalFaceFilterImportResult {
 std::wstring nativePath;
 std::string displayName,sourceSha256,notice;
 std::shared_ptr<const NativeFaceFilter> model;
 // Generic imports use a uniform source-to-metre scale and translated origin.
 float sourceToMeters=1;
 std::array<float,3> sourceOrigin{0,0,0};
 bool preservedAuthoredAnchors=false;
};
// Embedded GLB2 converted to static authored rest geometry. No scripts, external files, Python, network or global installation.
// The absolute local library is created safely if needed. Publication is atomic;
// cancellation/errors preserve the input and every previously published model.
// Result changes only on success. Cancel may be null, otherwise caller owns its lifetime.
bool ImportLocalFaceFilter(const std::wstring& source,const std::wstring& library,
 LocalFaceFilterImportResult& result,std::string& error,const std::atomic<bool>* cancel=nullptr);
// Explicit bounded policy snapshot; the legacy overload resolves Balanced.
bool ImportLocalFaceFilter(const std::wstring& source,const std::wstring& library,
 LocalFaceFilterImportResult& result,std::string& error,const ModelResourceBudget& budget,const std::atomic<bool>* cancel=nullptr);
// Only a direct content-hash .pfmodel child of this non-reparse library qualifies.
bool IsOwnedLocalFaceFilterPath(const std::wstring& path,const std::wstring& library,std::string& error);
// Removes only the owned converted copy; never the source model or other files.
bool RemoveOwnedLocalFaceFilter(const std::wstring& path,const std::wstring& library,std::string& error);
}
