#include "effects/overlays/face_accessory_3d.h"
#include "effects/studiostack/gpu/d3d11_face_accessories.h"
#include "effects/gpu/d3d11_context.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/dict.h>
}
namespace E=PrismForge::Effects;
namespace R=E::Overlays::Realistic;
namespace A=E::AnalysisBridge;
namespace G=E::StudioStack::Gpu;
using Microsoft::WRL::ComPtr;
namespace {
int assertions=0,groups=0;
std::filesystem::path assets,scratch;
void Need(bool ok,const std::string& message){++assertions;if(!ok)throw std::runtime_error(message);}
struct FreeFrame{void operator()(AVFrame* f)const{av_frame_free(&f);}};
using Frame=std::unique_ptr<AVFrame,FreeFrame>;
Frame Input(){Frame f(av_frame_alloc());Need(bool(f),"owned input descriptor");f->format=AV_PIX_FMT_BGR24;f->width=240;f->height=180;Need(av_frame_get_buffer(f.get(),32)>=0,"owned input pixels");
 for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x){auto* p=f->data[0]+size_t(y)*f->linesize[0]+x*3;p[0]=uint8_t(40+x/3);p[1]=uint8_t(60+y/3);p[2]=120;}
 f->pts=100000;f->time_base={1,1000000};f->duration=16667;Need(av_dict_set(&f->metadata,"original","retained",0)>=0,"source metadata");return f;}
std::vector<uint8_t> Bytes(const AVFrame* f){Need(f&&f->data[0],"valid pixels");std::vector<uint8_t> bytes(size_t(f->width)*f->height*3);for(int y=0;y<f->height;++y)std::memcpy(bytes.data()+size_t(y)*f->width*3,f->data[0]+size_t(y)*f->linesize[0],size_t(f->width)*3);return bytes;}
A::FaceLandmarkBundle Geometry(float yaw=0){A::FaceLandmarkBundle b;b.detected=true;b.evidence=A::FaceLandmarkEvidence::ValidatedNativeGeometry;b.geometryRevision=1;b.geometrySourceWidth=240;b.geometrySourceHeight=180;b.geometrySourceGeneration=7;b.geometrySequenceId=1;b.geometryTimestampPts=100000;
 b.leftEye={.40f,.5f,0};b.rightEye={.60f,.5f,0};b.noseBridge={.5f,.49f,0};b.foreheadCenter={.5f,.28f,0};
 auto g=std::make_shared<A::FaceAccessoryGeometry>();g->sourceWidth=240;g->sourceHeight=180;g->sourceGeneration=7;g->sequenceId=1;g->timestampPts=100000;g->hasTransform=true;g->transform={};g->transform[0]=g->transform[10]=std::cos(yaw);g->transform[2]=std::sin(yaw);g->transform[8]=-std::sin(yaw);g->transform[5]=g->transform[15]=1;
 g->vertices.assign(478,{.5f,.5f,.08f});g->vertices[468]={.40f,.5f,0};g->vertices[473]={.60f,.5f,0};g->vertices[168]={.5f,.49f,0};g->vertices[10]={.5f,.28f,0};
 g->vertices[200]={.30f,.32f,.08f};g->vertices[201]={.70f,.32f,.08f};g->vertices[202]={.70f,.9f,.08f};g->vertices[203]={.30f,.9f,.08f};g->triangles={{{200,201,202}},{{200,202,203}}};b.accessoryMesh=g;Need(b.NativeGeometryMatches(240,180,7,1,100000),"exact synthetic geometry");return b;}
A::FaceLandmarkBundle PosedGeometry(float faceScale=1,float yaw=0,float pitch=0){
 auto g=Geometry();auto mesh=std::make_shared<A::FaceAccessoryGeometry>(*g.accessoryMesh);g.accessoryMesh=mesh;
 for(auto& p:mesh->vertices){p.x=.5f+(p.x-.5f)*faceScale;p.y=.5f+(p.y-.5f)*faceScale;p.z*=faceScale;}
 for(const auto& pair:std::array<std::array<int,2>,4>{{{{54,284}},{{103,332}},{{127,356}},{{234,454}}}}){mesh->vertices[pair[0]]={.5f-.24f*faceScale,.5f-.07f*faceScale,0};mesh->vertices[pair[1]]={.5f+.24f*faceScale,.5f-.07f*faceScale,0};}
 const float f=180/(2.f*std::tan(3.14159265358979323846f/8.f)),distance=f*6.3f/(48.f*faceScale),cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
 mesh->transform={cy,sy*sp,sy*cp,0, 0,cp,-sp,0, -sy,cy*sp,cy*cp,0, 0,0,0,1};
 // Rotate the actual 3D landmark points and independently project them back
 // into the native normalized image convention, not just the pose matrix.
 for(auto& p:mesh->vertices){const float d=distance+p.z*240*distance/f,x=(p.x-.5f)*240*d/f,y=(.5f-p.y)*180*d/f,z=-d+distance;
  const float px=cy*x+sy*sp*y+sy*cp*z,py=cp*y-sp*z,pz=-sy*x+cy*sp*y+cy*cp*z,newDistance=distance-pz;
  Need(newDistance>1,"physically posed native landmark remains in front of camera");p.x=.5f+px*f/(240*newDistance);p.y=.5f-py*f/(180*newDistance);p.z=(newDistance-distance)*f/(240*distance);}
 const auto& a=mesh->vertices[468];const auto& b=mesh->vertices[473];const auto& c=mesh->vertices[168];const auto& d=mesh->vertices[10];
 g.leftEye={a.x,a.y,0};g.rightEye={b.x,b.y,0};g.noseBridge={c.x,c.y,0};g.foreheadCenter={d.x,d.y,0};
 Need(g.NativeGeometryMatches(240,180,7,1,100000)&&mesh->Matches(240,180,7,1,100000),"current physically posed image landmarks and row-major matrix valid");return g;
}
void LE(std::ofstream& f,uint32_t value){for(int n=0;n<4;++n)f.put(char(value>>(n*8)));}
void File(const std::filesystem::path& path,uint32_t w,uint32_t h,const std::vector<uint8_t>& pixels,const char* magic="PFC1",int tail=0){std::ofstream f(path,std::ios::binary);f.write(magic,4);LE(f,w);LE(f,h);f.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());for(int i=0;i<tail;++i)f.put(0);f.close();Need(bool(f),"fixture write complete");}
void AssetLoading(){std::string error;Need(R::SetCapAssetRootForTesting(assets.wstring()),"explicit absolute real asset root");auto white=R::LoadCapTexture(1,error),pokemon=R::LoadCapTexture(2,error);Need(white&&white->width==1024&&white->height==1024&&white->pixels.size()==4194304,"actual white1024 decoded texture");Need(pokemon&&pokemon->width==2048&&pokemon->height==2048&&pokemon->pixels.size()==16777216,"actual Pokemon2048 decoded texture at exact16MiB bound");Need(R::LoadCapTexture(1,error)==white,"immutable texture cache identity");Need(!R::LoadCapTexture(0,error)&&!R::LoadCapTexture(3,error),"invalid textured style rejected");Need(!R::SetCapAssetRootForTesting(L"relative"),"relative test root never changes production lookup");
 const auto good=white->pixels;for(int v=0;v<8;++v){const auto root=scratch/(L"bad-"+std::to_wstring(v));std::filesystem::create_directories(root);const auto path=root/L"white_tennis_cap.pfrgba";
  if(v==1)File(path,0,2,{});if(v==2)File(path,2049,1,{});if(v==3)File(path,2,2,{1,2,3,4});if(v==4)File(path,1,1,{1,2,3,255},"BAD1");if(v==5)File(path,1,1,{1,2,3,255},"PFC1",1);if(v==6){std::filesystem::create_directory(path);}if(v==7){std::ofstream f(path,std::ios::binary);f.write("PFC1",4);}
  Need(R::SetCapAssetRootForTesting(root.wstring()),"malformed root reset");Need(!R::LoadCapTexture(1,error)&&!error.empty(),"missing/truncated/header/dimension/trailing/directory texture rejected");}
 Need(white->pixels==good,"held snapshot unaffected by cache reset/malformed files");Need(R::SetCapAssetRootForTesting(assets.wstring()),"real assets restored");}
// Independent double-precision color oracle, separate from production byte LUTs.
double OracleSrgbDecode(uint8_t byte){const double encoded=double(byte)/255.;return encoded<=.04045?encoded/12.92:std::pow((encoded+.055)/1.055,2.4);}
double OracleSrgbEncode(double linear){return linear<=.0031308?12.92*linear:1.055*std::pow(linear,1./2.4)-.055;}
void Sampling(){R::TextureRGBA texture{2,2,{255,0,0,255, 0,255,0,128, 0,0,255,0, 255,255,255,255}};
 const auto red=R::SampleCapTexture(texture,.25f,.25f),green=R::SampleCapTexture(texture,.75f,.25f),blue=R::SampleCapTexture(texture,.25f,.75f),mid=R::SampleCapTexture(texture,.5f,.5f);
 Need(red[0]==1&&red[1]==0&&red[2]==0&&red[3]==1,"row0 upper-left red exact texel");Need(green[1]==1&&std::abs(green[3]-128.f/255)<1e-6f,"row0 upper-right color/alpha exact");Need(blue[2]==1&&blue[3]==0,"row1 orientation preserved");for(int c=0;c<3;++c)Need(std::abs(mid[c]-.5f)<1e-6f,"encoded compatibility midpoint RGB oracle");Need(std::abs(mid[3]-(1+128.f/255+0+1)/4)<1e-6f,"bilinear alpha independent oracle");Need(R::SampleCapTexture(texture,-100,-100)==red,"clamped borders");Need(R::SampleCapTexture(texture,std::nanf(""),0)==std::array<float,4>{},"nonfiniteUV bounded");
 Need(R::SampleCapTextureLinear(texture,.25f,.25f)==red&&R::SampleCapTextureLinear(texture,.75f,.25f)==green&&R::SampleCapTextureLinear(texture,.25f,.75f)==blue,"linear sampler exact source orientation and unencoded alpha");
 Need(R::SampleCapTextureLinear(texture,-100,-100)==red&&R::SampleCapTextureLinear(texture,100,100)==R::SampleCapTextureLinear(texture,.75f,.75f),"linear sampler clamps both borders");
 for(float invalid:{std::nanf(""),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()})Need(R::SampleCapTextureLinear(texture,invalid,0)==std::array<float,4>{}&&R::SampleCapTextureLinear(texture,0,invalid)==std::array<float,4>{},"linear sampler rejects nonfinite coordinates before indexing");
 auto bad=texture;bad.width=2049;Need(R::SampleCapTextureLinear(bad,.5f,.5f)==std::array<float,4>{},"linear sampler rejects oversized extent");bad=texture;bad.pixels.pop_back();Need(R::SampleCapTextureLinear(bad,.5f,.5f)==std::array<float,4>{},"linear sampler rejects truncated pixel owner");bad=texture;bad.height=0;Need(R::SampleCapTextureLinear(bad,.5f,.5f)==std::array<float,4>{},"linear sampler rejects empty dimensions");
 R::TextureRGBA mixed{2,2,{0,8,16,255, 128,64,192,128, 255,255,255,0, 32,128,240,64}};
 const auto linearMix=R::SampleCapTextureLinear(mixed,.375f,.625f),encodedMix=R::SampleCapTexture(mixed,.375f,.625f);const double weights[]={3./16,1./16,9./16,3./16};
 for(int channel=0;channel<3;++channel){double expected=0;for(int texel=0;texel<4;++texel)expected+=weights[texel]*OracleSrgbDecode(mixed.pixels[texel*4+channel]);Need(std::abs(linearMix[channel]-expected)<2e-7,"each mixed source texel decoded before independent bilinear filtering");}
 Need(linearMix[3]==encodedMix[3]&&std::abs(linearMix[3]-(3.+128./255+3.*64./255)/16)<1e-7,"mixed alpha remains original linear interpolation bit-identical");
 const auto half=R::SampleCapTextureLinear(texture,.5f,.5f);for(int channel=0;channel<3;++channel)Need(std::abs(half[channel]-.5f)<1e-7,"black/white RGB interpolation is linear light, not decode after filtering");Need(half[3]==mid[3],"sRGB decode never applies to alpha");
 R::ProjectedScene s;s.width=s.height=s.rasterWidth=s.rasterHeight=4;s.capTexture=std::make_shared<R::TextureRGBA>(texture);s.opaque={{0,0,.5f,1,1,1,1,1,0,0,1},{4,0,.5f,2,1,1,1,1,1,0,1},{0,4,.5f,4,1,1,1,1,0,1,1}};std::vector<float> rgba;
 Need(R::RasterizeProjectedSceneForTesting(s,rgba),"bounded controlled triangle raster");
 // Screen barycentrics at(1.5,1.5) are1/4,3/8,3/8; divide by1,2,4=>8/17,6/17,3/17.
 const double blend=7./34,alpha=(1-blend)+blend*128./255;const size_t at=(1*4+1)*4;
 Need(std::abs(rgba[at+0]-OracleSrgbEncode(1-blend)*alpha)<1e-5&&std::abs(rgba[at+1]-OracleSrgbEncode(blend)*alpha)<1e-5&&rgba[at+2]==0&&std::abs(rgba[at+3]-alpha)<1e-5,"independent perspective UV / linear RGB filtering / sRGB encode / premultiplied linear alpha oracle");
 auto gray=std::make_shared<R::TextureRGBA>(R::TextureRGBA{1,1,{128,128,128,255}});s.capTexture=gray;
 Need(std::abs(R::SampleCapTextureLinear(*gray,.5f,.5f)[0]-.21586050011389926)<2e-7,"known128 sRGB byte decodes to independent linear value");
 for(auto& vertex:s.opaque){vertex.distance=1;vertex.u=vertex.v=.5f;vertex.r=vertex.g=vertex.b=.5f;}
 Need(R::RasterizeProjectedSceneForTesting(s,rgba),"known gray and constant half-diffuse production raster");
 for(int channel=0;channel<3;++channel)Need(std::lround(rgba[at+channel]*255)==92&&std::abs(rgba[at+channel]-.362249142340021)<2e-6,"128gray under half linear light encodes to92byte, not64");Need(rgba[at+3]==1,"opaque alpha unaffected by color transfer");
 gray->pixels[3]=128;Need(R::RasterizeProjectedSceneForTesting(s,rgba),"half-alpha gray production raster");Need(std::abs(rgba[at+3]-128./255)<1e-7,"half alpha remains linear instead of sRGB decoded");for(int channel=0;channel<3;++channel)Need(std::abs(rgba[at+channel]-.362249142340021*128./255)<2e-6,"encoded shaded color premultiplied with linear alpha");
 for(auto& vertex:s.opaque)vertex.textured=0;Need(R::RasterizeProjectedSceneForTesting(s,rgba)&&rgba[at]==.5f&&rgba[at+1]==.5f&&rgba[at+2]==.5f&&rgba[at+3]==1,"nontextured/glasses colors and alpha remain exactly unchanged");
 for(auto& vertex:s.opaque)vertex.textured=1;auto transparent=std::make_shared<R::TextureRGBA>(texture);transparent->pixels.assign(16,0);s.capTexture=transparent;Need(R::RasterizeProjectedSceneForTesting(s,rgba)&&std::all_of(rgba.begin(),rgba.end(),[](float x){return x==0;}),"zeroalpha texture discards without painting");s.rasterWidth=641;Need(!R::RasterizeProjectedSceneForTesting(s,rgba)&&rgba.empty(),"raster seam rejects allocation beyondbound");}
void MeshAndScene(){Need(R::ValidateAccessoryMesh(R::GlassesMesh()),"unchanged actualWayfarer valid");for(int style:{1,2}){const auto& m=R::ReferenceCapMesh(style);Need(m.vertices.size()==size_t(style==1?2024:739)&&m.triangles.size()==size_t(style==1?3712:1080)&&R::ValidateAccessoryMesh(m),"exact exported GLB mesh counts/UV/normals/indices");float lo=100,hi=-100;for(const auto& v:m.vertices){lo=std::min(lo,v.x);hi=std::max(hi,v.x);}Need(std::abs(lo+.55f)<.0001f&&std::abs(hi-.55f)<.0001f,"independent14cmframe-relative cap width1.1");auto g=Geometry();R::ProjectedScene s;std::string error;Need(R::BuildProjectedScene(&g,240,180,true,true,1,s,error,style),error);Need(s.capTexture&&s.opaque.size()>m.triangles.size()*3&&s.rasterWidth<=640&&s.rasterHeight<=640&&s.rasterWidth*s.rasterHeight<=262144,"realmesh textured scene bounded");size_t textured=0;for(const auto& v:s.opaque)if(v.textured){++textured;Need(v.u>=-.01f&&v.u<=1.01f&&v.v>=-.01f&&v.v<=1.01f&&v.r==v.g&&v.g==v.b,"sourceUV unchanged; fabric shaded without navy tint");}Need(textured==m.triangles.size()*3,"every realcap triangle carries true textureUV");auto yaw=Geometry(.4f);R::ProjectedScene turn;Need(R::BuildProjectedScene(&yaw,240,180,false,true,1,turn,error,style),error);R::ProjectedScene front;Need(R::BuildProjectedScene(&g,240,180,false,true,1,front,error,style),error);Need(std::abs(turn.opaque.front().x-front.opaque.front().x)>.2f&&std::abs(turn.opaque.front().distance-front.opaque.front().distance)>.01f,"row-major yaw changes actual imported cap projection anddepth");}
 auto g=Geometry();R::ProjectedScene s;std::string error;Need(!R::BuildProjectedScene(&g,240,180,true,true,1,s,error,3),"invalid style rejected");auto bad=R::ReferenceCapMesh(1);bad.triangles[0].a=uint32_t(bad.vertices.size());Need(!R::ValidateAccessoryMesh(bad),"bad imported index rejected");bad=R::ReferenceCapMesh(1);bad.vertices[0].u=std::nanf("");Need(!R::ValidateAccessoryMesh(bad),"nonfinite importedUV rejected");}
void AutomaticCrownFit(){
 std::string error;for(int style:{1,2})for(float faceScale:{.65f,1.f,1.35f})for(float yaw:{-.5f,0.f,.5f})for(float pitch:{-.45f,0.f,.60f})for(float capScale:{.75f,1.f,1.5f}){
  auto g=PosedGeometry(faceScale,yaw,pitch);
  R::ProjectedScene scene;Need(R::BuildProjectedScene(&g,240,180,true,true,1,scene,error,style,0,capScale,0),error);
  Need(scene.capFit.automatic&&scene.capFit.headProxy&&scene.capFit.templePairs>=1&&scene.faces.size()==1734,"actual observed temple fit and bounded skull proxy");
  Need(scene.capFit.headWidthCm>=11&&scene.capFit.headWidthCm<=21&&scene.capFit.horizontalScaleCm>=6.75f&&scene.capFit.horizontalScaleCm<=34.5f&&scene.capFit.verticalScaleCm==scene.capFit.horizontalScaleCm&&scene.capFit.crownHeightCm>0&&scene.capFit.crownHeightCm<=40&&scene.capFit.pupilClearanceCm>=.4499f,"finite bounded uniform fit preserves authored crown volume and actual bill above pupil plane");
  R::ProjectedScene glasses;Need(R::BuildProjectedScene(&g,240,180,true,false,1,glasses,error,style),error);
  Need(scene.lenses.size()==glasses.lenses.size(),"cap fit preserves lens triangle count");
  for(size_t i=0;i<glasses.lenses.size();++i){const auto& a=scene.lenses[i];const auto& b=glasses.lenses[i];Need(a.x==b.x&&a.y==b.y&&a.distance==b.distance&&a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a,"cap fitting leaves every Wayfarer lens projection/material bit-identical");}
  R::ProjectedScene largeGlasses;Need(R::BuildProjectedScene(&g,240,180,true,true,1.5f,largeGlasses,error,style,0,capScale,0),error);
  const size_t capVertices=R::ReferenceCapMesh(style).triangles.size()*3;
  Need(largeGlasses.opaque.size()>=capVertices&&scene.opaque.size()>=capVertices,"cap list retained");
  const auto& lm=*g.accessoryMesh;const auto world=[&](const A::FaceMeshVertex3D& p){const float d=scene.faceDistance+(p.z-lm.vertices[168].z)*240*scene.faceDistance/scene.focalPixels;return std::array<float,3>{(p.x-.5f)*240*d/scene.focalPixels,(.5f-p.y)*180*d/scene.focalPixels,-d};};
  const auto eyeLeft=world(lm.vertices[468]),eyeRight=world(lm.vertices[473]);const std::array<float,3> up{lm.transform[1],lm.transform[5],lm.transform[9]};
  for(size_t i=scene.opaque.size()-capVertices;i<scene.opaque.size();++i){const auto& v=scene.opaque[i];const std::array<float,3> p{(v.x-120)*v.distance/scene.focalPixels,(90-v.y)*v.distance/scene.focalPixels,-v.distance};float height=0;for(int k=0;k<3;++k)height+=(p[k]-(eyeLeft[k]+eyeRight[k])*.5f)*up[k];Need(height>=.44f,"independently inverted camera projection proves every capvertex clear of pupil plane");}
  for(size_t i=0;i<capVertices;++i){const auto& a=scene.opaque[scene.opaque.size()-capVertices+i];const auto& b=largeGlasses.opaque[largeGlasses.opaque.size()-capVertices+i];Need(a.x==b.x&&a.y==b.y&&a.distance==b.distance,"glasses user size never changes imported cap");}
  // Independently invert the camera projection. Translation cancels from
  // X/Y/Z mesh extents, exposing any crown flattening or sideways stretching.
  std::array<float,3> low{10000,10000,10000},high{-10000,-10000,-10000},modelLow{10,10,10},modelHigh{-10,-10,-10};
  for(const auto& v:R::ReferenceCapMesh(style).vertices){const std::array<float,3> p{v.x,v.y,v.z};for(int k=0;k<3;++k){modelLow[k]=std::min(modelLow[k],p[k]);modelHigh[k]=std::max(modelHigh[k],p[k]);}}
  for(size_t i=scene.opaque.size()-capVertices;i<scene.opaque.size();++i){const auto& v=scene.opaque[i];const std::array<float,3> p{(v.x-120)*v.distance/scene.focalPixels,(90-v.y)*v.distance/scene.focalPixels,-v.distance};for(int axis=0;axis<3;++axis){float value=0;for(int k=0;k<3;++k)value+=p[k]*lm.transform[k*4+axis];low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);}}
  std::array<float,3> observed{};for(int k=0;k<3;++k){observed[k]=(high[k]-low[k])/(modelHigh[k]-modelLow[k]);Need(std::isfinite(observed[k])&&std::abs(observed[k]-scene.capFit.horizontalScaleCm)<.0005f,"actual unprojected X/Y/Z uniformly preserve original source mesh units across real poses and cap sizes");}
  Need(std::abs((high[1]-low[1])/(high[0]-low[0])-(modelHigh[1]-modelLow[1])/(modelHigh[0]-modelLow[0]))<.0001f&&std::abs((high[2]-low[2])/(high[0]-low[0])-(modelHigh[2]-modelLow[2])/(modelHigh[0]-modelLow[0]))<.0001f,"independent authored crown-height/width and depth/width ratios remain intact");
  // Locate the actual exported central band vertex in the expanded scene,
  // independently unproject it, and compare its head-local seat with the
  // requested forehead offset and the whole-source pupil clearance limit.
  const auto forehead=world(lm.vertices[10]);float eyeHeight=0;for(int k=0;k<3;++k)eyeHeight+=(forehead[k]-(eyeLeft[k]+eyeRight[k])*.5f)*up[k];
  const float minimumRaise=.45f-eyeHeight-modelLow[1]*observed[1],expectedRaise=std::max(-.35f,minimumRaise);
  const auto& model=R::ReferenceCapMesh(style);const uint32_t bandIndex=style==2?354u:493u;const auto& authoredBand=model.vertices[bandIndex];
  size_t bandAt=capVertices;for(size_t triangle=0;triangle<model.triangles.size()&&bandAt==capVertices;++triangle){const auto& t=model.triangles[triangle];const std::array<uint32_t,3> indices{t.a,t.b,t.c};for(size_t corner=0;corner<3;++corner)if(indices[corner]==bandIndex){bandAt=triangle*3+corner;break;}}
  Need(bandAt<capVertices&&std::abs(authoredBand.y)<1e-6f&&std::abs(authoredBand.z-.5f)<1e-6f,"actual source-calibrated central front band exists at wearing anchor");
  const auto& band=scene.opaque[scene.opaque.size()-capVertices+bandAt];const std::array<float,3> bandWorld{(band.x-120)*band.distance/scene.focalPixels,(90-band.y)*band.distance/scene.focalPixels,-band.distance};float seatY=0,seatZ=0;
  for(int k=0;k<3;++k){seatY+=(bandWorld[k]-forehead[k])*lm.transform[k*4+1];seatZ+=(bandWorld[k]-forehead[k])*lm.transform[k*4+2];}
  Need(std::abs(seatY-expectedRaise)<.0005f&&std::abs(seatZ-.15f)<.0005f,"actual central band seats at authored forehead anchor with pupil floor only, without lowest-lateral-brim lift");
  Need(eyeHeight+seatY+modelLow[1]*observed[1]>=.4495f,"independent original rear minimum retains conservative whole-model pupil floor after corrected band seating");
 }
 // The open white visor remains compatibility-only: a face-derived proxy
 // cannot prove the exposed band wraps around the user's hair/skull. Its
 // failed wrap expectation is deliberately not represented as a public feature.
 // Exercise real public Pokemon rear triangles, not a synthetic colored plane.
 for(float pitch:{-.65f,0.f,.65f}){auto g=PosedGeometry(1,0,pitch);R::ProjectedScene wrapped;
  Need(R::BuildProjectedScene(&g,240,180,true,true,1,wrapped,error,2),error);
  const auto& mesh=*g.accessoryMesh;const auto world=[&](const A::FaceMeshVertex3D& p){const float d=wrapped.faceDistance+(p.z-mesh.vertices[168].z)*240*wrapped.faceDistance/wrapped.focalPixels;return std::array<float,3>{(p.x-.5f)*240*d/wrapped.focalPixels,(.5f-p.y)*180*d/wrapped.focalPixels,-d};};
  const auto left=world(mesh.vertices[468]),right=world(mesh.vertices[473]);const auto front=[&](const R::ProjectedVertex& v){const std::array<float,3> p{(v.x-120)*v.distance/wrapped.focalPixels,(90-v.y)*v.distance/wrapped.focalPixels,-v.distance};float z=0;for(int k=0;k<3;++k)z+=(p[k]-(left[k]+right[k])*.5f)*mesh.transform[k*4+2];return z;};
  for(size_t i=0;i<1728;++i)Need(front(wrapped.faces[i])<=-.099f,"every bounded skull proxy vertex is strictly behind the pupil plane");
  // Compare exactly the same ROI and real front-rim triangles with only the
  // optional cap proxy changed; no cap color or rear temples can mask a loss.
  auto frontRims=wrapped;frontRims.opaque.resize(frontRims.opaque.size()-R::ReferenceCapMesh(2).triangles.size()*3);frontRims.capTexture.reset();
  auto select=[&](const std::vector<R::ProjectedVertex>& input,float threshold,bool ahead){std::vector<R::ProjectedVertex> result;for(size_t i=0;i<input.size();i+=3){bool keep=true;for(int k=0;k<3;++k)keep=keep&&(ahead?front(input[i+k])>=threshold:front(input[i+k])<threshold);if(keep)result.insert(result.end(),input.begin()+i,input.begin()+i+3);}return result;};
  frontRims.opaque=select(frontRims.opaque,.1f,true);frontRims.lenses=select(frontRims.lenses,.1f,true);Need(!frontRims.opaque.empty()&&!frontRims.lenses.empty(),"actual Wayfarer front rims and lenses selected");
  auto withoutProxy=frontRims;withoutProxy.faces.erase(withoutProxy.faces.begin(),withoutProxy.faces.begin()+1728);std::vector<float> with,without;
  Need(R::RasterizeProjectedSceneForTesting(frontRims,with)&&R::RasterizeProjectedSceneForTesting(withoutProxy,without),"actual front-rim depth comparison");
  Need(with==without&&std::any_of(with.begin(),with.end(),[](float x){return x>0;}),"cap proxy preserves every rendered front-rim pixel in neutral/up/down poses");
  std::vector<float> complete;Need(R::RasterizeProjectedSceneForTesting(wrapped,complete)&&std::any_of(complete.begin(),complete.end(),[](float x){return x>0;}),"public Pokemon and glasses remain visibly rendered for actual posed landmarks");
  if(pitch==0.f){auto rear=wrapped;rear.opaque.erase(rear.opaque.begin(),rear.opaque.end()-R::ReferenceCapMesh(2).triangles.size()*3);rear.opaque=select(rear.opaque,-2.f,false);rear.lenses.clear();Need(!rear.opaque.empty(),"actual exported Pokemon rear geometry selected independently");auto noHead=rear;noHead.faces.erase(noHead.faces.begin(),noHead.faces.begin()+1728);
   Need(R::RasterizeProjectedSceneForTesting(rear,with)&&R::RasterizeProjectedSceneForTesting(noHead,without),"actual public cap rear-triangle depth raster");size_t hidden=0;for(size_t i=3;i<with.size();i+=4)if(without[i]>with[i]+.1f)++hidden;Need(hidden>0,"bounded skull actually hides exported Pokemon rear pixels where they overlap the head");}
 }
 auto g=Geometry();for(int style:{1,2}){
  R::ProjectedScene normal,lower,wider;Need(R::BuildProjectedScene(&g,240,180,false,true,1,normal,error,style,0,1,0),error);
  Need(R::BuildProjectedScene(&g,240,180,false,true,1,lower,error,style,0,1,-2),error);Need(lower.capFit.pupilClearanceCm>=.4499f,"lowest manual height retains hard pupil clearance");
  Need(R::BuildProjectedScene(&g,240,180,false,true,1,wider,error,style,0,1.5f,0),error);
  Need(std::abs(wider.capFit.horizontalScaleCm/normal.capFit.horizontalScaleCm-1.5f)<1e-5f&&std::abs(wider.capFit.verticalScaleCm/normal.capFit.verticalScaleCm-1.5f)<1e-5f,"cap-only fit slider scales calibrated crown independently");
  for(int invalid=0;invalid<6;++invalid){float size=1,height=0;if(invalid==0)size=.74f;if(invalid==1)size=1.51f;if(invalid==2)size=std::nanf("");if(invalid==3)height=-2.01f;if(invalid==4)height=3.01f;if(invalid==5)height=std::numeric_limits<float>::infinity();R::ProjectedScene bad;Need(!R::BuildProjectedScene(&g,240,180,false,true,1,bad,error,style,0,size,height),"out-of-range/nonfinite cap correction rejected");}
 }
}
void CpuOwnership(){auto f=Input();auto g=Geometry();const auto source=Bytes(f.get());R::FaceAccessory3DRenderer renderer;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;std::vector<std::vector<uint8_t>> stylePixels;
 for(int style:{1,2}){auto* out=renderer.ProcessFrame(f.get(),&g,true,true,1,&outcome,&layers,style);Need(out&&outcome==E::Overlays::OverlayOutcome::Success&&layers.glasses==0&&layers.hat==0,"both imported cap andWayfarer actuallyrender");stylePixels.push_back(Bytes(out));Need(stylePixels.back()!=source,"texturedGLB genuinelychanges pixels");Need(out->pts==f->pts&&out->duration==f->duration&&av_dict_get(out->metadata,"original",nullptr,0),"original timing andmetadata retained");Frame held(av_frame_clone(out));const auto saved=Bytes(held.get());renderer.Reset();Need(Bytes(held.get())==saved&&Bytes(f.get())==source,"retainedoutput andcaller immutable throughreset");}
 Need(stylePixels[0]!=stylePixels[1],"two actual capstyles produce differentpixels");Need(renderer.ProcessFrame(f.get(),nullptr,true,true,1,&outcome,&layers,1)==f.get()&&outcome==E::Overlays::OverlayOutcome::NotReady,"noface passes throughcurrent input");Need(renderer.ProcessFrame(f.get(),&g,true,true,1,&outcome,&layers,99)==f.get()&&outcome==E::Overlays::OverlayOutcome::NotReady,"invalidstyle no partialglasses");
 auto missing=scratch/L"empty-assets";std::filesystem::create_directory(missing);Need(R::SetCapAssetRootForTesting(missing.wstring()),"missing root");Need(renderer.ProcessFrame(f.get(),&g,true,true,1,&outcome,&layers,1)==f.get()&&outcome==E::Overlays::OverlayOutcome::NotReady&&layers.glasses==2&&layers.hat==2,"missingtexture wholeaccessory notready");Need(R::SetCapAssetRootForTesting(assets.wstring()),"restoreassets");AVFrame raw=*f;AVBufferRef owner=*f->buf[0];owner.size=1;raw.buf[0]=&owner;Need(!renderer.ProcessFrame(&raw,&g,false,true,1,&outcome,&layers,1)&&outcome==E::Overlays::OverlayOutcome::InvalidInput,"shortowner rejectsbeforepixelreads");}
struct Targets{ComPtr<ID3D11Texture2D> input,output,readback;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
Targets MakeTargets(ID3D11Device* d,const AVFrame* f){Targets t;std::vector<uint8_t> rgba(size_t(f->width)*f->height*4);for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x){const auto* p=f->data[0]+size_t(y)*f->linesize[0]+x*3;auto* out=rgba.data()+(size_t(y)*f->width+x)*4;out[0]=p[2];out[1]=p[1];out[2]=p[0];out[3]=255;}
 D3D11_TEXTURE2D_DESC desc{};desc.Width=f->width;desc.Height=f->height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{rgba.data(),UINT(f->width*4),0};Need(SUCCEEDED(d->CreateTexture2D(&desc,&data,&t.input))&&SUCCEEDED(d->CreateShaderResourceView(t.input.Get(),nullptr,&t.srv)),"ownedGPU input");desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Need(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t.output))&&SUCCEEDED(d->CreateUnorderedAccessView(t.output.Get(),nullptr,&t.uav)),"distinctGPUoutput");desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Need(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t.readback)),"permittedcompletedtestreadback");return t;}
std::vector<uint8_t> Read(ID3D11DeviceContext* context,Targets& t,int w,int h){context->CopyResource(t.readback.Get(),t.output.Get());D3D11_MAPPED_SUBRESOURCE map{};Need(SUCCEEDED(context->Map(t.readback.Get(),0,D3D11_MAP_READ,0,&map))&&map.pData,"actualcompletedGPUreadback");std::vector<uint8_t> bgr(size_t(w)*h*3);for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* p=static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch+x*4;auto* out=bgr.data()+(size_t(y)*w+x)*3;out[0]=p[2];out[1]=p[1];out[2]=p[0];}context->Unmap(t.readback.Get(),0);return bgr;}
bool NearTriangleEdge(const R::ProjectedScene& scene,int x,int y){
 const float px=x+.5f,py=y+.5f;for(const auto* list:{&scene.faces,&scene.opaque,&scene.lenses})for(size_t i=0;i+2<list->size();i+=3)for(int edge=0;edge<3;++edge){const auto& a=(*list)[i+edge];const auto& b=(*list)[i+(edge+1)%3];const float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;if(length<1e-8f)continue;const float t=std::clamp(((px-a.x)*dx+(py-a.y)*dy)/length,0.f,1.f);if(std::hypot(px-a.x-t*dx,py-a.y-t*dy)<=1.5f)return true;}return false;
}
void Gpu(bool hardware){E::Gpu::D3D11Context d3d;Need(d3d.Initialize(hardware)&&d3d.IsHardwareAdapter()==hardware,"actualrequestedGPUbackend");std::cout<<"ADAPTER "<<d3d.GetAdapterName()<<std::endl;auto f=Input();auto g=Geometry(.15f);auto targets=MakeTargets(d3d.GetDevice(),f.get());G::D3D11FaceAccessories renderer(d3d.GetDevice(),d3d.GetContext());R::FaceAccessory3DRenderer cpu;std::string error;E::Overlays::LayerReport layers;E::Overlays::OverlayOutcome outcome;
 auto colored=scratch/(hardware?L"colored-hw":L"colored-warp");std::filesystem::create_directory(colored);
 const std::vector<uint8_t> texels={255,0,0,255, 0,255,0,128, 0,0,255,0, 255,255,255,255};File(colored/L"white_tennis_cap.pfrgba",2,2,texels);File(colored/L"pokemon_masters_cap.pfrgba",2,2,texels);
 for(int run=0;run<6;++run){const int style=run==0?0:run==1?1:run==2?2:run==3?1:run==4?1:2;
  if(run==4)Need(R::SetCapAssetRootForTesting(colored.wstring()),"actual GPU controlledRGBA texture root");auto expected=Bytes(cpu.ProcessFrame(f.get(),&g,true,true,1,&outcome,&layers,style));Need(outcome==E::Overlays::OverlayOutcome::Success,"actualCPUoracle");Need(renderer.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,true,true,1,&layers,&error,style)==G::GpuOverlayOutcome::Success,error);auto actual=Read(d3d.GetContext(),targets,240,180);R::ProjectedScene checkedScene;Need(R::BuildProjectedScene(&g,240,180,true,true,1,checkedScene,error,style),error);
 double total=0;size_t wide=0;int maximum=0;for(size_t i=0;i<actual.size();++i){const int diff=std::abs(int(actual[i])-expected[i]);total+=diff;maximum=std::max(maximum,diff);if(diff>8){++wide;Need(NearTriangleEdge(checkedScene,int((i/3)%240),int(i/3/240)),"large raster differences confined to independently computed triangle edge neighborhood");}}std::cout<<"CAP_GPU "<<(hardware?"HW":"WARP")<<" style="<<style<<" mean="<<total/actual.size()<<" wide="<<wide<<" max="<<maximum<<std::endl;Need(total/actual.size()<.08&&wide<=12,"GPUUV/alpha/depth agree withactualCPU withinboundededge tolerance");ComPtr<ID3D11ShaderResourceView> bound;d3d.GetContext()->PSGetShaderResources(2,1,&bound);Need(!bound,"capPSresource unbound afterdispatch");}
 Need(R::SetCapAssetRootForTesting(assets.wstring()),"realGPUassets restored aftercontrolledRGBA");
 const auto retainedGpu=Read(d3d.GetContext(),targets,240,180);
 Need(renderer.Process(targets.srv.Get(),targets.uav.Get(),240,180,nullptr,true,true,1,&layers,&error,1)==G::GpuOverlayOutcome::NotReady,"GPUmissinggeometry notready");Need(Read(d3d.GetContext(),targets,240,180)==retainedGpu,"notready leaves retained GPUoutput intact");auto missing=scratch/(hardware?L"missing-gpu-hw":L"missing-gpu-warp");std::filesystem::create_directory(missing);Need(R::SetCapAssetRootForTesting(missing.wstring()),"GPUmissingroot");Need(renderer.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,true,true,1,&layers,&error,1)==G::GpuOverlayOutcome::NotReady&&layers.glasses==2&&layers.hat==2,"GPUmissingtexture wholeaccessorynotready");Need(R::SetCapAssetRootForTesting(assets.wstring()),"GPUassetsreset");Need(renderer.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,true,true,1,&layers,&error,2)==G::GpuOverlayOutcome::Success,"GPUtexturecache recovery afteridentitychange");E::Gpu::D3D11Context other;Need(other.Initialize(false),"independentdevice");G::D3D11FaceAccessories invalid(other.GetDevice(),d3d.GetContext());Need(invalid.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,true,1,&layers,&error,1)!=G::GpuOverlayOutcome::Success,"foreignGPUresources rejected");
 auto foreign=MakeTargets(other.GetDevice(),f.get());Need(invalid.Process(foreign.srv.Get(),foreign.uav.Get(),240,180,&g,false,true,1,&layers,&error,1)!=G::GpuOverlayOutcome::Success&&error.find("context belongs")!=std::string::npos,"actual foreigncontext reaches checked Init ownershipguard");
 auto savedOutput=targets.output;targets.output=targets.input;Need(Read(d3d.GetContext(),targets,240,180)==Bytes(f.get()),"GPUinput texture remains exact acrossactualdraws/recovery");targets.output=savedOutput;
}
}
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;assets=std::filesystem::absolute(argv[1]);scratch=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(scratch);try{
#define GROUP(name,expr) ++groups;std::cout<<"GROUP " name<<std::endl;expr;std::cout<<"PASS " name<<std::endl;
 GROUP("bounded actual texture decode / malformed extents / immutable snapshots",AssetLoading());GROUP("known texel UV orientation / bilinear alpha / perspective oracle",Sampling());GROUP("actual exported geometry / pose / UV / boundedscene",MeshAndScene());GROUP("automatic crown fitting / independent cap size / poses / pupil clearance / bounded skull proxy",AutomaticCrownFit());GROUP("actual CPU importedcaps / unchangedWayfarer / ownership / noface / missingasset",CpuOwnership());GROUP("hardware texturedcap / actualreadback / cache / deviceguards",Gpu(true));GROUP("WARP texturedcap / actualreadback / cache / deviceguards",Gpu(false));
#undef GROUP
 R::SetCapAssetRootForTesting(L"");std::cout<<"CAP_MODELS_PASS groups="<<groups<<" assertions="<<assertions<<std::endl;return 0;
 }catch(const std::exception& e){std::cerr<<"CAP_MODELS_FAIL groups="<<groups<<" assertions="<<assertions<<" reason="<<e.what()<<std::endl;return 1;}}
