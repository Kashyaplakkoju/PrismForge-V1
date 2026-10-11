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
struct Targets{ComPtr<ID3D11Texture2D> input,output,readback;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
Targets MakeTargets(ID3D11Device* d,const AVFrame* f){Targets t;std::vector<uint8_t> rgba(size_t(f->width)*f->height*4);for(int y=0;y<f->height;++y)for(int x=0;x<f->width;++x){const auto* p=f->data[0]+size_t(y)*f->linesize[0]+x*3;auto* out=rgba.data()+(size_t(y)*f->width+x)*4;out[0]=p[2];out[1]=p[1];out[2]=p[0];out[3]=255;}
 D3D11_TEXTURE2D_DESC desc{};desc.Width=f->width;desc.Height=f->height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{rgba.data(),UINT(f->width*4),0};Need(SUCCEEDED(d->CreateTexture2D(&desc,&data,&t.input))&&SUCCEEDED(d->CreateShaderResourceView(t.input.Get(),nullptr,&t.srv)),"ownedGPU input");desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Need(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t.output))&&SUCCEEDED(d->CreateUnorderedAccessView(t.output.Get(),nullptr,&t.uav)),"distinctGPUoutput");desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Need(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t.readback)),"permittedcompletedtestreadback");return t;}
std::vector<uint8_t> Read(ID3D11DeviceContext* context,Targets& t,int w,int h){context->CopyResource(t.readback.Get(),t.output.Get());D3D11_MAPPED_SUBRESOURCE map{};Need(SUCCEEDED(context->Map(t.readback.Get(),0,D3D11_MAP_READ,0,&map))&&map.pData,"actualcompletedGPUreadback");std::vector<uint8_t> bgr(size_t(w)*h*3);for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* p=static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch+x*4;auto* out=bgr.data()+(size_t(y)*w+x)*3;out[0]=p[2];out[1]=p[1];out[2]=p[0];}context->Unmap(t.readback.Get(),0);return bgr;}

void NativePose(){std::string error;Need(R::SetFaceFilterAssetRootForTesting(assets.wstring()),"actual original filter root");
 for(int style=1;style<=3;++style){auto model=R::LoadFaceFilter(style,error);Need(model&&R::ValidateFaceFilter(*model,error),error);Need(model->baseTexture&&model->metalRoughTexture,"authored base / MR both loaded");
  for(float yaw:{-.65f,0.f,.65f})for(float pitch:{-.4f,0.f,.4f})for(float size:{.75f,1.25f}){auto g=PosedGeometry(1,yaw,pitch);R::ProjectedScene scene;Need(R::BuildProjectedScene(&g,240,180,false,false,1,scene,error,0,0,1,0,style,size,.4f),error);
   Need(scene.faces.size()>=1734&&scene.opaque.size()>50000,"real mesh and depth-only head proxy");Need(std::abs(scene.filterMetricScale-100.f*.063f/model->authoredIpdMeters*size)<1e-4,"authored pupil spacing determines one uniform metric scale");
   Need(scene.filterForwardClearanceCm>=.35f&&scene.filterForwardClearanceCm<=4.5f&&scene.filterSurfaceSamples<=61,"bounded current-face surface clearance");
   // Independent camera projection of the metric authored eye anchor must
   // remain on the observed pupil-midpoint ray across physical yaw/pitch.
   const auto& pose=g.accessoryMesh->transform;
   auto world=[&](const A::FaceMeshVertex3D& p){const float d=scene.faceDistance+(p.z-g.accessoryMesh->vertices[168].z)*240*scene.faceDistance/scene.focalPixels;return std::array<float,3>{(p.x-.5f)*240*d/scene.focalPixels,(.5f-p.y)*180*d/scene.focalPixels,-d};};
   auto nativeLeft=world(g.accessoryMesh->vertices[468]),nativeRight=world(g.accessoryMesh->vertices[473]);
   std::array<float,3> target{},authored{};
   for(int axis=0;axis<3;++axis){target[axis]=(nativeLeft[axis]+nativeRight[axis])*.5f+pose[axis*4+1]*.4f;authored[axis]=scene.filterAnchorCm[axis]+scene.filterMetricScale*(pose[axis*4+1]*model->authoredEyeHeightMeters+pose[axis*4+2]*model->authoredEyeDepthMeters);}
   Need(std::abs(target[0]/target[2]-authored[0]/authored[2])*scene.focalPixels<.002f&&std::abs(target[1]/target[2]-authored[1]/authored[2])*scene.focalPixels<.002f,"forward clearance preserves pupil midpoint projection instead of drifting with yaw / pitch");
   const auto& m=g.accessoryMesh->transform;size_t at=0;for(const auto& tri:model->triangles){if(model->materials[tri.material].blend)continue;for(uint32_t index:{tri.a,tri.b,tri.c}){const auto& v=model->vertices[index];const auto& q=scene.opaque[at++];const float x=(q.x-120)*q.distance/scene.focalPixels,y=(90-q.y)*q.distance/scene.focalPixels,z=-q.distance;
    const float ex=scene.filterAnchorCm[0]+scene.filterMetricScale*(m[0]*v.x+m[1]*v.y+m[2]*v.z),ey=scene.filterAnchorCm[1]+scene.filterMetricScale*(m[4]*v.x+m[5]*v.y+m[6]*v.z),ez=scene.filterAnchorCm[2]+scene.filterMetricScale*(m[8]*v.x+m[9]*v.y+m[10]*v.z);
    Need(std::abs(x-ex)<1e-4&&std::abs(y-ey)<1e-4&&std::abs(z-ez)<1e-4,"independent inverse-perspective oracle preserves authored X/Y/Z uniformly through physical pose");
   }}
   if(style==3){Need(scene.lenses.size()==3128*3,"transparent visor primitive remains separate");float previous=1e9f;for(size_t i=0;i<scene.lenses.size();i+=3){const float depth=scene.lenses[i].distance+scene.lenses[i+1].distance+scene.lenses[i+2].distance;Need(depth<=previous+1e-4f,"visor sorted far to near");previous=depth;Need(std::abs(scene.lenses[i].a-.35f)<1e-6&&scene.lenses[i].textured==3,"authored alpha retained for untextured visor");}}
  }
 }
 auto g=Geometry();R::ProjectedScene scene;for(int invalid=0;invalid<5;++invalid){auto bad=g;if(invalid==0)bad.detected=false;if(invalid==1)bad.geometrySequenceId++;if(invalid==2)bad.geometrySourceWidth--;if(invalid==3)bad.accessoryMesh.reset();if(invalid==4)bad.geometryTimestampPts++;Need(!R::BuildProjectedScene(&bad,240,180,false,false,1,scene,error,0,0,1,0,1),"invalid / lost / stale geometry cannot prepare overlay");}
 Need(!R::BuildProjectedScene(nullptr,240,180,false,false,1,scene,error,0,0,1,0,1),"no face never renders a frozen filter");
}
void SurfaceIndex(){
 std::string error;for(int style=1;style<=3;++style){auto model=R::LoadFaceFilter(style,error);Need(bool(model),error);
  for(float x:{-.15f,-.08f,-.032f,0.f,.032f,.08f,.15f})for(float y:{-.08f,-.04f,0.f,.008f,.03f,.06f,.10f,.18f}){
   float expected=-std::numeric_limits<float>::infinity();
   // Independent full authored-triangle oracle, with no index or bbox test.
   for(const auto& t:model->triangles){if(model->materials[t.material].blend)continue;const auto& a=model->vertices[t.a];const auto& b=model->vertices[t.b];const auto& c=model->vertices[t.c];
    const float den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);if(std::abs(den)<1e-12f)continue;
    const float wa=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/den,wb=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/den,wc=1.f-wa-wb;
    if(wa<0||wb<0||wc<0)continue;const float z=wa*a.z+wb*b.z+wc*c.z;if(z>=model->authoredEyeDepthMeters-.03f)expected=std::max(expected,z);
   }
   float indexed;Need(R::SampleFaceFilterFrontDepthForTesting(style,x,y,indexed),"actual bounded immutable surface index query");
   Need(indexed==expected,"indexed front-shell depth exactly agrees with independent full-triangle oracle, including open eye / helmet portals");
  }
 }
 float invalid;Need(!R::SampleFaceFilterFrontDepthForTesting(4,0,0,invalid)&&!R::SampleFaceFilterFrontDepthForTesting(1,std::nanf(""),0,invalid),"invalid surface-index request rejected");
}
void MeasuredClearance(){
 std::string error;auto g=PosedGeometry();auto baselineMesh=std::make_shared<A::FaceAccessoryGeometry>(*g.accessoryMesh);g.accessoryMesh=baselineMesh;baselineMesh->vertices[151]={.5f,.38f,0};
 for(int style:{1,2}){R::ProjectedScene base,protruding;Need(R::BuildProjectedScene(&g,240,180,false,false,1,base,error,0,0,1,0,style),error);
  auto raised=g;auto mesh=std::make_shared<A::FaceAccessoryGeometry>(*g.accessoryMesh);raised.accessoryMesh=mesh;
  // Move only the measured forehead 4 cm towards the camera, preserving its
  // physical X/Y position and the observed pupils. This is actual depth, not
  // an arbitrary changed pose matrix or a requested model-size multiplier.
  auto& p=mesh->vertices[151];const float d=base.faceDistance+(p.z-mesh->vertices[168].z)*240*base.faceDistance/base.focalPixels;
  const float next=d-4.f;p.x=.5f+(p.x-.5f)*d/next;p.y=.5f+(p.y-.5f)*d/next;p.z-=4.f*base.focalPixels/(240*base.faceDistance);
  Need(R::BuildProjectedScene(&raised,240,180,false,false,1,protruding,error,0,0,1,0,style),error);
  std::cout<<"CLEARANCE style="<<style<<" base="<<base.filterForwardClearanceCm<<" protruding="<<protruding.filterForwardClearanceCm<<std::endl;
  Need(protruding.filterForwardClearanceCm>base.filterForwardClearanceCm+1.f,"measured protruding forehead increases rigid mask clearance");
  Need(protruding.filterMetricScale==base.filterMetricScale,"clearance never changes authored uniform scale");
 }
}
void MaterialLighting(){R::ProjectedScene scene;scene.width=scene.height=scene.rasterWidth=scene.rasterHeight=4;
 scene.opaque={{0,0,.5f,1,.25f,.4f,.7f,1,0,0,3},{4,0,.5f,1,.25f,.4f,.7f,1,1,0,3},{0,4,.5f,1,.25f,.4f,.7f,1,0,1,3}};std::vector<float> base,metal,rough,texture,alpha;
 for(auto& v:scene.opaque){v.nx=-.189f;v.ny=.308f;v.nz=.933f;v.roughness=.15f;}Need(R::RasterizeProjectedSceneForTesting(scene,base),"material lighting actual raster");
 for(auto& v:scene.opaque)v.metallic=1;Need(R::RasterizeProjectedSceneForTesting(scene,metal)&&base!=metal,"metallic factor changes actual lighting");
 for(auto& v:scene.opaque){v.metallic=0;v.roughness=1;}Need(R::RasterizeProjectedSceneForTesting(scene,rough)&&base!=rough,"roughness factor changes actual highlight");
 scene.filterBaseTexture=std::make_shared<R::TextureRGBA>(R::TextureRGBA{1,1,{128,64,240,255}});scene.filterMetalRoughTexture=std::make_shared<R::TextureRGBA>(R::TextureRGBA{1,1,{255,20,240,255}});for(auto& v:scene.opaque){v.textured=2;v.metallic=1;}Need(R::RasterizeProjectedSceneForTesting(scene,texture)&&texture!=rough&&texture!=metal,"original base and linear MR pixels affect rendered colour");
 auto second=std::make_shared<R::TextureRGBA>(R::TextureRGBA{1,1,{255,255,0,255}});scene.filterMetalRoughTexture=second;Need(R::RasterizeProjectedSceneForTesting(scene,metal)&&texture!=metal,"MR map cannot be silently dropped");
 scene.opaque.clear();scene.lenses={{0,0,.5f,1,.8f,.9f,1,.35f,0,0,3},{4,0,.5f,1,.8f,.9f,1,.35f,1,0,3},{0,4,.5f,1,.8f,.9f,1,.35f,0,1,3}};
 Need(R::RasterizeProjectedSceneForTesting(scene,alpha)&&std::abs(alpha[(1*4+1)*4+3]-.35f)<1e-6,"visor alpha really blends instead of opaque painting");scene.faces={{0,0,.4f,1,0,0,0,1},{4,0,.4f,1,0,0,0,1},{0,4,.4f,1,0,0,0,1}};Need(R::RasterizeProjectedSceneForTesting(scene,base)&&std::all_of(base.begin(),base.end(),[](float v){return v==0;}),"wearer depth hides rear filter surfaces");
}
void MissingAsset(){auto input=Input();auto g=Geometry();R::FaceAccessory3DRenderer renderer;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;std::string error;const auto missing=scratch/L"missing-assets";std::filesystem::create_directories(missing);Need(R::SetFaceFilterAssetRootForTesting(missing.wstring()),"explicit missing root");Need(renderer.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,1)==input.get()&&outcome==E::Overlays::OverlayOutcome::InvalidInput&&layers.faceFilter==3&&!renderer.GetLastError().empty(),"missing model reports failed and retains live input rather than looking-for-face");E::Gpu::D3D11Context d3d;Need(d3d.Initialize(false),"WARP missing-asset guard");auto targets=MakeTargets(d3d.GetDevice(),input.get());G::D3D11FaceAccessories gpu(d3d.GetDevice(),d3d.GetContext());Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,false,1,&layers,&error,0,0,1,0,1)==G::GpuOverlayOutcome::AllocationFailed&&layers.faceFilter==3&&!error.empty(),"GPU missing asset is failed with concrete error");Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,false,1,&layers,&error,0,0,1,0,9)==G::GpuOverlayOutcome::InvalidInput&&layers.faceFilter==3,"invalid filter is config failure instead of noface");Need(R::SetFaceFilterAssetRootForTesting(assets.wstring()),"actual assets restored after failure");}
void NativeCpu(){auto input=Input();const auto original=Bytes(input.get());auto g=PosedGeometry();R::FaceAccessory3DRenderer renderer;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;
 for(int style=1;style<=3;++style){auto* actual=renderer.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,style);Need(actual&&actual!=input.get()&&outcome==E::Overlays::OverlayOutcome::Success&&layers.faceFilter==0,"actual CPU3D filter standalone applied");Need(Bytes(actual)!=original,"native filter changes output pixels");Need(actual->pts==input->pts&&actual->duration==input->duration,"original timing preserved");Frame held(av_frame_clone(actual));const auto retained=Bytes(held.get());Need(renderer.ProcessFrame(input.get(),nullptr,false,false,1,&outcome,&layers,0,0,1,0,style)==input.get()&&outcome==E::Overlays::OverlayOutcome::NotReady&&layers.faceFilter==2,"lost face returns current raw input without frozen model");Need(Bytes(held.get())==retained&&Bytes(input.get())==original,"retained frame and original input remain immutable");}
 Need(renderer.ProcessFrame(input.get(),nullptr,false,false,1,&outcome,&layers)==input.get()&&outcome==E::Overlays::OverlayOutcome::Bypass,"default off exact identity");
}
bool NearTriangleEdge(const R::ProjectedScene& scene,int x,int y){
 const float px=x+.5f,py=y+.5f;for(const auto* list:{&scene.faces,&scene.opaque,&scene.lenses})for(size_t i=0;i+2<list->size();i+=3)for(int edge=0;edge<3;++edge){const auto& a=(*list)[i+edge];const auto& b=(*list)[i+(edge+1)%3];const float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;if(length<1e-8f)continue;const float t=std::clamp(((px-a.x)*dx+(py-a.y)*dy)/length,0.f,1.f);if(std::hypot(px-a.x-t*dx,py-a.y-t*dy)<=1.5f)return true;}return false;
}
void NativeGpu(bool hardware){E::Gpu::D3D11Context d3d;Need(d3d.Initialize(hardware)&&d3d.IsHardwareAdapter()==hardware,"actual requested GPU backend");auto input=Input();auto targets=MakeTargets(d3d.GetDevice(),input.get());G::D3D11FaceAccessories gpu(d3d.GetDevice(),d3d.GetContext());R::FaceAccessory3DRenderer cpu;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;std::string error;
 const auto original=Bytes(input.get());Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,nullptr,false,false,1,&layers,&error)==G::GpuOverlayOutcome::Bypass&&layers.faceFilter==1,"GPU default off bypass identity");for(int style=1;style<=3;++style)for(float yaw:{0.f,.35f}){auto g=PosedGeometry(1,yaw,-.1f);auto expected=Bytes(cpu.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,style));Need(outcome==E::Overlays::OverlayOutcome::Success,"real CPU oracle applied");Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,false,1,&layers,&error,0,0,1,0,style)==G::GpuOverlayOutcome::Success&&layers.faceFilter==0,error);auto actual=Read(d3d.GetContext(),targets,240,180);
  R::ProjectedScene checkedScene;Need(R::BuildProjectedScene(&g,240,180,false,false,1,checkedScene,error,0,0,1,0,style),error);double total=0;size_t changed=0,wide=0;for(size_t i=0;i<actual.size();++i){const int diff=std::abs(int(actual[i])-expected[i]);total+=diff;if(diff>16){++wide;Need(NearTriangleEdge(checkedScene,int((i/3)%240),int(i/3/240)),"wide CPU/GPU differences confined to independently computed triangle edge neighbourhood");}changed+=actual[i]!=original[i];}std::cout<<"NATIVE_GPU "<<(hardware?"HW":"WARP")<<" style="<<style<<" yaw="<<yaw<<" mean="<<total/actual.size()<<" wide="<<wide<<std::endl;Need(changed>200,"actual GPU draw changes camera output");Need(total/actual.size()<.12&&wide<=24,"actual completed GPU result agrees with CPU shading / alpha / depth within bounded raster-edge differences (at most eight three-channel edge pixels)");
  for(int slot=2;slot<=4;++slot){ComPtr<ID3D11ShaderResourceView> bound;d3d.GetContext()->PSGetShaderResources(slot,1,&bound);Need(!bound,"all retained material GPU resources unbound after draw");}
  Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,nullptr,false,false,1,&layers,&error,0,0,1,0,style)==G::GpuOverlayOutcome::NotReady&&layers.faceFilter==2,"GPU lost face reports not ready rather than frozen success");Need(Read(d3d.GetContext(),targets,240,180)==actual,"NotReady does not overwrite a retained GPU output");
 }
}
}
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;assets=std::filesystem::absolute(argv[1]);scratch=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(scratch);try{
#define GROUP(name,expr) ++groups;std::cout<<"GROUP " name<<std::endl;expr;std::cout<<"PASS " name<<std::endl;
 GROUP("authored metric fit / physical pose / shape / eyes / visor / stale geometry",NativePose());GROUP("bounded immutable query index / full-triangle oracle",SurfaceIndex());GROUP("measured forehead clearance / preserved rigid scale",MeasuredClearance());GROUP("actual base colour / MR response / transparent visor / wearer depth",MaterialLighting());GROUP("native CPU output / retained ownership / no face / default off",NativeCpu());GROUP("missing model / invalid request / failed status / live input",MissingAsset());GROUP("hardware actual draw / complete pixel comparison / resource release",NativeGpu(true));GROUP("WARP actual draw / complete pixel comparison / resource release",NativeGpu(false));
 R::SetFaceFilterAssetRootForTesting(L"");std::cout<<"NATIVE_FILTER_RENDER_PASS groups="<<groups<<" assertions="<<assertions<<std::endl;return 0;
 }catch(const std::exception& e){std::cerr<<"NATIVE_FILTER_RENDER_FAIL groups="<<groups<<" assertions="<<assertions<<" reason="<<e.what()<<std::endl;return 1;}}
