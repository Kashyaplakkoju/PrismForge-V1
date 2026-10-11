#include "face_landmarker_runtime.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace PrismForge::Providers {
namespace {
// Layouts/signatures copied from the PINNED 0.10.32 Windows wheel's ctypes
// definitions. They deliberately exclude newer host/telemetry options.
struct NativeBaseOptions { const char* buffer; unsigned count; const char* path; int delegate; };
struct NativeLandmark {
    float x,y,z; bool hasVisibility; float visibility; bool hasPresence;
    float presence; const char* name;
};
struct NativeLandmarks { NativeLandmark* data; uint32_t count; };
struct NativeCategory { int index; float score; const char* name; const char* displayName; };
struct NativeCategories { NativeCategory* data; uint32_t count; };
struct NativeMatrix { uint32_t rows,cols; float* data; };
struct NativeResult {
    NativeLandmarks* landmarks; uint32_t landmarkCount;
    NativeCategories* blendshapes; uint32_t blendshapeCount;
    NativeMatrix* matrices; uint32_t matrixCount;
};
using NativeCallback = void (__cdecl*)(int32_t,const NativeResult*,void*,int64_t);
struct NativeOptions {
    NativeBaseOptions base;
    int runningMode,numFaces;
    float detectionConfidence,presenceConfidence,trackingConfidence;
    bool outputBlendshapes,outputMatrices;
    NativeCallback callback;
};
static_assert(sizeof(void*)==8 && sizeof(bool)==1 && sizeof(unsigned)==4, "Pinned MediaPipe ABI requires Windows x64");
static_assert(sizeof(NativeBaseOptions)==32 && offsetof(NativeBaseOptions,path)==16);
static_assert(sizeof(NativeLandmark)==40 && offsetof(NativeLandmark,name)==32);
static_assert(offsetof(NativeLandmark,visibility)==16 && offsetof(NativeLandmark,presence)==24);
static_assert(sizeof(NativeLandmarks)==16 && sizeof(NativeCategories)==16 && sizeof(NativeCategory)==24);
static_assert(sizeof(NativeMatrix)==16 && offsetof(NativeMatrix,data)==8);
static_assert(sizeof(NativeResult)==48 && offsetof(NativeResult,blendshapes)==16 && offsetof(NativeResult,matrices)==32);
static_assert(sizeof(NativeOptions)==64 && offsetof(NativeOptions,runningMode)==32 && offsetof(NativeOptions,callback)==56);
using CreateFn=int32_t (__cdecl*)(NativeOptions*,void**,char**);
using DetectFn=int32_t (__cdecl*)(void*,void*,const void*,int64_t,NativeResult*,char**);
using CloseFn=int32_t (__cdecl*)(void*,char**);
using CloseResultFn=void (__cdecl*)(NativeResult*);
using CreateImageFn=int32_t (__cdecl*)(int,int,int,const uint8_t*,int,void**,char**);
using FreeImageFn=void (__cdecl*)(void*);
using FreeErrorFn=void (__cdecl*)(void*);
constexpr std::array<unsigned char,32> kModelDigest={
    0x64,0x18,0x4e,0x22,0x9b,0x26,0x31,0x07,0xbc,0x2b,0x80,0x4c,0x66,0x25,0xdb,0x13,
    0x41,0xff,0x2b,0xb7,0x31,0x87,0x4b,0x0b,0xcc,0x2f,0xe6,0x54,0x4e,0x0b,0xc9,0xff};
constexpr size_t kMaxFileBytes=128ULL*1024*1024;
constexpr size_t kMaxImageBytes=32ULL*1024*1024;
constexpr std::array<unsigned char,32> kRuntimeDigest={
    0x6d,0xbb,0x21,0xa3,0x91,0xcf,0x79,0xbe,0xad,0x3d,0x82,0xef,0xb0,0x0d,0x02,0xb3,
    0x63,0x84,0x35,0x34,0x0c,0x95,0x05,0x45,0x04,0x2b,0x83,0x19,0x4a,0x5b,0x87,0xe7};
struct FileOwner {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~FileOwner(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
bool OpenFile(const std::wstring& path,FileOwner& file,std::wstring& resolved,size_t& bytes,std::string& error) {
    // No relative path, UNC/device path, embedded NUL or alternate data stream.
    if(path.size()<3 || !((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z')) ||
       path[1]!=L':' || (path[2]!=L'\\'&&path[2]!=L'/') || path.find(L'\0')!=std::wstring::npos ||
       path.find(L':',2)!=std::wstring::npos) {error="Face runtime requires an absolute local file path";return false;}
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER length{};
    if(file.value==INVALID_HANDLE_VALUE || GetFileType(file.value)!=FILE_TYPE_DISK ||
       !GetFileInformationByHandle(file.value,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
       !GetFileSizeEx(file.value,&length) || length.QuadPart<=0 || uint64_t(length.QuadPart)>kMaxFileBytes) {
        error="Face runtime file is missing, not a regular file, or too large";return false;
    }
    DWORD need=GetFinalPathNameByHandleW(file.value,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!need || need>32768) {error="Cannot resolve face runtime file";return false;}
    std::vector<wchar_t> buffer(size_t(need)+1);
    DWORD got=GetFinalPathNameByHandleW(file.value,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!got || got>=buffer.size()) {error="Cannot resolve face runtime file";return false;}
    resolved.assign(buffer.data(),got);
    if(resolved.rfind(L"\\\\?\\",0)!=0 || resolved.size()<7 || resolved[5]!=L':') {
        error="Face runtime resolved outside a local drive";return false;
    }
    bytes=size_t(length.QuadPart);return true;
}
bool ReadExact(HANDLE file,std::vector<uint8_t>& bytes,size_t size,std::string& error) {
    bytes.resize(size);size_t offset=0;
    while(offset<size) {DWORD got=0;DWORD request=DWORD(std::min(size-offset,size_t(64*1024)));
        if(!ReadFile(file,bytes.data()+offset,request,&got,nullptr)||got!=request){error="Face model read failed";return false;}
        offset+=got;
    }return true;
}
bool CheckFileDigest(HANDLE file,const std::array<unsigned char,32>& expected,const char* label,std::string& error) {
    // Hash storage outlives its handles even on allocation failure.
    std::vector<unsigned char> object;
    struct HashOwner {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
        void Clear(){if(hash)BCryptDestroyHash(hash);hash=nullptr;
            if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;}
        ~HashOwner(){Clear();}
    } owner;
    auto& algorithm=owner.algorithm;auto& hash=owner.hash;
    const auto clean=[&](){owner.Clear();};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) {clean();error="Face runtime hash unavailable";return false;}
    DWORD objectBytes=0,received=0;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&received,0)<0 ||
       received!=sizeof(objectBytes) || objectBytes>1024*1024) {clean();error="Face runtime hash setup failed";return false;}
    object.resize(objectBytes);
    if(BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0)<0) {clean();error="Face runtime hash setup failed";return false;}
    std::array<unsigned char,65536> buffer{};DWORD got=0;bool ok=true;
    while(true) {
        if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&got,nullptr)){ok=false;break;}
        if(!got)break;
        if(BCryptHashData(hash,buffer.data(),got,0)<0){ok=false;break;}
    }
    std::array<unsigned char,32> digest{};
    if(ok)ok=BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)>=0;
    clean();
    if(!ok || digest!=expected){error=std::string("Pinned face ")+label+" hash mismatch";return false;}
    return true;
}
std::string NativeError(int32_t status,char* message,const char* action,FreeErrorFn release) {
    std::string result;
    try {result=action;result+=" (native status ";result+=std::to_string(status);result+=")";
        if(message){result+=": ";size_t n=0;while(n<2048&&message[n])++n;result.append(message,n);}
    }catch(...){if(message&&release)release(message);throw;}
    if(message&&release)release(message);return result;
}
}
struct FaceLandmarkerRuntime::Impl {
    std::mutex mutex;
    HMODULE library=nullptr;HANDLE runtimeFile=INVALID_HANDLE_VALUE;void* task=nullptr;
    CreateFn create=nullptr;DetectFn detect=nullptr;CloseFn close=nullptr;
    CloseResultFn closeResult=nullptr;CreateImageFn createImage=nullptr;FreeImageFn freeImage=nullptr;FreeErrorFn freeError=nullptr;
    std::vector<uint8_t> model,packed;
    bool closeFailed=false;
    bool haveTags=false;FaceFrameTags lastTags{};int64_t lastNativeMs=-1;
    bool CloseTask() {
        if(task&&close){char* error=nullptr;const int32_t status=close(task,&error);if(error&&freeError)freeError(error);
            // The pinned C API does not delete a task when Close returns an error.
            // Retain its model/DLL rather than unloading code used by native workers.
            if(status!=0){closeFailed=true;return false;}}
        task=nullptr;closeFailed=false;haveTags=false;lastTags={};lastNativeMs=-1;return true;
    }
    bool Clear() {
        if(!CloseTask())return false;
        model.clear();packed.clear();
        if(library)FreeLibrary(library);library=nullptr;
        if(runtimeFile!=INVALID_HANDLE_VALUE)CloseHandle(runtimeFile);runtimeFile=INVALID_HANDLE_VALUE;
        create=nullptr;detect=nullptr;close=nullptr;closeResult=nullptr;createImage=nullptr;freeImage=nullptr;freeError=nullptr;
        return true;
    }
    bool CreateTask(std::string& error) {
        NativeOptions options{};options.base.buffer=reinterpret_cast<const char*>(model.data());
        options.base.count=unsigned(model.size());options.base.delegate=0;
        options.runningMode=2;options.numFaces=1;
        options.detectionConfidence=options.presenceConfidence=options.trackingConfidence=0.5f;
        options.outputBlendshapes=true;options.outputMatrices=true;
        char* nativeError=nullptr;void* candidate=nullptr;
        int32_t status=create(&options,&candidate,&nativeError);
        if(status!=0||!candidate) {
            if(candidate){task=candidate;CloseTask();}
            error=NativeError(status,nativeError,"Face model creation failed",freeError);
            return false;
        }
        if(nativeError)freeError(nativeError);task=candidate;return true;
    }
    template<typename Function> bool Load(const char* name,Function& destination,std::string& error) {
        destination=reinterpret_cast<Function>(GetProcAddress(library,name));
        if(!destination){error=std::string("Missing pinned face runtime export: ")+name;return false;}return true;
    }
};
FaceLandmarkerRuntime::FaceLandmarkerRuntime():impl_(std::make_unique<Impl>()){}
FaceLandmarkerRuntime::~FaceLandmarkerRuntime(){
    Stop();
    // Exceptional native-close failure: keep the bounded task/model/library alive
    // until process exit. Destroying their backing storage would risk use-after-free.
    if(impl_&&impl_->task)impl_.release();
}
bool FaceLandmarkerRuntime::Initialize(const std::wstring& runtimeDllPath,const std::wstring& modelPath,std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);error.clear();if(!impl_->Clear()){error="Previous native face task could not close";return false;}
    try {
        FileOwner runtime,model;std::wstring runtimeResolved,modelResolved;size_t runtimeBytes=0,modelBytes=0;
        if(!OpenFile(runtimeDllPath,runtime,runtimeResolved,runtimeBytes,error)||
           !OpenFile(modelPath,model,modelResolved,modelBytes,error)||!CheckFileDigest(runtime.value,kRuntimeDigest,"runtime",error)||
           !CheckFileDigest(model.value,kModelDigest,"model",error)){impl_->Clear();return false;}
        LARGE_INTEGER beginning{};
        if(!SetFilePointerEx(model.value,beginning,nullptr,FILE_BEGIN)||
           !ReadExact(model.value,impl_->model,modelBytes,error)){impl_->Clear();return false;}
        impl_->library=LoadLibraryExW(runtimeResolved.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!impl_->library){error="Cannot load reviewed face runtime or its system dependencies";impl_->Clear();return false;}
        impl_->runtimeFile=runtime.value;runtime.value=INVALID_HANDLE_VALUE;
        if(!impl_->Load("MpFaceLandmarkerCreate",impl_->create,error)||
           !impl_->Load("MpFaceLandmarkerDetectForVideo",impl_->detect,error)||
           !impl_->Load("MpFaceLandmarkerClose",impl_->close,error)||
           !impl_->Load("MpFaceLandmarkerCloseResult",impl_->closeResult,error)||
           !impl_->Load("MpImageCreateFromUint8Data",impl_->createImage,error)||
           !impl_->Load("MpImageFree",impl_->freeImage,error)||
           !impl_->Load("MpErrorFree",impl_->freeError,error)||!impl_->CreateTask(error)){impl_->Clear();return false;}
        return true;
    }catch(...){impl_->Clear();error="Face runtime initialization allocation failed";return false;}
}
bool FaceLandmarkerRuntime::DetectRGB(const uint8_t* data,size_t capacityBytes,int width,int height,int strideBytes,
                                    FaceFrameTags tags,FaceObservation& observation,std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);observation={};error.clear();
    if(impl_->closeFailed){error="Native face task failed to close; inference is disabled";return false;}
    if(!impl_->library||!impl_->create){error="Face runtime is not initialized";return false;}
    if(!data||width<=0||height<=0||width>8192||height>8192||strideBytes<=0||
       uint64_t(strideBytes)<uint64_t(width)*3||uint64_t(width)*height*3>kMaxImageBytes||
       uint64_t(strideBytes)*uint64_t(height-1)+uint64_t(width)*3>capacityBytes||
       !tags.generation||!tags.sequence||tags.microPts<0){error="Invalid owned face RGB frame or tags";return false;}
    if(impl_->haveTags&&(tags.generation<impl_->lastTags.generation||
       (tags.generation==impl_->lastTags.generation&&(tags.sequence<=impl_->lastTags.sequence||tags.microPts<impl_->lastTags.microPts)))) {
        error="Out-of-order face frame";return false;
    }
    if(impl_->haveTags&&tags.generation!=impl_->lastTags.generation&&!impl_->CloseTask()){error="Previous face source could not close";return false;}
    try {
        if(!impl_->task&&!impl_->CreateTask(error))return false;
        const int64_t requestedMs=tags.microPts/1000;
        if(impl_->lastNativeMs==std::numeric_limits<int64_t>::max()){error="Face runtime timestamp exhausted";return false;}
        const int64_t nativeMs=std::max(requestedMs,impl_->lastNativeMs+1);
        const size_t rowBytes=size_t(width)*3,packedBytes=rowBytes*size_t(height);
        const uint8_t* packedData=data;
        if(size_t(strideBytes)!=rowBytes){impl_->packed.resize(packedBytes);
            for(int y=0;y<height;++y)std::memcpy(impl_->packed.data()+size_t(y)*rowBytes,data+size_t(y)*size_t(strideBytes),rowBytes);
            packedData=impl_->packed.data();
        }
        struct ImageOwner {void* value=nullptr;FreeImageFn release=nullptr;~ImageOwner(){if(value&&release)release(value);}} image;
        image.release=impl_->freeImage;char* nativeError=nullptr;
        int32_t status=impl_->createImage(1,width,height,packedData,int(packedBytes),&image.value,&nativeError);
        if(status!=0||!image.value){error=NativeError(status,nativeError,"Face RGB image creation failed",impl_->freeError);return false;}
        if(nativeError)impl_->freeError(nativeError);
        struct ResultOwner {NativeResult value{};CloseResultFn release=nullptr;~ResultOwner(){if(release)release(&value);}} result;
        result.release=impl_->closeResult;nativeError=nullptr;
        // Advance submission ordering even if native inference fails: its graph
        // may already have consumed this timestamp. Original frame tags stay exact.
        impl_->lastNativeMs=nativeMs;impl_->lastTags=tags;impl_->haveTags=true;
        status=impl_->detect(impl_->task,image.value,nullptr,nativeMs,&result.value,&nativeError);
        if(status!=0){error=NativeError(status,nativeError,"Face inference failed",impl_->freeError);return false;}
        if(nativeError)impl_->freeError(nativeError);
        const auto& r=result.value;FaceObservation candidate;
        candidate.tags=tags;candidate.imageWidth=width;candidate.imageHeight=height;
        if(r.landmarkCount>1||r.blendshapeCount>1||r.matrixCount>1){error="Face result exceeds one-face contract";return false;}
        if(r.landmarkCount==0) {
            if(r.blendshapeCount||r.matrixCount){error="Face metadata has no corresponding face";return false;}
            observation=std::move(candidate);return true;
        }
        if(!r.landmarks||r.landmarks[0].count!=478||!r.landmarks[0].data){error="Invalid pinned face landmark output";return false;}
        candidate.vertices.reserve(478);
        for(uint32_t i=0;i<478;++i){const auto& point=r.landmarks[0].data[i];
            if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)){error="Non-finite face geometry";return false;}
            candidate.vertices.push_back({point.x,point.y,point.z});
        }
        if(r.blendshapeCount) {
            if(!r.blendshapes||r.blendshapes[0].count!=52||!r.blendshapes[0].data){error="Invalid face expression output";return false;}
            std::array<bool,52> seen{};
            for(uint32_t i=0;i<52;++i){const auto& category=r.blendshapes[0].data[i];
                if(category.index<0||category.index>=52||seen[size_t(category.index)]||!std::isfinite(category.score)||category.score<0||category.score>1){error="Invalid face expression index or score";return false;}
                seen[size_t(category.index)]=true;candidate.blendshapes[size_t(category.index)]=category.score;
            }candidate.hasBlendshapes=true;
        }
        if(r.matrixCount) {
            if(!r.matrices||r.matrices[0].rows!=4||r.matrices[0].cols!=4||!r.matrices[0].data){error="Invalid face transform output";return false;}
            // The pinned C matrix is COLUMN-major. Expose an owned ROW-major4x4.
            for(size_t row=0;row<4;++row)for(size_t column=0;column<4;++column){float value=r.matrices[0].data[column*4+row];
                if(!std::isfinite(value)){error="Non-finite face transform";return false;}candidate.transform[row*4+column]=value;
            }candidate.hasTransform=true;
        }
        candidate.detected=true;observation=std::move(candidate);return true;
    }catch(...){observation={};error="Face runtime result allocation failed";return false;}
}
void FaceLandmarkerRuntime::Reset(){std::string ignored;Reset(ignored);}
bool FaceLandmarkerRuntime::Reset(std::string& error){
    std::lock_guard<std::mutex> lock(impl_->mutex);error.clear();
    if(!impl_->CloseTask()){error="Native face task failed to close; tracking reset is unavailable";return false;}
    return true;
}
void FaceLandmarkerRuntime::Stop(){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->Clear();}
} // namespace PrismForge::Providers
