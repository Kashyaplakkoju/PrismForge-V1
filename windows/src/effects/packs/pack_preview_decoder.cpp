#include "pack_preview_decoder.h"
#include "pack_validator.h"
#include "sha256.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
namespace PrismForge::Effects::Packs {
namespace {
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<uint8_t>;
uint16_t Be16(const uint8_t* p){return uint16_t((uint16_t(p[0])<<8)|p[1]);}
uint32_t Be32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
uint16_t Le16(const uint8_t* p){return uint16_t(p[0]|(uint16_t(p[1])<<8));}
uint32_t Le32(const uint8_t* p){return p[0]|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool Dimensions(uint32_t w,uint32_t h){return w&&h&&w<=4096&&h<=4096&&uint64_t(w)*h<=PackPreviewDecoder::kMaxPixelCount;}
std::string Lower(std::string v){for(auto& c:v)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return v;}
// Table-free CRC has no shared mutable first-call state (parallel decodes are safe).
uint32_t Crc(const uint8_t* p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(int b=0;b<8;++b)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
struct Header {PreviewFormat format=PreviewFormat::Unknown;uint32_t w=0,h=0,orientation=1;};
std::string Exif(const uint8_t* p,size_t n,uint32_t& orientation){
    if(n<8)return "Truncated EXIF TIFF header";
    const bool le=p[0]=='I'&&p[1]=='I',be=p[0]=='M'&&p[1]=='M';
    if(!le&&!be)return "Unsupported EXIF byte order";
    auto r16=[&](const uint8_t* q){return le?Le16(q):Be16(q);};
    auto r32=[&](const uint8_t* q){return le?Le32(q):Be32(q);};
    if(r16(p+2)!=42)return "Invalid EXIF TIFF magic";
    uint32_t off=r32(p+4); if(off<8||off>n-2)return "Invalid EXIF IFD offset";
    uint16_t count=r16(p+off);if(count>1024||uint64_t(off)+2+uint64_t(count)*12+4>n)return "Truncated or oversized EXIF IFD";
    bool seen=false;
    for(uint32_t i=0;i<count;++i){const auto e=p+off+2+i*12;uint16_t tag=r16(e),type=r16(e+2);uint32_t len=r32(e+4);
        if(tag==274){if(seen||type!=3||len!=1)return "Invalid or duplicate EXIF orientation";seen=true;orientation=r16(e+8);if(orientation<1||orientation>8)return "Unsupported EXIF orientation";}
        if(tag==34675)return "Embedded EXIF ICC profile unsupported";
        if(tag==34665){uint32_t sub=r32(e+8);if(type!=4||len!=1||sub>n-2)return "Invalid EXIF colour IFD";
            uint16_t subcount=r16(p+sub);if(subcount>1024||uint64_t(sub)+2+uint64_t(subcount)*12+4>n)return "Truncated EXIF colour IFD";
            for(uint32_t j=0;j<subcount;++j){const auto s=p+sub+2+j*12;if(r16(s)==40961&&(r16(s+2)!=3||r32(s+4)!=1||r16(s+8)!=1))return "Non-sRGB EXIF colour space unsupported";}}
    }
    if(r32(p+off+2+count*12)!=0)return "Multipage EXIF TIFF unsupported";
    return {};
}
// Independently check the complete PNG zlib/DEFLATE stream. WIC codecs may be
// permissive about damaged trailers, so successful CopyPixels alone is insufficient.
struct BitReader {
    const Bytes& b;size_t bit=0;bool ok=true;
    uint32_t Read(unsigned n){if(n>24||bit+n>b.size()*8){ok=false;return 0;}uint32_t v=0;for(unsigned i=0;i<n;++i)v|=uint32_t((b[bit/8]>>(bit%8))&1)<<i,++bit;return v;}
    void Align(){bit=(bit+7)&~size_t(7);if(bit>b.size()*8)ok=false;}
};
struct Huffman {
    std::array<uint16_t,16> count{};std::array<uint16_t,320> symbol{};bool valid=false;
    explicit Huffman(const std::vector<unsigned>& lengths){
        for(auto l:lengths){if(l>15)return;++count[l];}if(count[0]==lengths.size())return;
        int left=1;for(int l=1;l<=15;++l){left=left*2-count[l];if(left<0)return;}
        // RFC1951 permits an incomplete single-symbol tree, never an arbitrary one.
        if(left>0&&(lengths.size()-count[0]!=1||count[1]!=1))return;
        std::array<uint16_t,16> next{};for(int l=1;l<15;++l)next[l+1]=next[l]+count[l];
        for(unsigned i=0;i<lengths.size();++i)if(lengths[i])symbol[next[lengths[i]]++]=uint16_t(i);valid=true;
    }
    int Decode(BitReader& r)const{if(!valid)return -1;uint32_t code=0,first=0,index=0;
        for(unsigned len=1;len<=15;++len){code|=r.Read(1);if(!r.ok)return -1;if(code>=first&&code-first<count[len])return symbol[index+code-first];index+=count[len];first=(first+count[len])<<1;code<<=1;}return -1;}
};
std::string Inflate(const Bytes& compressed,size_t expected,size_t scanline){
    if(compressed.size()<6)return "Truncated PNG zlib stream";
    if((compressed[0]&15)!=8||(compressed[0]>>4)>7||((unsigned(compressed[0])*256+compressed[1])%31)||compressed[1]&32)return "Invalid PNG zlib header";
    Bytes body(compressed.begin()+2,compressed.end()-4),out;out.reserve(expected);
    BitReader r{body};bool last=false;
    static constexpr unsigned lbase[]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr unsigned lextra[]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static constexpr unsigned dbase[]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static constexpr unsigned dextra[]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    while(!last&&r.ok){last=r.Read(1)!=0;unsigned kind=r.Read(2);if(kind==3)return "Invalid PNG DEFLATE block type";
        if(kind==0){r.Align();uint32_t len=r.Read(16),inv=r.Read(16);if(!r.ok||(len^inv)!=65535||len>expected-out.size())return "Invalid PNG stored block";
            while(len--){out.push_back(uint8_t(r.Read(8)));if(!r.ok)return "Truncated PNG stored data";}continue;}
        std::vector<unsigned> ll,dd;
        if(kind==1){ll.resize(288);dd.resize(32,5);for(int i=0;i<288;++i)ll[i]=i<144?8:i<256?9:i<280?7:8;}
        else {unsigned nl=r.Read(5)+257,nd=r.Read(5)+1,nc=r.Read(4)+4;if(!r.ok||nl>286||nd>32)return "Invalid PNG Huffman lengths";
            // RFC1951 alphabet order, explicit to avoid any mutable lookup tables.
            static constexpr unsigned clorder[]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<unsigned> cl(19);for(unsigned i=0;i<nc;++i)cl[clorder[i]]=r.Read(3);Huffman h(cl);if(!h.valid)return "Invalid PNG code-length tree";
            std::vector<unsigned> lengths;while(lengths.size()<nl+nd){int s=h.Decode(r);if(s<0)return "Corrupt PNG code-length codes";
                if(s<=15)lengths.push_back(unsigned(s));else {unsigned repeat=0,v=0;if(s==16){if(lengths.empty())return "Invalid PNG length repeat";v=lengths.back();repeat=r.Read(2)+3;}else if(s==17)repeat=r.Read(3)+3;else if(s==18)repeat=r.Read(7)+11;else return "Invalid PNG length symbol";
                    if(!r.ok||repeat>nl+nd-lengths.size())return "PNG length repeat overflow";lengths.insert(lengths.end(),repeat,v);}}
            ll.assign(lengths.begin(),lengths.begin()+nl);dd.assign(lengths.begin()+nl,lengths.end());}
        if(ll.size()<=256||!ll[256])return "PNG DEFLATE missing end symbol";Huffman lh(ll),dh(dd);if(!lh.valid)return "Invalid PNG literal tree";
        for(;;){int s=lh.Decode(r);if(s<0)return "Corrupt PNG DEFLATE literal";if(s==256)break;
            if(s<256){if(out.size()==expected)return "PNG decompressed data too large";out.push_back(uint8_t(s));continue;}
            if(s>285)return "Reserved PNG length symbol";unsigned i=unsigned(s)-257,len=lbase[i]+r.Read(lextra[i]);int d=dh.Decode(r);if(d<0||d>29)return "Invalid PNG distance symbol";
            unsigned dist=dbase[d]+r.Read(dextra[d]);if(!r.ok||dist>(1u<<((compressed[0]>>4)+8))||dist>out.size()||len>expected-out.size())return "PNG DEFLATE distance/size invalid";while(len--)out.push_back(out[out.size()-dist]);}
    }
    if(!r.ok||!last||(r.bit+7)/8!=body.size()||out.size()!=expected)return "PNG compressed stream is truncated or has trailing data";
    uint32_t a=1,b=0;for(auto v:out){a=(a+v)%65521;b=(b+a)%65521;}if((b<<16|a)!=Be32(compressed.data()+compressed.size()-4))return "PNG Adler checksum mismatch";
    for(size_t off=0;off<out.size();off+=scanline)if(out[off]>4)return "Invalid PNG row filter";return {};
}
std::string Png(const uint8_t* p,size_t n,Header& h){
    const uint8_t sig[]={137,80,78,71,13,10,26,10};if(n<8||memcmp(p,sig,8))return "PNG signature mismatch";
    unsigned paletteEntries=0;bool ihdr=false,plte=false,idat=false,ended=false,closedIdat=false,trns=false,exif=false,srgb=false,gamma=false,chrom=false;
    unsigned depth=0,colour=0;size_t row=0;Bytes compressed;
    for(size_t off=8;off<n;){if(n-off<12)return "Truncated PNG chunk";uint32_t len=Be32(p+off);if(uint64_t(len)+12>n-off)return "PNG chunk exceeds input";const auto t=p+off+4,d=p+off+8;
        for(int i=0;i<4;++i)if(!((t[i]>='A'&&t[i]<='Z')||(t[i]>='a'&&t[i]<='z')))return "Invalid PNG chunk type";
        if(t[2]&32)return "Invalid PNG reserved chunk bit";
        if(Crc(t,size_t(len)+4)!=Be32(d+len))return "PNG chunk CRC mismatch";
        std::string name(reinterpret_cast<const char*>(t),4);if(!ihdr&&name!="IHDR")return "PNG IHDR must be first";
        if(name=="IHDR"){if(ihdr||len!=13)return "Invalid or duplicate PNG IHDR";ihdr=true;h.w=Be32(d);h.h=Be32(d+4);depth=d[8];colour=d[9];
            if(!Dimensions(h.w,h.h)||d[10]||d[11]||d[12])return "Unsupported PNG dimensions/compression/filter/interlace";
            bool legal=(colour==0&&(depth==1||depth==2||depth==4||depth==8||depth==16))||(colour==2&&(depth==8||depth==16))||(colour==3&&(depth==1||depth==2||depth==4||depth==8))||((colour==4||colour==6)&&(depth==8||depth==16));if(!legal)return "Illegal PNG colour/depth combination";
            unsigned channels=colour==0||colour==3?1:colour==2?3:colour==4?2:4;row=(size_t(h.w)*channels*depth+7)/8+1;
            if(row*size_t(h.h)>PackPreviewDecoder::kMaxDecodedSizeBytes+4096)return "PNG decompressed scanlines exceed limit";}
        else if(name=="PLTE"){if(plte||trns||idat||len==0||len%3||len>768||colour==0||colour==4||(colour==3&&len/3>(1u<<depth)))return "Invalid PNG palette";plte=true;paletteEntries=len/3;}
        else if(name=="tRNS"){if(trns||idat||(colour==0&&len!=2)||(colour==2&&len!=6)||(colour==3&&(!plte||len==0||len>paletteEntries))||(colour!=0&&colour!=2&&colour!=3))return "Invalid PNG transparency";trns=true;}
        else if(name=="IDAT"){if(closedIdat||(colour==3&&!plte))return "Invalid PNG IDAT order";idat=true;if(len>PackPreviewDecoder::kMaxEncodedSizeBytes-compressed.size())return "PNG compressed data exceeds limit";compressed.insert(compressed.end(),d,d+len);}
        else {if(idat)closedIdat=true;
            if(name=="IEND"){if(len||!idat||off+12!=n)return "Invalid PNG IEND or trailing bytes";ended=true;}
            else if(name=="acTL"||name=="fcTL"||name=="fdAT")return "Animated PNG unsupported";
            else if(name=="iCCP"||name=="cICP"||name=="mDCv"||name=="cLLi")return "Embedded PNG ICC/HDR profiles unsupported";
            else if(name=="sRGB"){if(srgb||idat||plte||len!=1||d[0]>3)return "Invalid PNG sRGB metadata";srgb=true;}
            else if(name=="gAMA"){if(gamma||idat||plte||len!=4||Be32(d)!=45455)return "Unsupported PNG gamma";gamma=true;}
            else if(name=="cHRM"){static constexpr uint32_t standard[]={31270,32900,64000,33000,30000,60000,15000,6000};if(chrom||idat||plte||len!=32)return "Invalid PNG chromaticity";for(int i=0;i<8;++i)if(Be32(d+i*4)!=standard[i])return "Non-sRGB PNG chromaticity unsupported";chrom=true;}
            else if(name=="eXIf"){if(exif||idat)return "Invalid PNG EXIF order";exif=true;auto e=Exif(d,len,h.orientation);if(!e.empty())return e;}
            else if(!(t[0]&32))return "Unknown PNG critical chunk";
        }
        off+=size_t(len)+12;
    }
    if(!ihdr||!idat||!ended||compressed.empty())return "Incomplete PNG image";
    return Inflate(compressed,row*size_t(h.h),row);
}
std::string Bmp(const uint8_t* p,size_t n,Header& h){
    if(n<54||p[0]!='B'||p[1]!='M')return "BMP signature/header mismatch";
    if(Le32(p+2)!=n||Le32(p+6)!=0||Le32(p+14)!=40)return "Unsupported BMP file/DIB header";
    int32_t w=int32_t(Le32(p+18)),height=int32_t(Le32(p+22));if(w<=0||height==0||height==std::numeric_limits<int32_t>::min())return "Invalid BMP dimensions";
    h.w=uint32_t(w);h.h=uint32_t(height<0?-int64_t(height):height);if(!Dimensions(h.w,h.h))return "BMP dimensions exceed limit";
    unsigned bits=Le16(p+28);if(Le16(p+26)!=1||(bits!=24&&bits!=32)||Le32(p+30)!=0||Le32(p+46)!=0||Le32(p+50)!=0)return "Unsupported BMP planes/palette/compression";
    uint64_t row=((uint64_t(h.w)*bits+31)/32)*4,total=row*h.h;
    if(Le32(p+10)!=54||54+total!=n||(Le32(p+34)!=0&&Le32(p+34)!=total))return "Invalid BMP offset/rows/size/trailing bytes";return {};
}
struct ComScope {HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}bool Valid()const{return SUCCEEDED(hr)||hr==RPC_E_CHANGED_MODE;}};
PackPreviewDecodeResult Decode(const uint8_t* data,size_t size,const std::string& name,PreviewDecodeFault fault){
    using R=PackPreviewDecodeResult;
    if(!data||!size||size>PackPreviewDecoder::kMaxEncodedSizeBytes)return R::Failure("Preview encoded size must be1..10MiB");
    const auto ext=Lower(std::filesystem::u8path(name).extension().string());Header h;std::string err;
    if(ext==".raw"){
        if(size!=64*64*4)return R::Failure("Raw preview must be exactly64x64 RGBA8 (16384 bytes)");
        return R::Success(DecodedPreviewImage(64,64,PreviewFormat::RawRgba,PreviewColorSpace::sRgb,1,Bytes(data,data+size)));}
    if(ext==".png"){h.format=PreviewFormat::Png;err=Png(data,size,h);}else if(ext==".jpg"||ext==".jpeg"){return R::Failure("JPEG previews unsupported in v1: use PNG, BMP or raw64 (WIC recovers corrupt entropy)");}else if(ext==".bmp"){h.format=PreviewFormat::Bmp;err=Bmp(data,size,h);}else return R::Failure("Unsupported preview extension");
    if(!err.empty())return R::Failure(err);
    ComScope com;if(!com.Valid())return R::Failure("WIC COM initialization failed");
    ComPtr<IWICImagingFactory> factory;HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));if(FAILED(hr))return R::Failure("WIC factory creation failed");
    ComPtr<IWICStream> stream;hr=factory->CreateStream(&stream);if(FAILED(hr))return R::Failure("WIC stream creation failed");
    // WIC's memory stream has a non-const API. Give it a private bounded copy;
    // the codec cannot mutate the caller's verified immutable source bytes.
    Bytes owned(data,data+size);hr=stream->InitializeFromMemory(owned.data(),DWORD(owned.size()));if(FAILED(hr))return R::Failure("WIC memory stream initialization failed");
    ComPtr<IWICBitmapDecoder> decoder;hr=factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder);if(FAILED(hr))return R::Failure("WIC image decoding failed");
    GUID format{};hr=decoder->GetContainerFormat(&format);GUID expected=h.format==PreviewFormat::Png?GUID_ContainerFormatPng:GUID_ContainerFormatBmp;
    if(FAILED(hr)||format!=expected)return R::Failure("WIC container mismatch");
    UINT count=0;hr=decoder->GetFrameCount(&count);if(FAILED(hr)||count!=1)return R::Failure("Preview must have exactly one frame");
    ComPtr<IWICBitmapFrameDecode> frame;hr=decoder->GetFrame(0,&frame);if(FAILED(hr))return R::Failure("WIC frame retrieval failed");
    UINT w=0,height=0;hr=frame->GetSize(&w,&height);if(FAILED(hr)||w!=h.w||height!=h.h||!Dimensions(w,height))return R::Failure("WIC dimensions disagree with validated header");
    UINT contexts=0;hr=frame->GetColorContexts(0,nullptr,&contexts);if(FAILED(hr)&&hr!=WINCODEC_ERR_UNSUPPORTEDOPERATION)return R::Failure("WIC colour context query failed");
    if(SUCCEEDED(hr)&&contexts){if(contexts>4)return R::Failure("Too many WIC colour contexts");
        std::vector<ComPtr<IWICColorContext>> owners(contexts);std::vector<IWICColorContext*> ptrs;for(auto& owner:owners){hr=factory->CreateColorContext(&owner);if(FAILED(hr))return R::Failure("WIC colour context creation failed");ptrs.push_back(owner.Get());}
        UINT got=0;hr=frame->GetColorContexts(contexts,ptrs.data(),&got);if(FAILED(hr)||got!=contexts)return R::Failure("WIC colour context retrieval failed");
        for(auto& owner:owners){WICColorContextType type;hr=owner->GetType(&type);if(FAILED(hr)||type!=WICColorContextExifColorSpace)return R::Failure("Embedded colour profiles unsupported");UINT space=0;hr=owner->GetExifColorSpace(&space);if(FAILED(hr)||space!=1)return R::Failure("Non-sRGB colour space unsupported");}}
    ComPtr<IWICFormatConverter> converter;hr=factory->CreateFormatConverter(&converter);if(FAILED(hr))return R::Failure("WIC converter creation failed");
    WICPixelFormatGUID input{};BOOL can=FALSE;hr=frame->GetPixelFormat(&input);if(FAILED(hr))return R::Failure("WIC pixel format query failed");hr=converter->CanConvert(input,GUID_WICPixelFormat32bppRGBA,&can);if(FAILED(hr)||!can)return R::Failure("WIC straight RGBA conversion unsupported");
    hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);if(FAILED(hr))return R::Failure("WIC straight RGBA initialization failed");
    size_t stride=size_t(w)*4,total=stride*height;if(total>PackPreviewDecoder::kMaxDecodedSizeBytes||total>UINT_MAX)return R::Failure("Decoded preview exceeds64MiB");if(fault==PreviewDecodeFault::PixelAllocation)throw std::bad_alloc();Bytes pixels(total);
    hr=fault==PreviewDecodeFault::PixelConversion?E_FAIL:converter->CopyPixels(nullptr,UINT(stride),UINT(total),pixels.data());if(FAILED(hr))return R::Failure("WIC complete pixel decode failed");
    if(h.format==PreviewFormat::Bmp)for(size_t i=3;i<pixels.size();i+=4)pixels[i]=255;
    const bool transpose=h.orientation>=5;uint32_t fw=transpose?height:w,fh=transpose?w:height;
    if(h.orientation!=1){Bytes oriented(total);for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<w;++x){uint32_t dx=x,dy=y;
        switch(h.orientation){case 2:dx=w-1-x;break;case 3:dx=w-1-x;dy=height-1-y;break;case 4:dy=height-1-y;break;case 5:dx=y;dy=x;break;case 6:dx=height-1-y;dy=x;break;case 7:dx=height-1-y;dy=w-1-x;break;case 8:dx=y;dy=w-1-x;break;}
        memcpy(oriented.data()+(size_t(dy)*fw+dx)*4,pixels.data()+(size_t(y)*w+x)*4,4);}pixels=std::move(oriented);}
    return R::Success(DecodedPreviewImage(fw,fh,h.format,PreviewColorSpace::sRgb,h.orientation,std::move(pixels)));
}
}
PackPreviewDecodeResult PackPreviewDecoder::DecodePreviewAsset(const PackAssetEntry& asset,const uint8_t* data,size_t size){
    try {
    if(asset.assetType!=PackAssetType::PreviewImage)return PackPreviewDecodeResult::Failure("Asset type is not PreviewImage");
    if(!data||!size||size>kMaxEncodedSizeBytes||asset.sizeBytes!=size)return PackPreviewDecodeResult::Failure("Preview declared/actual size invalid or exceeds10MiB");
    std::string error;if(!PackValidator::ValidateRelativePath(asset.relativePath,asset.assetType,error))return PackPreviewDecodeResult::Failure(error);
    if(asset.sha256Hex.size()!=64||!std::all_of(asset.sha256Hex.begin(),asset.sha256Hex.end(),[](unsigned char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}))return PackPreviewDecodeResult::Failure("Invalid declared preview SHA256");
    const auto extension=Lower(std::filesystem::u8path(asset.relativePath).extension().string());
    if(extension==".jpg"||extension==".jpeg")return PackPreviewDecodeResult::Failure("JPEG previews unsupported in v1: use PNG, BMP or raw64");
    if(Lower(Sha256::ComputeHex(data,size))!=Lower(asset.sha256Hex))return PackPreviewDecodeResult::Failure("Preview SHA256 mismatch");
    return DecodePreviewBytes(data,size,asset.relativePath);
    }catch(const std::bad_alloc&){return PackPreviewDecodeResult::Failure("Preview asset allocation failed");}
    catch(const std::exception& e){return PackPreviewDecodeResult::Failure(std::string("Invalid preview asset: ")+e.what());}
}
PackPreviewDecodeResult PackPreviewDecoder::DecodePreviewBytes(const uint8_t* data,size_t size,const std::string& name){
    return DecodePreviewBytesForTesting(data,size,name,PreviewDecodeFault::None);
}
PackPreviewDecodeResult PackPreviewDecoder::DecodePreviewBytesForTesting(const uint8_t* data,size_t size,const std::string& name,PreviewDecodeFault fault){
    try{return Decode(data,size,name,fault);}catch(const std::bad_alloc&){return PackPreviewDecodeResult::Failure("Preview allocation failed");}catch(const std::exception& e){return PackPreviewDecodeResult::Failure(std::string("Preview decode failed: ")+e.what());}}
bool PackPreviewDecoder::ValidateAndGetDimensions(const uint8_t* p,size_t n,const std::string& path,int& w,int& h,std::string& e){
    w=h=0;e.clear();auto decoded=DecodePreviewBytes(p,n,path);if(!decoded.success){e=decoded.errorReason;return false;}w=int(decoded.image.Width());h=int(decoded.image.Height());return true;}
}
