#include "../../windows/src/effects/packs/pack_preview_decoder.h"
#include "../../windows/src/effects/packs/pack_validator.h"
#include "../../windows/src/effects/packs/sha256.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <cstring>
#include <array>
#include <cmath>
using namespace PrismForge::Effects::Packs;
namespace fs=std::filesystem;using Bytes=std::vector<uint8_t>;
int assertions=0,failures=0,groups=0,skips=0;
#define CHECK(c,m) do{++assertions;if(!(c)){++failures;std::cerr<<"FAIL "<<m<<" at "<<__LINE__<<"\n";}}while(0)
void Group(const char* n){++groups;std::cout<<"GROUP "<<groups<<": "<<n<<"\n";}
Bytes Read(const fs::path& path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Missing required fixture: "+path.string());return Bytes(std::istreambuf_iterator<char>(f),{});}
void Write(const fs::path& p,const Bytes& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("Fixture write failed");}
uint32_t Crc(const uint8_t* p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(int i=0;i<8;++i)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
void Be(Bytes& b,size_t off,uint32_t v){for(int i=0;i<4;++i)b[off+i]=uint8_t(v>>(24-8*i));}
void FixIhdr(Bytes& b){Be(b,29,Crc(b.data()+12,17));}
PackAssetEntry Entry(const Bytes& b,const std::string& path="preview.png"){PackAssetEntry a;a.assetType=PackAssetType::PreviewImage;a.assetId="thumbnail";a.relativePath=path;a.sizeBytes=b.size();a.sha256Hex=Sha256::ComputeHex(b.data(),b.size());return a;}
const Bytes expected={255,0,0,0,0,255,0,128,0,0,255,255,40,80,120,255,200,100,50,64,90,30,240,255};
// Each list is an independently specified destination-to-source pixel order for
// the asymmetric 2x3 fixture (avoids reproducing the production transform loop).
const std::array<std::array<int,6>,8> order={{{0,1,2,3,4,5},{1,0,3,2,5,4},{5,4,3,2,1,0},{4,5,2,3,0,1},
    {0,2,4,1,3,5},{4,2,0,5,3,1},{5,3,1,4,2,0},{1,3,5,0,2,4}}};
bool Junction(const fs::path& alias,const fs::path& target){
    fs::create_directory(alias);HANDLE f=CreateFileW(alias.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    std::wstring substitute=L"\\??\\"+target.wstring(),print=target.wstring();
    struct Data {DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;wchar_t path[1024];} data{};
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subLength=WORD(substitute.size()*2);data.printOffset=WORD((substitute.size()+1)*2);data.printLength=WORD(print.size()*2);
    memcpy(data.path,substitute.c_str(),(substitute.size()+1)*2);memcpy(reinterpret_cast<uint8_t*>(data.path)+data.printOffset,print.c_str(),(print.size()+1)*2);
    data.length=WORD(8+data.printOffset+data.printLength+2);DWORD out=0;BOOL ok=DeviceIoControl(f,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&out,nullptr);CloseHandle(f);return ok!=FALSE;
}
int main(int argc,char** argv){
    if(argc>1&&std::string(argv[1])=="--exit-proof")return 42;
    if(argc>1&&std::string(argv[1])=="--hang-proof"){Sleep(60000);return 0;}
    try{
    if(argc!=3)throw std::runtime_error("Expected absolute fixture root and sample root");const fs::path fixture=argv[1],samples=argv[2];
    Bytes png=Read(fixture/"rgba_2x3.png"),bmp=Read(fixture/"rgb_2x3.bmp"),jpg=Read(fixture/"solid.jpg"),raw=Read(fixture/"raw64.raw");
    Group("Concurrent first decode, no mutable CRC initialization");
    std::atomic<int> concurrentFailures{0};std::vector<std::thread> threads;
    for(int t=0;t<12;++t)threads.emplace_back([&]{for(int i=0;i<20;++i){auto d=PackPreviewDecoder::DecodePreviewBytes(png.data(),png.size(),"x.png");if(!d.success||d.image.Pixels()!=expected)++concurrentFailures;}});
    for(auto& t:threads)t.join();CHECK(concurrentFailures==0,"240 concurrent complete exact-pixel decodes");
    Group("Straight RGBA, complete pixels and source-independent lifetime");
    auto decoded=PackPreviewDecoder::DecodePreviewAsset(Entry(png),png.data(),png.size());CHECK(decoded.success,decoded.errorReason);
    CHECK(decoded.image.Width()==2&&decoded.image.Height()==3&&decoded.image.StrideBytes()==8,"PNG shape/stride");CHECK(decoded.image.Pixels()==expected,"PNG exact full image including transparent colour");
    CHECK(decoded.image.ColorSpace()==PreviewColorSpace::sRgb&&decoded.image.Format()==PreviewFormat::Png,"Explicit sRGB/RGBA format");
    Bytes caller=png;auto life=PackPreviewDecoder::DecodePreviewBytes(caller.data(),caller.size(),"x.png");caller.assign(caller.size(),0);caller.clear();caller.shrink_to_fit();
    auto copy=life.image;life={};CHECK(copy.Pixels()==expected,"Pixels survive input mutation/destruction and result destruction");auto moved=std::move(copy);CHECK(moved.Pixels()==expected,"Move preserves privately owned pixels");
    auto stored=Read(fixture/"stored_2x3.png");auto st=PackPreviewDecoder::DecodePreviewBytes(stored.data(),stored.size(),"x.png");CHECK(st.success&&st.image.Pixels()==expected,"Stored DEFLATE full decode");
    auto dyn=Read(fixture/"dynamic.png"),dynpixels=Read(fixture/"dynamic.rgba");auto dy=PackPreviewDecoder::DecodePreviewBytes(dyn.data(),dyn.size(),"x.png");CHECK(dy.success&&dy.image.Pixels()==dynpixels,"Dynamic Huffman full pixels");
    Group("All eight PNG EXIF orientations with independent pixel permutations");
    for(int o=1;o<=8;++o){auto b=Read(fixture/("orientation_"+std::to_string(o)+".png"));auto r=PackPreviewDecoder::DecodePreviewBytes(b.data(),b.size(),"x.png");CHECK(r.success,r.errorReason);CHECK(r.image.Orientation()==o,"Source orientation preserved");CHECK(r.image.Width()==(o>=5?3u:2u)&&r.image.Height()==(o>=5?2u:3u),"Oriented dimensions");
        Bytes want;for(int i:order[o-1])want.insert(want.end(),expected.begin()+i*4,expected.begin()+i*4+4);CHECK(r.image.Pixels()==want,"Exact oriented RGBA pixels");}
    Group("BMP exact RGB/opaque alpha and explicit JPEG unsupported contract");
    auto b=PackPreviewDecoder::DecodePreviewBytes(bmp.data(),bmp.size(),"x.bmp");Bytes opaque=expected;for(size_t i=3;i<opaque.size();i+=4)opaque[i]=255;CHECK(b.success,b.errorReason);CHECK(b.image.Pixels()==opaque,"Complete BGR row-order and opaque alpha");
    auto j=PackPreviewDecoder::DecodePreviewBytes(jpg.data(),jpg.size(),"x.jpg");CHECK(!j.success&&!j.image.IsValid()&&j.errorReason.find("unsupported")!=std::string::npos,"JPEG explicitly unsupported, never silently recovered");
    Group("Targeted JPEG corruption must not be silently recovered");
    auto trailingEntropy=jpg;trailingEntropy.insert(trailingEntropy.end()-2,0);
    auto tr=PackPreviewDecoder::DecodePreviewBytes(trailingEntropy.data(),trailingEntropy.size(),"x.jpg");
    std::cout<<"JPEG trailing entropy accepted="<<tr.success<<" reason="<<tr.errorReason<<"\n";
    CHECK(!tr.success,"JPEG trailing compressed data must fail");
    auto damagedEntropy=jpg;
    for(size_t i=2;i+4<damagedEntropy.size();++i)if(damagedEntropy[i]==255&&damagedEntropy[i+1]==218){size_t offset=i+2+((size_t(damagedEntropy[i+2])<<8)|damagedEntropy[i+3]);if(offset+2<damagedEntropy.size())damagedEntropy[offset]=0;break;}
    auto de=PackPreviewDecoder::DecodePreviewBytes(damagedEntropy.data(),damagedEntropy.size(),"x.jpg");
    std::cout<<"JPEG damaged entropy accepted="<<de.success<<" reason="<<de.errorReason<<"\n";
    CHECK(!de.success,"JPEG damaged compressed data must fail");
    auto damagedHuffman=jpg;
    for(size_t i=2;i+22<damagedHuffman.size();++i)if(damagedHuffman[i]==255&&damagedHuffman[i+1]==196){damagedHuffman[i+5]=255;break;}
    auto dh=PackPreviewDecoder::DecodePreviewBytes(damagedHuffman.data(),damagedHuffman.size(),"x.jpg");
    std::cout<<"JPEG damaged Huffman accepted="<<dh.success<<" reason="<<dh.errorReason<<"\n";
    CHECK(!dh.success,"JPEG damaged Huffman table must fail");
    Group("All JPEG variants/orientations are explicitly unsupported for v1");
    for(int o=1;o<=8;++o){auto v=Read(fixture/("jpeg_orientation_"+std::to_string(o)+".jpg"));auto r=PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.jpg");CHECK(!r.success&&!r.image.IsValid()&&r.errorReason.find("unsupported")!=std::string::npos,"JPEG orientation not silently accepted");}
    Group("Typed contract rejects before invalid-pointer hash/decode or oversized allocations");
    auto entry=Entry(png);entry.assetType=PackAssetType::CubeLut;CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Wrong asset type");
    for(const auto& path:{"../x.png","C:/x.png","x.png:stream","a//x.png","a/./x.png","a/NUL.png","x.exe","x.png "}){entry=Entry(png,path);CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Unsafe typed path");}
    entry=Entry(png,std::string("\xff.png",5));CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Invalid UTF8 descriptor fails without throwing");
    entry=Entry(png);entry.sha256Hex.assign(64,'0');CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Wrong SHA");entry.sha256Hex="zz";CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Malformed SHA");
    entry=Entry(png);++entry.sizeBytes;CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,png.data(),png.size()).success,"Wrong declared size");
    entry.sizeBytes=PackPreviewDecoder::kMaxEncodedSizeBytes+1;CHECK(!PackPreviewDecoder::DecodePreviewAsset(entry,reinterpret_cast<const uint8_t*>(1),size_t(entry.sizeBytes)).success,"Oversized fails before touching inaccessible bytes");
    CHECK(!PackPreviewDecoder::DecodePreviewBytes(reinterpret_cast<const uint8_t*>(1),PackPreviewDecoder::kMaxEncodedSizeBytes+1,"x.png").success,"Byte API bound before memory access");
    CHECK(!PackPreviewDecoder::DecodePreviewBytes(png.data(),png.size(),"x.jpg").success,"Extension/magic mismatch");CHECK(!PackPreviewDecoder::DecodePreviewBytes(nullptr,1,"x.png").success,"Null bytes");
    int width=123,height=123;std::string error="old";CHECK(!PackValidator::ValidatePreviewImageBytes(nullptr,0,"x.png",width,height,error)&&width==0&&height==0&&!error.empty(),"Failure clears output");
    CHECK(PackValidator::ValidatePreviewImageBytes(png.data(),png.size(),"x.png",width,height,error)&&error.empty(),"Success clears stale error");
    Group("Every byte-boundary truncation and complete-file trailer rejection");
    for(const auto& pair:std::vector<std::pair<Bytes,std::string>>{{png,"x.png"},{bmp,"x.bmp"}}){for(size_t n=0;n<pair.first.size();++n)CHECK(!PackPreviewDecoder::DecodePreviewBytes(pair.first.data(),n,pair.second).success,"All truncated prefixes fail");auto trailing=pair.first;trailing.push_back(0);CHECK(!PackPreviewDecoder::DecodePreviewBytes(trailing.data(),trailing.size(),pair.second).success,"Trailing byte fails");}
    CHECK(!PackPreviewDecoder::DecodePreviewBytes(png.data(),32,"x.png").success,"Old32-byte fakePNG is rejected");
    Group("CRC-valid compressed corruption, colour profiles, animation and orientation failures");
    for(const auto& f:{"damaged_deflate.png","bad_adler.png","trailing_zlib.png","invalid_filter.png","icc.png","gamma.png","apng.png","bad_orientation.png","unknown_critical.png"}){auto v=Read(fixture/f);auto r=PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.png");CHECK(!r.success,f);CHECK(!r.image.IsValid(),"Failure publishes no image");}
    auto crc=png;crc[40]^=1;CHECK(!PackPreviewDecoder::DecodePreviewBytes(crc.data(),crc.size(),"x.png").success,"CRC mismatch");
    Group("Overflow dimensions, invalid PNG colour/header and BMP layout/mask/palette cases");
    for(uint32_t dim:{0u,4097u,0xffffffffu}){auto v=png;Be(v,16,dim);FixIhdr(v);CHECK(!PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.png").success,"CRC-valid dimension bomb");}
    for(const auto index:{24,25,26,27,28}){auto v=png;v[index]=255;FixIhdr(v);CHECK(!PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.png").success,"Illegal PNG IHDR field");}
    for(const auto index:{10,14,18,22,26,28,30,34,46,50}){auto v=bmp;v[index]^=255;CHECK(!PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.bmp").success,"Invalid BMP structural field");}
    auto minimum=bmp;minimum[22]=minimum[23]=minimum[24]=0;minimum[25]=128;CHECK(!PackPreviewDecoder::DecodePreviewBytes(minimum.data(),minimum.size(),"x.bmp").success,"INT_MIN height fails without signed overflow");
    Group("Checked allocation/conversion failures publish no image");
    for(auto fault:{PreviewDecodeFault::PixelAllocation,PreviewDecodeFault::PixelConversion}) {
        auto result=PackPreviewDecoder::DecodePreviewBytesForTesting(png.data(),png.size(),"x.png",fault);
        CHECK(!result.success&&!result.image.IsValid()&&!result.errorReason.empty(),"Checked failure clears output and reports error");
    }
    Group("Explicit raw64 contract and both repository samples");
    auto r=PackPreviewDecoder::DecodePreviewAsset(Entry(raw,"preview.raw"),raw.data(),raw.size());CHECK(r.success&&r.image.Width()==64&&r.image.Height()==64&&r.image.Pixels()==raw,"Exact raw64 owned pixels");
    for(size_t size:{size_t(4),size_t(16),size_t(16383),size_t(16385)}){Bytes v(size);CHECK(!PackPreviewDecoder::DecodePreviewBytes(v.data(),v.size(),"x.raw").success,"Arbitrary raw shape rejected");}
    for(const auto& sample:{"pack_warm_studio","pack_cool_broadcast"}){PackManifest manifest;std::string e;const auto path=samples/sample;CHECK(ManifestJson::ReadFromFile((path/"manifest.json").string(),manifest,e),"Required repository sample exists/parses");auto result=PackValidator::ValidateDirectoryPack(path.string(),manifest);CHECK(result.valid,result.errorReason);}
    Group("Production typed directory validation rejects forged complete header-only preview");
    fs::path owned=fs::current_path()/"preview-owned-test";if(fs::exists(owned))throw std::runtime_error("Test-owned path must be fresh");fs::create_directories(owned);
    PackManifest manifest;manifest.packId="com.prismforge.previewtest";manifest.version="1.0.0";manifest.displayName="Preview fixture";manifest.previewImagePath="preview.png";manifest.assets={Entry(png)};Write(owned/"preview.png",png);
    CHECK(PackValidator::ValidateDirectoryPack(owned.string(),manifest).valid,"Production accepts complete exact image");Bytes fake(png.begin(),png.begin()+32);Write(owned/"preview.png",fake);manifest.assets={Entry(fake)};CHECK(!PackValidator::ValidateDirectoryPack(owned.string(),manifest).valid,"Production rejects correctly hashed header fragment");
    Write(owned/"preview.png",png);manifest.assets={Entry(png)};manifest.assets[0].sha256Hex.assign(64,'0');CHECK(!PackValidator::ValidateDirectoryPack(owned.string(),manifest).valid,"Production typed hash check");
    Group("Drive-absolute managed roots and original reparse aliases");
    std::string e;CHECK(PackValidator::ValidateLibraryRoot(owned.string(),e),e);CHECK(PackValidator::ValidateOwnedDescendant(owned,owned/"new"/"file.json",e),e);
    for(const auto& path:{"relative","D:relative","/rooted","\\rooted","\\\\server\\share","\\\\?\\D:\\any","D:\\x\\..\\y","D:\\x\\NUL","D:\\x:ads"})CHECK(!PackValidator::ValidateLibraryRoot(path,e),"Invalid managed root rejected");
    CHECK(!PackValidator::ValidateOwnedDescendant(owned,owned,e),"Equal root fails");CHECK(!PackValidator::ValidateOwnedDescendant(owned,fs::path(owned.string()+"-sibling")/"x",e),"Prefix sibling fails");
    const auto unicode=owned/fs::u8path(u8"效果-प्रिज़्म");fs::create_directory(unicode);Write(unicode/"preview.png",png);
    manifest.assets={Entry(png)};CHECK(PackValidator::ValidateLibraryRoot(unicode.u8string(),e),"UTF8 managed root");
    CHECK(PackValidator::ValidateDirectoryPack(unicode.u8string(),manifest).valid,"UTF8 directory preview acceptance");
    CHECK(PackValidator::ValidateOwnedDescendant(owned,unicode/"preview.png",e),"Unicode owned descendant");
    fs::create_directory(owned/"real");const auto alias=owned/"alias";bool linked=Junction(alias,owned/"real");CHECK(linked,"Actual junction creation required for ownership regression");
    if(linked){CHECK(!PackValidator::ValidateLibraryRoot(alias.string(),e),"Original root junction rejected");CHECK(!PackValidator::ValidateOwnedDescendant(owned,alias/"new.json",e),"Alias into same root rejected before canonicalization");}
    CHECK(RemoveDirectoryW(alias.c_str())!=0,"Remove only test-owned junction/empty alias");
    Group("Bounded streaming non-preview hashing rejects truncation, extra bytes and growth");
    const auto hashed=owned/"bounded.cube";Bytes content(16384,71);Write(hashed,content);uint64_t bytesRead=999;
    CHECK(Sha256::ComputeFileHexBounded(hashed.u8string(),content.size(),content.size(),e,&bytesRead)==Sha256::ComputeHex(content.data(),content.size())&&bytesRead==content.size()&&e.empty(),"Exact bounded hash matches independent memory digest");
    CHECK(Sha256::ComputeFileHexBounded(hashed.u8string(),content.size()+1,content.size()+1,e,&bytesRead).empty()&&bytesRead==content.size()&&!e.empty(),"Truncated bounded hash fails");
    CHECK(Sha256::ComputeFileHexBounded(hashed.u8string(),1024,content.size(),e,&bytesRead).empty()&&bytesRead==1025&&!e.empty(),"Extra byte rejected after expected plus one byte");
    CHECK(Sha256::ComputeFileHexBounded("invalid-unopened-path",16385,16384,e,&bytesRead).empty()&&bytesRead==0&&e.find("limit")!=std::string::npos,"Explicit cap rejected before open/read");
    Write(hashed,{});CHECK(Sha256::ComputeFileHexBounded(hashed.u8string(),0,0,e,&bytesRead)==Sha256::ComputeHex(nullptr,0)&&bytesRead==0,"Exact empty hash compatibility");
    Write(hashed,content);std::atomic<bool> stopWriter{false};std::thread grower([&]{std::ofstream append(hashed,std::ios::binary|std::ios::app);for(int i=0;i<128&&!stopWriter;++i){append.write(reinterpret_cast<const char*>(content.data()),content.size());append.flush();}});
    auto growthHash=Sha256::ComputeFileHexBounded(hashed.u8string(),1024,content.size(),e,&bytesRead);stopWriter=true;grower.join();
    CHECK(growthHash.empty()&&bytesRead<=1025&&!e.empty(),"Concurrent append cannot extend bounded hash work beyond expected plus one");
    Write(hashed,content);PackAssetEntry payload;payload.assetType=PackAssetType::CubeLut;payload.assetId="bounded";payload.relativePath="bounded.cube";payload.sizeBytes=content.size();payload.sha256Hex=Sha256::ComputeHex(content.data(),content.size());
    manifest.assets={Entry(png),payload};Write(owned/"preview.png",png);CHECK(PackValidator::ValidateDirectoryPack(owned.u8string(),manifest).valid,"Production validates non-preview through bounded hash");
    content.push_back(0);Write(hashed,content);CHECK(!PackValidator::ValidateDirectoryPack(owned.u8string(),manifest).valid,"Production rejects extra payload byte");
    fs::remove_all(owned);
    std::cout<<"RESULT groups="<<groups<<" assertions="<<assertions<<" failures="<<failures<<" skips="<<skips<<"\n";return failures?1:0;
    }catch(const std::exception& e){std::cerr<<"FATAL "<<e.what()<<"\n";return 2;}
}
