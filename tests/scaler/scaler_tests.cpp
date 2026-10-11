#include "video/directshowscaler.h"
#include "video/guipreviewscaler.h"
#include "video/1_1scaler.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
static int assertions=0, groups=0;
void Check(bool value,const char* message) { ++assertions; if(!value) throw std::runtime_error(message); }
struct Image {
 AVFrame* frame=av_frame_alloc();
 Image(int w,int h,int format=AV_PIX_FMT_BGR24,int padding=0){
  Check(frame!=nullptr,"allocate descriptor");frame->width=w;frame->height=h;frame->format=format;
  if(padding){frame->linesize[0]=w*3+padding;frame->buf[0]=av_buffer_alloc(size_t(frame->linesize[0])*h+64);Check(frame->buf[0]!=nullptr,"allocate padded owner");frame->data[0]=frame->buf[0]->data;}
  else Check(av_frame_get_buffer(frame,64)>=0,"allocate pixels");
  for(auto* owner:frame->buf)if(owner)std::memset(owner->data,0xcd,owner->size);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=frame->data[0]+size_t(y)*frame->linesize[0]+3*x;p[0]=uint8_t(x*13+y*7);p[1]=uint8_t(x*3+y*11);p[2]=uint8_t(x*17+y*5);}
 }
 ~Image(){av_frame_free(&frame);}
};
void CompareRows(const uint8_t* bytes,const AVFrame* input,int width,int height){
 Check(bytes!=nullptr,"output exists");Check(width==input->width&&height==input->height,"matching dimensions");
 for(int y=0;y<height;++y)Check(std::memcmp(bytes+size_t(y)*width*3,input->data[0]+size_t(y)*input->linesize[0],size_t(width)*3)==0,"exact active row pixels");
}
void Test(){
 {Image input(1920,1080);Video::DirectShowScaler scaler(1920,1080);int w,h;auto* p=scaler.Process(input.frame,w,h);CompareRows(p,input.frame,w,h);Check(p==input.frame->data[0],"bounded packed input is borrowed without a copy");++groups;}
 {Image input(13,9,AV_PIX_FMT_BGR24,17);Video::DirectShowScaler scaler(13,9);int w,h;auto* p=scaler.Process(input.frame,w,h);CompareRows(p,input.frame,w,h);Check(p!=input.frame->data[0],"padded rows become tight output");++groups;}
 {Image input(32,18,AV_PIX_FMT_RGB24);Video::_1_1Scaler scaler(AV_PIX_FMT_BGR24);int w,h;auto* p=scaler.Process(input.frame,w,h);Check(p&&w==32&&h==18,"RGB conversion exists");for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* a=input.frame->data[0]+size_t(y)*input.frame->linesize[0]+x*3;auto* b=p+(size_t(y)*w+x)*3;Check(b[0]==a[2]&&b[1]==a[1]&&b[2]==a[0],"RGB BGR channels convert");}++groups;}
 {Image input(32,18);Video::DirectShowScaler scaler(32,32);int w,h;auto* p=scaler.Process(input.frame,w,h);Check(p&&w==32&&h==32,"letterbox output dimensions");for(int y=0;y<h;++y)for(int x=0;x<w*3;++x){auto expected=(y>=7&&y<25)?input.frame->data[0]+size_t(y-7)*input.frame->linesize[0]:nullptr;Check(p[size_t(y)*w*3+x]==(expected?expected[x]:0),"independent letterbox pixels");}++groups;}
 {Image input(64,48);Video::DirectShowScaler scaler(32,32);int w,h;auto* p=scaler.Process(input.frame,w,h);Check(p&&w==32&&h==32,"scaled output");std::vector<uint8_t> expected(size_t(w)*h*3+64,0);auto* ctx=sws_getContext(64,48,AV_PIX_FMT_BGR24,32,24,AV_PIX_FMT_BGR24,SWS_BILINEAR,nullptr,nullptr,nullptr);Check(ctx!=nullptr,"independent FFmpeg conversion oracle");uint8_t* d[4]{expected.data()+size_t(4)*32*3};int stride[4]{32*3};Check(sws_scale(ctx,input.frame->data,input.frame->linesize,0,48,d,stride)==24,"oracle scaled rows");sws_freeContext(ctx);Check(std::memcmp(p,expected.data(),size_t(w)*h*3)==0,"scaled output matches independent conversion");++groups;}
 {Image input(32,18);Video::DirectShowScaler scaler(32,18);int w=99,h=99;auto oldStride=input.frame->linesize[0];input.frame->linesize[0]=95;Check(!scaler.Process(input.frame,w,h)&&w==0&&h==0,"short stride rejected");input.frame->linesize[0]=oldStride;auto* oldData=input.frame->data[0];input.frame->data[0]=oldData+input.frame->buf[0]->size-1;Check(!scaler.Process(input.frame,w,h),"outside owner extent rejected");input.frame->data[0]=oldData;input.frame->width=std::numeric_limits<int>::max();Check(!scaler.Process(input.frame,w,h),"dimensions checked before byte multiplication");input.frame->width=32;input.frame->linesize[0]=-96;Check(!scaler.Process(input.frame,w,h),"negative stride rejected");AVFrame raw{};raw.width=32;raw.height=18;raw.format=AV_PIX_FMT_BGR24;raw.data[0]=oldData;raw.linesize[0]=96;auto* output=scaler.Process(&raw,w,h);Check(output&&output!=oldData,"unowned raw input uses synchronous copy");++groups;}
 {Image a(64,36),b(13,9,AV_PIX_FMT_BGR24,1);Video::DirectShowScaler scaler(64,36);int w,h;Check(scaler.Process(a.frame,w,h)==a.frame->data[0],"first size borrowed");scaler=Video::DirectShowScaler(13,9);auto* converted=scaler.Process(b.frame,w,h);CompareRows(converted,b.frame,w,h);scaler=Video::DirectShowScaler(64,36);Check(scaler.Process(a.frame,w,h)==a.frame->data[0],"resize/move returns correct current pixels");++groups;}
 {Image input(64,36);Video::GuiPreviewScaler scaler(64,36);int w,h;auto* p=scaler.Process(input.frame,w,h);Check(p==input.frame->data[0],"preview packed same-size fast path");AVFrame* retained=av_frame_clone(input.frame);Check(retained!=nullptr,"retained input clone");Check(av_frame_make_writable(input.frame)>=0,"current input copy on write");input.frame->data[0][0]^=0x7f;Check(retained->data[0][0]!=input.frame->data[0][0]&&p==retained->data[0],"borrowed pixels retained safely by input clone");av_frame_free(&retained);++groups;}
 {Image input(12,40);Video::DirectShowScaler scaler(53,37);int w,h;auto* p=scaler.Process(input.frame,w,h);Check(p&&w==53&&h==37,"odd narrow fit dimensions");for(int y=0;y<37;++y)for(int x=0;x<53;++x)if(x<21||x>=32)for(int c=0;c<3;++c)Check(p[(size_t(y)*53+x)*3+c]==0,"SIMD fit preserves black bars");++groups;}
 std::cout<<"SCALER_TESTS "<<groups<<"/9 assertions="<<assertions<<"\n";
}
void Benchmark(){
 Image input(1920,1080);Video::DirectShowScaler scaler(1920,1080);constexpr int warm=24,samples=240;std::vector<double> durations;int w=0,h=0;
 for(int i=0;i<warm+samples;++i){input.frame->data[0][0]=uint8_t(i);auto before=std::chrono::steady_clock::now();auto* output=scaler.Process(input.frame,w,h);auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();if(i>=warm)durations.push_back(ms);CompareRows(output,input.frame,w,h);}
 std::sort(durations.begin(),durations.end());std::cout<<std::fixed<<std::setprecision(6)<<"SCALER_BENCH exact_Bgr1080p warm="<<warm<<" samples="<<samples<<" p50_ms="<<durations[119]<<" p95_ms="<<durations[227]<<" every_frame_pixels=PASS\n";
}
int main(int argc,char** argv){try{if(argc>1&&std::string(argv[1])=="--benchmark")Benchmark();else Test();return 0;}catch(const std::exception& ex){std::cerr<<"FAIL "<<ex.what()<<" assertions="<<assertions<<" groups="<<groups<<"\n";return 1;}}
