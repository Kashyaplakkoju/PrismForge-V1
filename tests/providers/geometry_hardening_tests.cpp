#include "providers/frame_request.h"
#include "providers/frame_preprocessor.h"
#include "providers/model_manifest.h"
#include "providers/result_adapter.h"
#include <iostream>
#include <vector>
#include <limits>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

using namespace PrismForge::Providers;
using PrismForge::Effects::AnalysisBridge::MaskBuffer;
int checks = 0, failures = 0, groups = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "[FAIL] line " << __LINE__ << ": " #x "\n"; } } while (0)
bool Near(float a, float b, float epsilon = 1e-5f) { return std::abs(a - b) <= epsilon; }
auto Frame(int w, int h, FrameOrientation orientation = FrameOrientation::Normal, bool mirror = false, ColorRange range = ColorRange::Full) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
    for (int i = 0; i < w * h; ++i) { pixels[i * 3] = static_cast<uint8_t>((i + 1) * 10); pixels[i * 3 + 1] = 80; pixels[i * 3 + 2] = 160; }
    return FrameRequest::CreateFromPixels(7, 11, 1234, w, h, w * 3, PixelFormat::RGB24, std::move(pixels), range, ColorMatrix::Identity, orientation, mirror);
}
void Group(const char* name) { ++groups; std::cout << "[GROUP] " << name << "\n"; }

void Rotations() {
    Group("independent asymmetric clockwise rotation and oriented mirror, both tensor layouts");
    const std::vector<std::vector<int>> expected = {{10,20,30,40,50,60}, {40,10,50,20,60,30}, {60,50,40,30,20,10}, {30,60,20,50,10,40}};
    FramePreprocessor prep;
    for (int rotation = 0; rotation < 4; ++rotation) for (bool mirror : {false, true}) for (auto layout : {TensorLayout::NCHW, TensorLayout::NHWC}) {
        auto frame = Frame(3,2,static_cast<FrameOrientation>(rotation),mirror);
        int w = rotation % 2 ? 2 : 3, h = rotation % 2 ? 3 : 2;
        std::vector<float> tensor; GeometryTransform t;
        CHECK(prep.Preprocess(*frame,w,h,layout,{},AspectMode::Stretch,tensor,t));
        CHECK(t.orientedWidth == w && t.orientedHeight == h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            size_t index = static_cast<size_t>(y) * w + x;
            int value = expected[rotation][y * w + (mirror ? w-1-x : x)];
            CHECK(Near(tensor[layout == TensorLayout::NCHW ? index : index * 3], value/255.f));
            CHECK(Near(tensor[layout == TensorLayout::NCHW ? 6+index : index*3+1],80/255.f));
        }
        std::vector<float> mask(6);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            mask[y*w+x] = expected[rotation][y*w+(mirror ? w-1-x:x)]/100.f;
        MaskBuffer output; CHECK(prep.MapMaskBack(mask.data(),w,h,t,output));
        for (int i = 0; i < 6; ++i) CHECK(Near(output.data[i],(i+1)/10.f));
    }
}
void ResolutionAndPadding() {
    Group("rectangular output resolution changes and center-aligned sampling with complete edges");
    FramePreprocessor prep; auto frame = Frame(6,4); std::vector<float> tensor; GeometryTransform t;
    CHECK(prep.Preprocess(*frame,6,4,TensorLayout::NCHW,{},AspectMode::Stretch,tensor,t));
    float low[] = {0,.2f,.4f,.6f,.8f,1}; MaskBuffer out;
    CHECK(prep.MapMaskBack(low,3,2,t,out));
    CHECK(Near(out.data[0],0)); CHECK(Near(out.data[5],.4f)); CHECK(Near(out.data[18],.6f)); CHECK(Near(out.data[23],1));
    CHECK(Near(out.data[7],.2f));
    frame = Frame(3,2); CHECK(prep.Preprocess(*frame,3,2,TensorLayout::NHWC,{},AspectMode::Stretch,tensor,t));
    std::vector<float> high(24); for (int y=0;y<4;++y) for (int x=0;x<6;++x) high[y*6+x]=x*.1f+y*.03f;
    CHECK(prep.MapMaskBack(high.data(),6,4,t,out)); CHECK(Near(out.data[0],.065f)); CHECK(Near(out.data[5],.525f));
    float one[] = {.7f}; CHECK(prep.MapMaskBack(one,1,1,t,out)); for (float f:out.data) CHECK(Near(f,.7f));
    Group("letterbox ignores poisoned padding and crop masks unseen source pixels");
    frame = Frame(4,2); CHECK(prep.Preprocess(*frame,4,4,TensorLayout::NCHW,{},AspectMode::LetterboxPad,tensor,t));
    std::vector<float> padded(16,1.f); for(int y=1;y<=2;++y) for(int x=0;x<4;++x) padded[y*4+x]=.25f;
    CHECK(prep.MapMaskBack(padded.data(),4,4,t,out)); for(float f:out.data) CHECK(Near(f,.25f));
    // Downsampled active rows have centers at row1 of a3-row output; adjacent padded predictions cannot bleed.
    float lowPad[] = {1,1,.25f,.25f,1,1};
    CHECK(prep.MapMaskBack(lowPad,2,3,t,out)); for(float f:out.data) CHECK(Near(f,.25f));
    frame = Frame(6,2); CHECK(prep.Preprocess(*frame,2,2,TensorLayout::NCHW,{},AspectMode::CropToFill,tensor,t));
    float crop[] = {.6f,.6f,.6f,.6f}; CHECK(prep.MapMaskBack(crop,2,2,t,out));
    for(int y=0;y<2;++y) for(int x=0;x<6;++x) CHECK(Near(out.data[y*6+x],x>=2&&x<4?.6f:0));
    Group("invalid transforms and bounded allocation rejection leave no partial output");
    auto bad=t; bad.scaleX=0; CHECK(!prep.MapMaskBack(crop,2,2,bad,out)); CHECK(!out.IsValid());
    bad=t; bad.orientation=static_cast<FrameOrientation>(99); CHECK(!prep.MapMaskBack(crop,2,2,bad,out));
    CHECK(!prep.MapMaskBack(crop,2,2,t,out,8192,8192)); CHECK(!out.IsValid());
    float nan[] = {0,0,0,std::numeric_limits<float>::quiet_NaN()}; CHECK(!prep.MapMaskBack(nan,2,2,t,out));
    CHECK(!prep.Preprocess(*frame,8192,8192,TensorLayout::NCHW,{},AspectMode::Stretch,tensor,t)); CHECK(tensor.empty()); CHECK(t.srcWidth==0);
    auto norm=NormalizationConfig{}; norm.std[1]=-1; CHECK(!prep.Preprocess(*frame,2,2,TensorLayout::NCHW,norm,AspectMode::Stretch,tensor,t));
    norm={}; norm.mean.pop_back(); CHECK(!prep.Preprocess(*frame,2,2,TensorLayout::NCHW,norm,AspectMode::Stretch,tensor,t));
    CHECK(!prep.Preprocess(*frame,2,2,static_cast<TensorLayout>(99),{},AspectMode::Stretch,tensor,t));
    CHECK(!prep.Preprocess(*frame,2,2,TensorLayout::NCHW,{},static_cast<AspectMode>(99),tensor,t));
    Group("one-pixel source and padded BGR/RGBA packed channels avoid poisoned row storage");
    for (int orientation=0;orientation<4;++orientation) {
        auto oneFrame=Frame(1,1,static_cast<FrameOrientation>(orientation),true);
        CHECK(prep.Preprocess(*oneFrame,1,1,TensorLayout::NHWC,{},AspectMode::LetterboxPad,tensor,t));
        CHECK(Near(tensor[0],10/255.f));CHECK(prep.MapMaskBack(one,1,1,t,out));CHECK(out.data.size()==1&&Near(out.data[0],.7f));
    }
    for (auto format : {PixelFormat::BGR24, PixelFormat::BGRA32, PixelFormat::RGB24, PixelFormat::RGBA32}) {
        const int bpp=static_cast<int>(FrameRequest::GetBytesPerPixel(format));const int stride=2*bpp+5;
        std::vector<uint8_t> pixels(stride*2,255);
        bool bgr=format==PixelFormat::BGR24||format==PixelFormat::BGRA32;
        for(int y=0;y<2;++y)for(int x=0;x<2;++x){auto p=pixels.data()+y*stride+x*bpp;p[bgr?2:0]=static_cast<uint8_t>(10+y*20+x*10);p[1]=50;p[bgr?0:2]=90;}
        auto paddedFrame=FrameRequest::CreateFromPixels(1,2,3,2,2,stride,format,std::move(pixels));
        CHECK(prep.Preprocess(*paddedFrame,2,2,TensorLayout::NHWC,{},AspectMode::Stretch,tensor,t));
        for(int i=0;i<4;++i){CHECK(Near(tensor[i*3],(10+i*10)/255.f));CHECK(Near(tensor[i*3+1],50/255.f));CHECK(Near(tensor[i*3+2],90/255.f));}
    }
}
auto MakeAV(AVPixelFormat format, int w=5, int h=3) {
    AVFrame* raw=av_frame_alloc(); CHECK(raw!=nullptr);
    auto frame=std::unique_ptr<AVFrame,void(*)(AVFrame*)>(raw,[](AVFrame* f){av_frame_free(&f);});
    raw->width=w;raw->height=h;raw->format=format;CHECK(av_frame_get_buffer(raw,32)==0);return frame;
}
void OwnedPlanesAndColor() {
    Group("odd padded software AVFrame601/709 full/limited conversion uses actual full RGB semantics");
    for(auto matrix:{AVCOL_SPC_BT709,AVCOL_SPC_SMPTE170M}) for(auto range:{AVCOL_RANGE_MPEG,AVCOL_RANGE_JPEG}) {
        auto f=MakeAV(AV_PIX_FMT_YUV420P); f->colorspace=matrix; f->color_range=range;
        for(int y=0;y<3;++y) std::memset(f->data[0]+y*f->linesize[0],81,5);
        for(int y=0;y<2;++y) {std::memset(f->data[1]+y*f->linesize[1],90,3);std::memset(f->data[2]+y*f->linesize[2],240,3);}
        auto req=FrameRequest::CreateFromAVFrame(1,2,3,f.get()); CHECK(req && req->IsValid());
        if(!req)continue;
        CHECK(req->colorRange==ColorRange::Full && req->colorMatrix==ColorMatrix::Identity && req->strideBytes==15);
        double Y=range==AVCOL_RANGE_MPEG?(81-16)*255./219:81;
        double U=(90-128)*(range==AVCOL_RANGE_MPEG?255./224:1.);
        double V=(240-128)*(range==AVCOL_RANGE_MPEG?255./224:1.);
        double R=Y+(matrix==AVCOL_SPC_BT709?1.5748:1.402)*V;
        double G=Y-(matrix==AVCOL_SPC_BT709?.187324:.344136)*U-(matrix==AVCOL_SPC_BT709?.468124:.714136)*V;
        double B=Y+(matrix==AVCOL_SPC_BT709?1.8556:1.772)*U;
        double oracle[]={R,G,B};
        for(int i=0;i<15;++i) for(int c=0;c<3;++c) CHECK(std::abs(static_cast<int>((*req->pixelData)[i*3+c])-std::clamp(oracle[c],0.,255.))<=3.0);
    }
    Group("packed RGB limited range expands explicitly; malformed or unowned AVFrame planes rejected");
    auto f=MakeAV(AV_PIX_FMT_RGB24);f->color_range=AVCOL_RANGE_MPEG;
    for(int y=0;y<3;++y) for(int x=0;x<5;++x) {auto p=f->data[0]+y*f->linesize[0]+x*3;p[0]=16;p[1]=235;p[2]=126;}
    auto req=FrameRequest::CreateFromAVFrame(1,2,3,f.get()); CHECK(req!=nullptr); if(req){CHECK((*req->pixelData)[0]==0);CHECK((*req->pixelData)[1]==255);CHECK((*req->pixelData)[2]==128);}
    FramePreprocessor prep;std::vector<float> tensor;GeometryTransform t;
    auto packed=FrameRequest::CreateFromPixels(1,2,3,1,1,3,PixelFormat::RGB24,{16,235,126},ColorRange::Limited);
    CHECK(prep.Preprocess(*packed,1,1,TensorLayout::NHWC,{},AspectMode::Stretch,tensor,t));CHECK(tensor[0]==0&&tensor[1]==1&&Near(tensor[2],128/255.f));
    auto yuv=MakeAV(AV_PIX_FMT_YUV420P);
    int saved=yuv->linesize[1];yuv->linesize[1]=2;CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,yuv.get()));yuv->linesize[1]=saved;
    uint8_t* plane=yuv->data[2];yuv->data[2]=nullptr;CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,yuv.get()));yuv->data[2]=plane;
    auto size=yuv->buf[0]->size;yuv->buf[0]->size=1;CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,yuv.get()));yuv->buf[0]->size=size;
    saved=yuv->linesize[0];yuv->linesize[0]=-saved;CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,yuv.get()));yuv->linesize[0]=saved;
    yuv->colorspace=AVCOL_SPC_BT2020_NCL;CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,yuv.get()));
    auto unowned=av_frame_alloc();std::vector<uint8_t> memory(16);unowned->width=2;unowned->height=2;unowned->format=AV_PIX_FMT_RGB24;unowned->linesize[0]=6;unowned->data[0]=memory.data();
    CHECK(!FrameRequest::CreateFromAVFrame(1,2,3,unowned));av_frame_free(&unowned);
    FrameRequest invalid=*packed;invalid.strideBytes=-1;CHECK(!invalid.IsValid());invalid=*packed;invalid.orientation=static_cast<FrameOrientation>(4);CHECK(!invalid.IsValid());
    // Count actual retained storage, not just a tiny logical size on a previously huge vector.
    std::vector<uint8_t> excessiveCapacity={1,2,3};excessiveCapacity.reserve(FrameRequest::kMaxOwnedBytes+1);
    CHECK(!FrameRequest::CreateFromPixels(1,2,3,1,1,3,PixelFormat::RGB24,std::move(excessiveCapacity)));
}
void Interpretation() {
    Group("independent NHWC output class axes, configurable person softmax and extreme logits");
    auto frame=Frame(3,2);FramePreprocessor prep;GeometryTransform t;std::vector<float> input;
    CHECK(prep.Preprocess(*frame,3,2,TensorLayout::NCHW,{},AspectMode::Stretch,input,t));
    ModelManifest m;m.inputDims={1,3,2,3};m.aspectMode=AspectMode::Stretch;m.outputDims={1,2,3,2};m.outputLayout=TensorLayout::NHWC;
    m.outputInterpretation=ModelOutputInterpretation::SoftmaxTwoClass;m.personClassIndex=0;
    float large=std::numeric_limits<float>::max();
    std::vector<float> output={large,-large,-large,large,7,7,0,0,-1000,1000,1000,-1000};
    ResultAdapter adapter;std::shared_ptr<PrismForge::Effects::AnalysisBridge::AnalysisBundle> bundle;
    CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));float expected[]={1,0,.5f,.5f,0,1};
    if(bundle) for(int i=0;i<6;++i)CHECK(Near(bundle->personMask.data[i],expected[i]));
    m.personClassIndex=1;CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));if(bundle)for(int i=0;i<6;++i)CHECK(Near(bundle->personMask.data[i],1-expected[i]));
    m.outputInterpretation=ModelOutputInterpretation::LogitsSigmoid; output={0,-large,0,large,0,0,0,-1000,0,1000,0,1};
    CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));float sigmoid[]={0,1,.5f,0,1,.7310586f};if(bundle)for(int i=0;i<6;++i)CHECK(Near(bundle->personMask.data[i],sigmoid[i]));
    Group("multiclass ArgMax deterministic ties, thresholding and strict malformed probability rejection");
    m.outputDims={1,2,3,3};m.outputInterpretation=ModelOutputInterpretation::ArgMax;m.personClassIndex=2;
    output={0,1,2,2,1,0,7,7,7,1,1,2,-1000,1000,0,1,2,3};
    CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));float winner[]={1,0,0,1,0,1};if(bundle)for(int i=0;i<6;++i)CHECK(bundle->personMask.data[i]==winner[i]);
    // Same class data in planar layout produces identical independently expected labels.
    std::vector<float> planar(18);for(int i=0;i<6;++i)for(int c=0;c<3;++c)planar[c*6+i]=output[i*3+c];
    m.outputLayout=TensorLayout::NCHW;m.outputDims={1,3,2,3};CHECK(adapter.AdaptResult(planar,m,t,*frame,bundle));if(bundle)for(int i=0;i<6;++i)CHECK(bundle->personMask.data[i]==winner[i]);
    m.outputLayout=TensorLayout::NHWC;m.outputDims={1,2,3,2};m.outputInterpretation=ModelOutputInterpretation::RawProbability;m.personClassIndex=1;
    output={.9f,.1f,.8f,.2f,.7f,.3f,.6f,.4f,.5f,.5f,.4f,.6f};m.retainSoftProbability=false;m.probabilityThreshold=.5f;
    CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));if(bundle)for(int i=0;i<6;++i)CHECK(bundle->personMask.data[i]==(i>=4?1:0));
    output[0]=-0.001f;CHECK(!adapter.AdaptResult(output,m,t,*frame,bundle));CHECK(!bundle);
    output[0]=std::numeric_limits<float>::infinity();CHECK(!adapter.AdaptResult(output,m,t,*frame,bundle));CHECK(!bundle);
    m.outputDims={1,1,2,1};m.personClassIndex=0;output={0,1};
    CHECK(adapter.AdaptResult(output,m,t,*frame,bundle));if(bundle)for(int y=0;y<2;++y)for(int x=0;x<3;++x)CHECK(bundle->personMask.data[y*3+x]==(x>=1?1:0));
    m.outputDims={1,2,3,2};m.personClassIndex=1;
    m.producesNormalMap=true;CHECK(!m.Validate());m.producesNormalMap=false;m.personClassIndex=2;CHECK(!m.Validate());
    m.personClassIndex=1;m.outputDims={2,2,3,2};CHECK(!m.Validate());m.outputDims={1,2,3,2};m.inputDims={1,3,2};CHECK(!m.Validate());
}
void ManifestParsing() {
    Group("explicit checked manifest parsing rejects partial schema, duplicates, wrong types and malformed numbers");
    const std::string valid=R"({"modelName":"external-test-graph","modelVersion":"2","inputTensorName":"x","inputDims":[1,2,3,3],"inputLayout":"NHWC","outputTensorName":"y","outputDims":[1,2,2,3],"outputLayout":"NCHW","outputInterpretation":"SoftmaxTwoClass","personClassIndex":1,"mean":[0.1,0.2,0.3],"std":[1,2,3],"scaleTo01":false,"aspectMode":"CropToFill","probabilityThreshold":0.75,"retainSoftProbability":false})";
    auto path=std::filesystem::temp_directory_path()/"prismforge-geometry-manifest-test.json";
    auto parse=[&](const std::string& text,ModelManifest& out){std::ofstream f(path,std::ios::binary|std::ios::trunc);f<<text;f.close();return ModelManifest::LoadFromFile(path.wstring(),out);};
    ModelManifest m;CHECK(parse(valid,m));CHECK(m.inputLayout==TensorLayout::NHWC&&m.outputLayout==TensorLayout::NCHW&&m.inputDims[1]==2&&m.outputDims[1]==2);
    CHECK(m.personClassIndex==1&&m.normConfig.std[2]==3&&m.normConfig.mean[1]==.2f&&m.aspectMode==AspectMode::CropToFill);
    const auto oldName=m.modelName;
    CHECK(!parse("{}",m));CHECK(m.modelName==oldName);
    CHECK(!parse(valid+" trailing",m));CHECK(!parse(valid.substr(0,valid.size()-1)+",\"modelName\":\"duplicate\"}",m));
    auto replace=[&](std::string from,std::string to){auto s=valid;auto p=s.find(from);CHECK(p!=std::string::npos);s.replace(p,from.size(),to);return s;};
    CHECK(!parse(replace("\"personClassIndex\":1","\"personClassIndex\":1.5"),m));
    CHECK(!parse(replace("\"NHWC\"","\"BROKEN\""),m));
    CHECK(!parse(replace("\"std\":[1,2,3]","\"std\":[1,0,3]"),m));
    CHECK(!parse(replace("\"mean\":[0.1,0.2,0.3]","\"mean\":[0.1,NaN,0.3]"),m));
    CHECK(!parse(replace("\"scaleTo01\":false","\"scaleTo01\":\"false\""),m));
    CHECK(!parse(replace("[1,2,3,3]","[01,2,3,3]"),m));
    CHECK(!parse(valid.substr(0,valid.size()-1)+",\"producesSkinMask\":true}",m));
    CHECK(!parse(valid.substr(0,valid.size()-1)+",\"unknownContract\":true}",m));
    CHECK(!parse(replace("[1,2,3,3]","[1,8192,8192,3]"),m));
    CHECK(!parse(replace("\"external-test-graph\"","\"bad\\u0000name\""),m));
    CHECK(!parse(std::string(65537,' '),m));
    std::error_code ec;std::filesystem::remove(path,ec);
}
int main(){Rotations();ResolutionAndPadding();OwnedPlanesAndColor();Interpretation();ManifestParsing();
    std::cout<<"[RESULT] geometry groups="<<groups<<" assertions="<<checks<<" failures="<<failures<<"\n";return failures?1:0;}
