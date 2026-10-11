#pragma once
// Manager-local Windows namespace ownership. No reparse following and no delete
// sharing on a held component. Mutations use the same DELETE-capable object handle.
#include <bcrypt.h>
#include <map>
#include <set>
#include <limits>
#include <cstring>
#include <cwctype>
#pragma comment(lib, "bcrypt.lib")
namespace PrismForge::Effects::Packs::PackStorage {
namespace fs = std::filesystem;
inline std::wstring NativePath(const fs::path& path){auto p=path.native();std::replace(p.begin(),p.end(),L'/',L'\\');return p.rfind(L"\\\\?\\",0)==0?p:L"\\\\?\\"+p;}
inline std::string PathIdentity(const fs::path& path){
    auto wide=path.generic_wstring();
    int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,wide.data(),int(wide.size()),nullptr,0,nullptr,nullptr,0);
    if(!n)throw std::runtime_error("Cannot normalize Windows path identity");
    std::wstring upper(size_t(n),L'\0');
    if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,wide.data(),int(wide.size()),upper.data(),n,nullptr,nullptr,0)!=n)throw std::runtime_error("Cannot normalize path identity");
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,upper.data(),n,nullptr,0,nullptr,nullptr);
    if(!bytes)throw std::runtime_error("Cannot encode owned path identity");
    std::string utf8(size_t(bytes),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,upper.data(),n,utf8.data(),bytes,nullptr,nullptr)!=bytes)throw std::runtime_error("Cannot encode owned path identity");return utf8;
}
inline std::string WinError(const char* action) {
    return std::string(action) + " (Win32 " + std::to_string(GetLastError()) + ")";
}
class Handle {
    HANDLE h_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE h):h_(h){}
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
    Handle(Handle&& x) noexcept:h_(x.h_){x.h_=INVALID_HANDLE_VALUE;}
    Handle& operator=(Handle&& x) noexcept { if(this!=&x){Reset();h_=x.h_;x.h_=INVALID_HANDLE_VALUE;}return *this; }
    ~Handle(){Reset();}
    HANDLE Get()const{return h_;}
    bool Valid()const{return h_!=INVALID_HANDLE_VALUE && h_!=nullptr;}
    bool Close(std::string& e){if(!Valid())return true;auto h=h_;h_=INVALID_HANDLE_VALUE;
        if(!CloseHandle(h)){e=WinError("Cannot close owned handle");return false;}return true;}
    void Reset(){if(Valid())CloseHandle(h_);h_=INVALID_HANDLE_VALUE;}
};
inline bool Attributes(HANDLE h, bool directory, std::string& error) {
    BY_HANDLE_FILE_INFORMATION i{};
    if(!GetFileInformationByHandle(h,&i)){error=WinError("Cannot inspect owned object");return false;}
    if((i.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       bool(i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory ||
       (!directory && i.nNumberOfLinks!=1)) {
        error="Owned object is a reparse point, wrong type or multiply linked file";return false;
    }
    return true;
}
inline bool ReadHandle(HANDLE h, uint64_t cap, std::vector<uint8_t>& out, std::string& error) {
    LARGE_INTEGER n{},zero{};
    if(!GetFileSizeEx(h,&n) || n.QuadPart<=0 || uint64_t(n.QuadPart)>cap ||
       !SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)) {error="Owned file is empty/outside read bound or inaccessible";return false;}
    out.resize(size_t(n.QuadPart)); size_t done=0;
    while(done<out.size()) {DWORD got=0;DWORD want=DWORD(std::min<size_t>(65536,out.size()-done));
        if(!ReadFile(h,out.data()+done,want,&got,nullptr)||got!=want){error=WinError("Incomplete owned read");return false;}
        done+=got;
    }
    BYTE extra;DWORD got=0;
    if(!ReadFile(h,&extra,1,&got,nullptr)||got){error="Owned file changed length during bounded read";return false;}
    return true;
}
inline bool OriginalAbsolute(const fs::path& path,std::string& error) {
    const auto raw=path.generic_u8string();
    if(raw.size()<4 || !((raw[0]>='A'&&raw[0]<='Z')||(raw[0]>='a'&&raw[0]<='z')) ||
       raw[1]!=':' || raw[2]!='/' || path.root_directory().empty() || path.relative_path().empty() ||
       raw.find(':',2)!=std::string::npos || raw.find("//",2)!=std::string::npos) {
        error="Library authority requires an absolute ordinary drive path below its volume root";return false;
    }
    for(const auto& component:path.relative_path()) {
        const auto s=component.u8string();
        if(!PackValidator::ValidatePathComponent(s,error))return false;
        // NT short aliases must not give one directory multiple mutex identities.
        if(s.find('~')!=std::string::npos){error="Short-name aliases are not managed-library authority";return false;}
        for(unsigned char c:s)if(c=='<'||c=='>'||c=='"'||c=='|'||c=='?'||c=='*'){
            error="Illegal Windows path character";return false;
        }
    }
    return true;
}
inline std::string RandomHex(size_t bytes,std::string& error) {
    std::vector<unsigned char> b(bytes);
    if(BCryptGenRandom(nullptr,b.data(),ULONG(b.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){error="Cannot generate transaction entropy";return {};}
    static const char* hex="0123456789abcdef";std::string s; s.reserve(bytes*2);
    for(auto v:b){s.push_back(hex[v>>4]);s.push_back(hex[v&15]);}return s;
}
inline bool Hex64(const std::string& s){return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline bool TxId(const std::string& s){return s.size()==35 && s.rfind("tx_",0)==0 && std::all_of(s.begin()+3,s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline std::vector<uint8_t> Unhex(const std::string& s){std::vector<uint8_t>b;for(size_t i=0;i<s.size();i+=2){auto d=[](char c){return c<='9'?c-'0':c-'a'+10;};b.push_back(uint8_t(d(s[i])*16+d(s[i+1])));}return b;}
inline std::string Mac(const std::string& key,const std::string& message) {
    auto k=Unhex(key);k.resize(64,0);std::vector<uint8_t> inner(64),outer(64);
    for(size_t i=0;i<64;++i){inner[i]=k[i]^0x36;outer[i]=k[i]^0x5c;}
    Sha256 a;a.Update(inner.data(),inner.size());a.Update(message);auto digest=Unhex(a.Final());
    Sha256 b;b.Update(outer.data(),outer.size());b.Update(digest.data(),digest.size());return b.Final();
}
inline bool RenameHandle(HANDLE h,const fs::path& destination,bool replace,std::string& error) {
    auto name=NativePath(destination);
    const auto bytes=name.size()*sizeof(wchar_t);
    std::vector<uint8_t> mem(sizeof(FILE_RENAME_INFO)+bytes+sizeof(wchar_t),0);
    auto* i=reinterpret_cast<FILE_RENAME_INFO*>(mem.data());
    i->ReplaceIfExists=replace?TRUE:FALSE;i->RootDirectory=nullptr;i->FileNameLength=DWORD(bytes);
    std::memcpy(i->FileName,name.data(),bytes);
    if(!SetFileInformationByHandle(h,FileRenameInfo,i,DWORD(mem.size()))){error=WinError("Cannot rename owned object");return false;}return true;
}
inline bool DeleteHandle(Handle& h,std::string& error) {
    FILE_DISPOSITION_INFO info{TRUE};
    if(!SetFileInformationByHandle(h.Get(),FileDispositionInfo,&info,sizeof(info))){error=WinError("Cannot delete owned object");return false;}
    return h.Close(error);
}
inline bool DurableFile(const fs::path& path,const std::vector<uint8_t>& bytes,bool createNew,
    std::string& error,Handle* retained=nullptr) {
    Handle h(CreateFileW(NativePath(path).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,
        createNew?CREATE_NEW:CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!h.Valid()||!Attributes(h.Get(),false,error)){if(error.empty())error=WinError("Cannot create owned file");return false;}
    size_t done=0;
    while(done<bytes.size()){DWORD put=0;DWORD want=DWORD(std::min<size_t>(65536,bytes.size()-done));
        if(!WriteFile(h.Get(),bytes.data()+done,want,&put,nullptr)||put!=want){error=WinError("Incomplete durable write");return false;}done+=put;}
    if(!FlushFileBuffers(h.Get())){error=WinError("Cannot flush owned file to storage");return false;}
    if(retained){*retained=std::move(h);return true;}return h.Close(error);
}
class LibraryOwner {
    fs::path root_;std::vector<Handle> guards_;std::set<std::wstring> guarded_;std::string key_;
public:
    const fs::path& Root()const{return root_;} const std::string& Key()const{return key_;}
    bool HoldDirectory(const fs::path& path,bool create,std::string& error) {
        auto id=path.native();std::transform(id.begin(),id.end(),id.begin(),::towlower);
        if(guarded_.count(id))return true;
        Handle h(CreateFileW(NativePath(path).c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.Valid()&&(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)&&create){
            if(!CreateDirectoryW(NativePath(path).c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){error=WinError("Cannot create owned directory");return false;}
            h=Handle(CreateFileW(NativePath(path).c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        }
        if(!h.Valid()){error=WinError("Cannot hold directory namespace");return false;}
        if(!Attributes(h.Get(),true,error))return false;
        guards_.push_back(std::move(h));guarded_.insert(std::move(id));return true;
    }
    bool Open(const fs::path& original,bool create,std::string& error) {
        if(!OriginalAbsolute(original,error))return false;root_=original;
        fs::path p=original.root_path();if(!HoldDirectory(p,false,error))return false;
        for(const auto& c:original.relative_path()){p/=c;if(!HoldDirectory(p,create,error))return false;}
        const auto marker=root_/".prismforge-library-v2";
        Handle h(CreateFileW(NativePath(marker).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.Valid()&&(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)&&create){
            key_=RandomHex(32,error);if(key_.empty())return false;
            const auto text=std::string("PFSOWN2\n")+key_+"\n";
            return DurableFile(marker,std::vector<uint8_t>(text.begin(),text.end()),true,error);
        }
        if(!h.Valid()||!Attributes(h.Get(),false,error)){if(error.empty())error="Missing managed-library ownership marker";return false;}
        std::vector<uint8_t> bytes;if(!ReadHandle(h.Get(),128,bytes,error))return false;
        std::string s(bytes.begin(),bytes.end());
        if(s.size()!=73||s.substr(0,8)!="PFSOWN2\n"||s.back()!='\n'||!Hex64(s.substr(8,64))){error="Invalid managed-library ownership marker";return false;}
        key_=s.substr(8,64);return true;
    }
    bool EnsurePackParent(const std::string& id,std::string& error){return HoldDirectory(root_/id,true,error);}
    bool HoldStaging(bool create,std::string& error){return HoldDirectory(root_/".staging",create,error);}
};
class OwnedTree {
    struct Entry{fs::path path;std::string relative;bool directory;Handle h;};
    std::vector<Entry> entries_;fs::path path_;
    bool Add(const fs::path& p,const std::string& relative,bool directory,std::string& error){
        Handle h(CreateFileW(NativePath(p).c_str(),DELETE|FILE_READ_ATTRIBUTES|(directory?FILE_LIST_DIRECTORY:GENERIC_READ),
            directory?(FILE_SHARE_READ|FILE_SHARE_WRITE):FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.Valid()){error=WinError("Cannot acquire mutation ownership");return false;}
        if(!Attributes(h.Get(),directory,error))return false;
        entries_.push_back({p,relative,directory,std::move(h)});return true;
    }
public:
    bool Open(const fs::path& p,std::string& error){
        path_=p;if(!Add(p,"",true,error))return false;
        std::error_code ec;std::set<std::string> seen;
        fs::recursive_directory_iterator it(p,fs::directory_options::none,ec),end;
        if(ec){error="Cannot enumerate owned tree: "+ec.message();return false;}
        for(;it!=end;it.increment(ec)){
            if(ec){error="Cannot enumerate owned tree: "+ec.message();return false;}
            const auto rel=it->path().lexically_relative(p).generic_u8string();
            for(const auto& c:fs::u8path(rel))if(!PackValidator::ValidatePathComponent(c.u8string(),error))return false;
            auto lower=rel;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
            if(!seen.insert(lower).second){error="Case-colliding owned tree entries";return false;}
            const auto attrs=GetFileAttributesW(NativePath(it->path()).c_str());
            if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT)){
                error="Unowned/reparse entry in mutation tree";return false;}
            if(!Add(it->path(),rel,(attrs&FILE_ATTRIBUTE_DIRECTORY)!=0,error))return false;
            if(entries_.size()>512){error="Owned tree exceeds entry bound";return false;}
        }
        if(ec){error="Cannot finish owned tree enumeration";return false;}return true;
    }
    std::vector<std::string> Directories()const {std::vector<std::string> result;for(const auto& e:entries_)if(e.directory&&!e.relative.empty())result.push_back(e.relative);return result;}
    bool Bytes(std::unordered_map<std::string,std::vector<uint8_t>>& files,std::string& error){
        uint64_t total=0;
        for(auto& e:entries_)if(!e.directory){std::vector<uint8_t>b;
            if(!ReadHandle(e.h.Get(),e.relative=="manifest.json"?1024*1024:50ull*1024*1024,b,error))return false;
            total+=b.size();if(total>151ull*1024*1024){error="Owned tree exceeds total byte bound";return false;}
            files.emplace(e.relative,std::move(b));}return true;
    }
    bool Rename(const fs::path& destination,std::string& error){
        // Windows refuses a directory rename while children deny delete sharing.
        // Keep the actual root DELETE handle (and both owned parents), release
        // only descendants, rename that root by handle, then reacquire children.
        // Callers revalidate the complete byte digest before further mutation.
        for(size_t i=1;i<entries_.size();++i)entries_[i].h.Reset();
        if(!RenameHandle(entries_.front().h.Get(),destination,false,error))return false;
        path_=destination;entries_.front().path=destination;
        entries_.erase(entries_.begin()+1,entries_.end());
        std::error_code ec;fs::recursive_directory_iterator it(destination,fs::directory_options::none,ec),end;
        if(ec){error="Cannot reacquire renamed owned tree: "+ec.message();return false;}
        for(;it!=end;it.increment(ec)){
            if(ec){error="Renamed tree enumeration failed: "+ec.message();return false;}
            const auto relative=it->path().lexically_relative(destination).generic_u8string();
            for(const auto& c:fs::u8path(relative))if(!PackValidator::ValidatePathComponent(c.u8string(),error))return false;
            auto attr=GetFileAttributesW(NativePath(it->path()).c_str());
            if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT)){
                error="Renamed tree contains unowned/reparse entry";return false;}
            if(!Add(it->path(),relative,(attr&FILE_ATTRIBUTE_DIRECTORY)!=0,error))return false;
            if(entries_.size()>512){error="Renamed tree exceeds entry bound";return false;}
        }
        if(ec){error="Cannot finish renamed tree ownership";return false;}return true;
    }
    bool Remove(std::string& error){
        for(auto i=entries_.rbegin();i!=entries_.rend();++i)if(!DeleteHandle(i->h,error))return false;
        entries_.clear();return true;
    }
};
// This parser accepts only one finite JSON object of scalar strings/bools/uints.
// Unknown/duplicate keys, escapes, leading-zero/negative numbers and trailing data fail.
struct Scalar{enum Kind{String,Number,Boolean}kind;std::string text;};
inline bool ParseRecord(const std::string& s,std::map<std::string,Scalar>& out,std::string& error){
    size_t p=0;auto ws=[&]{while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r'))++p;};
    auto quoted=[&](std::string& v){if(p>=s.size()||s[p++]!='"')return false;v.clear();
        while(p<s.size()&&s[p]!='"'){unsigned char c=s[p++];if(c<32||c>=127||c=='\\')return false;v.push_back(char(c));if(v.size()>512)return false;}
        return p<s.size()&&s[p++]=='"';};
    ws();if(p>=s.size()||s[p++]!='{'){error="Journal must be a strict scalar JSON object";return false;}ws();
    if(p<s.size()&&s[p]=='}'){error="Empty journal";return false;}
    while(p<s.size()){
        std::string key;Scalar value{};if(!quoted(key)){error="Invalid journal key";return false;}ws();
        if(p>=s.size()||s[p++]!=':'){error="Missing journal colon";return false;}ws();
        if(p<s.size()&&s[p]=='"'){value.kind=Scalar::String;if(!quoted(value.text)){error="Invalid journal string";return false;}}
        else if(s.compare(p,4,"true")==0){value.kind=Scalar::Boolean;value.text="true";p+=4;}
        else if(s.compare(p,5,"false")==0){value.kind=Scalar::Boolean;value.text="false";p+=5;}
        else {value.kind=Scalar::Number;size_t start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;
            value.text=s.substr(start,p-start);if(value.text.empty()||value.text.size()>20||(value.text.size()>1&&value.text[0]=='0')){error="Invalid journal integer";return false;}
            try{size_t n=0;(void)std::stoull(value.text,&n);if(n!=value.text.size())return false;}catch(...){error="Journal integer overflow";return false;}}
        if(!out.emplace(key,std::move(value)).second){error="Duplicate journal key";return false;}
        ws();if(p>=s.size()){error="Truncated journal";return false;}
        if(s[p]=='}'){++p;ws();if(p!=s.size()){error="Trailing journal data";return false;}return true;}
        if(s[p++]!=','){error="Invalid journal separator";return false;}ws();
    }
    error="Truncated journal";return false;
}
} // namespace PrismForge::Effects::Packs::PackStorage
