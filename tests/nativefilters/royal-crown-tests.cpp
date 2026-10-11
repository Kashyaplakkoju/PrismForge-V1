#define wmain BundledRenderMain
#include "native_filter_render_tests.cpp"
#undef wmain
#define main ExistingPresetMain
#include "../facefilterproduct/preset_tests.cpp"
#undef main
#define main ExistingLocalPresetMain
#define Check LocalPresetCheck
#define With LocalPresetWith
#define checks LocalPresetChecks
#define failures LocalPresetFailures
#include "local-product-tests.cpp"
#undef main
#undef Check
#undef With
#undef checks
#undef failures
#include <map>
namespace {
void CrownCacheAndPose(){
 std::string error;Need(R::SetFaceFilterAssetRootForTesting(assets.wstring()),"private optional crown test root");
 const auto model=R::LoadFaceFilter(5,error);Need(model&&R::ValidateFaceFilter(*model,error),error);Need(model->vertices.size()==35518&&model->triangles.size()==47499,"exact provided crown geometry preserved");
 Need(model->baseTexture&&model->metalRoughTexture,"crown material maps retained");Need(R::LoadFaceFilter(5,error)==model,"crown immutable snapshot cached");
 for(int original:{1,2,3}){const auto other=R::LoadFaceFilter(original,error);Need(other&&other!=model&&R::LoadFaceFilter(original,error)==other,"crown cache cannot replace any original built-in");}
 Need(!R::LoadFaceFilter(4,error)&&!error.empty(),"local style remains pointer-only and cannot index built-in cache");
 Need(model->authoredIpdMeters==.064f&&model->authoredEyeHeightMeters==0&&model->authoredEyeDepthMeters==0,"neutral pupil anchor represents user's accepted baked fit");
 const auto raw=R::LoadFaceFilterFile((assets.parent_path()/L"library"/L"8b886cd24d9e6c007280491336a3b370cf8f41075fba8d1496d17d6a11db6bc5.pfmodel").wstring(),error);
 Need(raw&&raw->vertices.size()==model->vertices.size()&&raw->triangles.size()==model->triangles.size()&&raw->materials.size()==model->materials.size(),"unfitted native source oracle preserves every vertex and triangle");
 constexpr double nativeUnits=98.43749532446985;
 for(size_t i=0;i<raw->vertices.size();++i){const auto& before=raw->vertices[i];const auto& after=model->vertices[i];const double ex=1.26*before.x,ey=1.26*(before.y-raw->authoredEyeHeightMeters)+8.8/nativeUnits,ez=1.26*(before.z-raw->authoredEyeDepthMeters)-5.8/nativeUnits;
  Need(std::abs(after.x-ex)<2e-7&&std::abs(after.y-ey)<2e-7&&std::abs(after.z-ez)<2e-7,"exact screenshot fit baked uniformly:126percent,headlocal y8.8cm,z-5.8cm");
  Need(before.nx==after.nx&&before.ny==after.ny&&before.nz==after.nz&&before.u==after.u&&before.v==after.v,"fit bake leaves normals and texture coordinates unchanged");}
 for(size_t i=0;i<raw->triangles.size();++i){const auto& before=raw->triangles[i];const auto& after=model->triangles[i];Need(before.a==after.a&&before.b==after.b&&before.c==after.c&&before.material==after.material,"fit bake preserves original topology and material assignment");}
 for(size_t i=0;i<raw->materials.size();++i){const auto& before=raw->materials[i];const auto& after=model->materials[i];Need(before.baseColor==after.baseColor&&before.metallic==after.metallic&&before.roughness==after.roughness&&before.blend==after.blend&&before.textured==after.textured,"fit bake preserves original material values");}
 for(const auto& pair:{std::make_pair(raw->baseTexture,model->baseTexture),std::make_pair(raw->metalRoughTexture,model->metalRoughTexture)})Need(pair.first&&pair.second&&pair.first->width==pair.second->width&&pair.first->height==pair.second->height&&pair.first->pixels==pair.second->pixels,"fit bake preserves exact base and MR texture pixels");
 for(float yaw:{-.65f,0.f,.65f})for(float pitch:{-.35f,0.f,.35f})for(float size:{.75f,1.25f})for(float height:{-.7f,.4f}){
  const auto g=PosedGeometry(1,yaw,pitch);const auto& m=g.accessoryMesh->transform;R::ProjectedScene scene;
  Need(R::BuildProjectedScene(&g,240,180,false,false,1,scene,error,0,0,1,0,5,size,height),error);
  Need(scene.faces.size()==1734&&scene.opaque.size()+scene.lenses.size()==model->triangles.size()*3,"all crown triangles and depth-only skull plus face retained");
  Need(std::abs(scene.filterMetricScale-100.f*.063f/.064f*size)<1e-4f&&scene.filterForwardClearanceCm==0&&scene.filterSurfaceSamples==0,"one uniform scale; mask clearance never flattens or shifts crown");
  auto world=[&](const A::FaceMeshVertex3D& p){const float d=scene.faceDistance+(p.z-g.accessoryMesh->vertices[168].z)*240*scene.faceDistance/scene.focalPixels;return std::array<float,3>{(p.x-.5f)*240*d/scene.focalPixels,(.5f-p.y)*180*d/scene.focalPixels,-d};};
  const auto left=world(g.accessoryMesh->vertices[468]),right=world(g.accessoryMesh->vertices[473]);
  for(int axis=0;axis<3;++axis){const float expected=(left[axis]+right[axis])*.5f+m[axis*4+1]*height;
   Need(std::abs(expected-scene.filterAnchorCm[axis])<1e-4f,"independent pupil-midpoint oracle preserves baked head-local screenshot fit across yaw/pitch/height");}
  size_t at=0;for(const auto& tri:model->triangles){if(model->materials[tri.material].blend)continue;for(uint32_t index:{tri.a,tri.b,tri.c}){
   const auto& v=model->vertices[index];const auto& q=scene.opaque[at++];const float x=(q.x-120)*q.distance/scene.focalPixels,y=(90-q.y)*q.distance/scene.focalPixels,z=-q.distance;
   const float ex=scene.filterAnchorCm[0]+scene.filterMetricScale*(m[0]*v.x+m[1]*v.y+m[2]*v.z),ey=scene.filterAnchorCm[1]+scene.filterMetricScale*(m[4]*v.x+m[5]*v.y+m[6]*v.z),ez=scene.filterAnchorCm[2]+scene.filterMetricScale*(m[8]*v.x+m[9]*v.y+m[10]*v.z);
   Need(std::abs(x-ex)<1e-4f&&std::abs(y-ey)<1e-4f&&std::abs(z-ez)<1e-4f,"independent inverse perspective preserves original crown XYZ proportions for every opaque vertex");
  }}
 }
}
void CrownBlendPositionCloud(const std::vector<R::ProjectedVertex>& expected,const std::vector<R::ProjectedVertex>& actual){
 using Key=std::array<int64_t,3>;auto key=[](const R::ProjectedVertex& p){return Key{int64_t(std::floor(p.x*1000)),int64_t(std::floor(p.y*1000)),int64_t(std::floor(p.distance*1000))};};
 std::map<Key,std::vector<R::ProjectedVertex>> positions;for(const auto& p:expected)positions[key(p)].push_back(p);
 for(const auto& p:actual){const auto k=key(p);bool found=false;auto consume=[&](const Key& candidate){const auto at=positions.find(candidate);if(at==positions.end())return false;auto& list=at->second;for(size_t i=0;i<list.size();++i){const auto& q=list[i];if(std::abs(p.x-q.x)<.0003f&&std::abs(p.y-q.y)<.0003f&&std::abs(p.distance-q.distance)<.0001f&&p.u==q.u&&p.v==q.v&&p.r==q.r&&p.g==q.g&&p.b==q.b&&p.a==q.a){list[i]=list.back();list.pop_back();return true;}}return false;};
  found=consume(k);for(int x=-1;!found&&x<=1;++x)for(int y=-1;!found&&y<=1;++y)for(int z=-1;!found&&z<=1;++z)if(x||y||z)found=consume({k[0]+x,k[1]+y,k[2]+z});
  Need(found,"order-independent transparent vertex multiset retains exact accepted local projected shape/material/UV under harmless depth-sort ties");}
 for(const auto& item:positions)Need(item.second.empty(),"every accepted transparent source vertex accounted for once");
}
void CrownAcceptedLocalFit(){
 std::string error;const auto raw=R::LoadFaceFilterFile((assets.parent_path()/L"library"/L"8b886cd24d9e6c007280491336a3b370cf8f41075fba8d1496d17d6a11db6bc5.pfmodel").wstring(),error);Need(bool(raw),error);
 R::CustomFaceFilterFit fit;fit.yCm=8.8f;fit.zCm=-5.8f;fit.headOcclusion=true;
 for(float yaw:{-.5f,0.f,.5f})for(float pitch:{-.25f,0.f,.25f}){const auto g=PosedGeometry(1,yaw,pitch);R::ProjectedScene manual,native;
  Need(R::BuildProjectedScene(&g,240,180,false,false,1,manual,error,0,0,1,0,4,1.26f,0,raw,fit),error);
  Need(R::BuildProjectedScene(&g,240,180,false,false,1,native,error,0,0,1,0,5,1,0),error);
  Need(manual.opaque.size()==native.opaque.size()&&manual.lenses.size()==native.lenses.size()&&manual.faces.size()==native.faces.size(),"native neutral crown preserves accepted local-fit topology and occlusion");
  for(size_t i=0;i<manual.opaque.size();++i){const auto& a=manual.opaque[i];const auto& b=native.opaque[i];Need(std::abs(a.x-b.x)<.0003f&&std::abs(a.y-b.y)<.0003f&&std::abs(a.distance-b.distance)<.0001f&&a.u==b.u&&a.v==b.v&&a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a,"style5 at100percent projects exactly like user's raw local model at126percent/y8.8/z-5.8");}
  // Equal-depth transparent triangles can swap sorting order after float baking.
  // Their projected vertex multiset and actual rendered pixels must still match.
  CrownBlendPositionCloud(manual.lenses,native.lenses);
  for(size_t i=0;i<manual.faces.size();++i){const auto& a=manual.faces[i];const auto& b=native.faces[i];Need(a.x==b.x&&a.y==b.y&&a.distance==b.distance,"native crown uses same depth-only head proxy as accepted local model");}
 }
 auto input=Input();const auto g=PosedGeometry();R::FaceAccessory3DRenderer local,builtin;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;
 auto* a=local.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,4,1.26f,0,raw,fit);Need(a&&outcome==E::Overlays::OverlayOutcome::Success,"accepted raw local model actual CPU draw");const auto expected=Bytes(a);
 auto* b=builtin.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,5);Need(b&&outcome==E::Overlays::OverlayOutcome::Success,"neutral built-in crown actual CPU draw");const auto actual=Bytes(b);
 double difference=0;size_t wide=0;for(size_t i=0;i<actual.size();++i){const int d=std::abs(int(actual[i])-int(expected[i]));difference+=d;wide+=d>16;}
 std::cout<<"CROWN_ACCEPTED_FIT mean="<<difference/actual.size()<<" wide="<<wide<<std::endl;Need(difference/actual.size()<.02&&wide<=8,"neutral built-in output matches accepted raw-local screenshot fit within rounding-edge tolerance");
}
void CrownDepthAndCpu(){
 auto input=Input();const auto original=Bytes(input.get());const auto g=PosedGeometry(1,.35f,-.1f);std::string error;R::ProjectedScene scene;
 Need(R::BuildProjectedScene(&g,240,180,false,false,1,scene,error,0,0,1,0,5),error);std::vector<float> hidden,all,depthOnly;
 Need(R::RasterizeProjectedSceneForTesting(scene,hidden),"actual crown raster with skull");auto unoccluded=scene;unoccluded.faces.clear();Need(R::RasterizeProjectedSceneForTesting(unoccluded,all),"actual crown raster without skull");
 size_t differences=0;for(size_t i=0;i<all.size();++i)differences+=std::abs(all[i]-hidden[i])>.001f;Need(differences>20,"skull depth proxy visibly hides back/intersecting crown surfaces");
 auto depth=scene;depth.opaque.clear();depth.lenses.clear();Need(R::RasterizeProjectedSceneForTesting(depth,depthOnly)&&std::all_of(depthOnly.begin(),depthOnly.end(),[](float value){return value==0;}),"depth-only skull never paints skin or hides camera itself");
 R::FaceAccessory3DRenderer cpu;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;
 auto* rendered=cpu.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,5);
 Need(rendered&&rendered!=input.get()&&outcome==E::Overlays::OverlayOutcome::Success&&layers.faceFilter==0&&Bytes(rendered)!=original,"native CPU crown visibly changes live output");Need(rendered->pts==input->pts&&rendered->duration==input->duration,"crown retains camera timestamps");Frame held(av_frame_clone(rendered));const auto retained=Bytes(held.get());
 for(int invalid=0;invalid<5;++invalid){auto bad=g;if(invalid==0)bad.detected=false;if(invalid==1)bad.geometrySequenceId++;if(invalid==2)bad.geometrySourceWidth--;if(invalid==3)bad.accessoryMesh.reset();if(invalid==4)bad.geometryTimestampPts++;
  Need(cpu.ProcessFrame(input.get(),&bad,false,false,1,&outcome,&layers,0,0,1,0,5)==input.get()&&outcome==E::Overlays::OverlayOutcome::NotReady&&layers.faceFilter==2,"lost/stale crown tracking returns current live input");}
 Need(Bytes(held.get())==retained&&Bytes(input.get())==original,"retained crown output and camera input remain immutable");
}
void CrownGpu(bool hardware){
 E::Gpu::D3D11Context d3d;Need(d3d.Initialize(hardware)&&d3d.IsHardwareAdapter()==hardware,"actual requested crown GPU backend");auto input=Input();auto targets=MakeTargets(d3d.GetDevice(),input.get());G::D3D11FaceAccessories gpu(d3d.GetDevice(),d3d.GetContext());R::FaceAccessory3DRenderer cpu;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;std::string error;const auto original=Bytes(input.get());
 for(float yaw:{0.f,.35f}){const auto g=PosedGeometry(1,yaw,-.1f);auto* expectedFrame=cpu.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,5);Need(expectedFrame&&outcome==E::Overlays::OverlayOutcome::Success,"crown CPU oracle succeeds before GPU comparison");const auto expected=Bytes(expectedFrame);
  Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,false,1,&layers,&error,0,0,1,0,5)==G::GpuOverlayOutcome::Success&&layers.faceFilter==0,error);const auto actual=Read(d3d.GetContext(),targets,240,180);R::ProjectedScene scene;Need(R::BuildProjectedScene(&g,240,180,false,false,1,scene,error,0,0,1,0,5),error);
  double total=0;size_t wide=0,changed=0;for(size_t i=0;i<actual.size();++i){const int difference=std::abs(int(actual[i])-expected[i]);total+=difference;changed+=actual[i]!=original[i];if(difference>16){++wide;Need(NearTriangleEdge(scene,int((i/3)%240),int(i/3/240)),"wide GPU difference confined to physical triangle edge");}}
  std::cout<<"CROWN_GPU "<<(hardware?"HW":"WARP")<<" yaw="<<yaw<<" mean="<<total/actual.size()<<" wide="<<wide<<" changed="<<changed<<std::endl;
  Need(changed>200&&total/actual.size()<.12&&wide<=24,"actual completed CPU/GPU crown shading agrees within established bounded edge tolerance");
  for(int slot=2;slot<=4;++slot){ComPtr<ID3D11ShaderResourceView> bound;d3d.GetContext()->PSGetShaderResources(slot,1,&bound);Need(!bound,"crown GPU material resources released after draw");}
  Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,nullptr,false,false,1,&layers,&error,0,0,1,0,5)==G::GpuOverlayOutcome::NotReady&&layers.faceFilter==2,"GPU lost tracking cannot report stale crown success");Need(Read(d3d.GetContext(),targets,240,180)==actual,"NotReady preserves retained GPU output for live-input caller fallback");
 }
}
void CrownMissing(){
 const auto missing=scratch/L"originals-without-private-crown";std::filesystem::create_directories(missing);for(const auto& file:std::filesystem::directory_iterator(assets))if(file.is_regular_file()&&file.path().filename()!=L"royal-crown.pfmodel")std::filesystem::copy_file(file.path(),missing/file.path().filename(),std::filesystem::copy_options::overwrite_existing);
 Need(R::SetFaceFilterAssetRootForTesting(missing.wstring()),"explicit root preserving originals but omitting private crown");std::string error;const auto first=R::LoadFaceFilter(5,error);Need(!first&&error.find("Royal crown asset unavailable")!=std::string::npos,"missing optional private crown has actionable unavailable error");const auto heldError=error;Need(!R::LoadFaceFilter(5,error)&&error==heldError,"failed crown result cached consistently");
 for(int style:{1,2,3})Need(bool(R::LoadFaceFilter(style,error)),"missing optional crown cannot disable original models");auto input=Input();const auto original=Bytes(input.get());auto g=PosedGeometry();R::FaceAccessory3DRenderer cpu;E::Overlays::OverlayOutcome outcome;E::Overlays::LayerReport layers;
 Need(cpu.ProcessFrame(input.get(),&g,false,false,1,&outcome,&layers,0,0,1,0,5)==input.get()&&outcome==E::Overlays::OverlayOutcome::InvalidInput&&layers.faceFilter==3&&cpu.GetLastError().find("Royal crown asset unavailable")!=std::string::npos,"missing crown retains live CPU feed and failed status");Need(Bytes(input.get())==original,"missing crown cannot modify camera input");
 E::Gpu::D3D11Context d3d;Need(d3d.Initialize(false),"actual WARP missing crown check");auto targets=MakeTargets(d3d.GetDevice(),input.get());G::D3D11FaceAccessories gpu(d3d.GetDevice(),d3d.GetContext());Need(gpu.Process(targets.srv.Get(),targets.uav.Get(),240,180,&g,false,false,1,&layers,&error,0,0,1,0,5)==G::GpuOverlayOutcome::AllocationFailed&&layers.faceFilter==3&&error.find("Royal crown asset unavailable")!=std::string::npos,"missing crown has concrete GPU asset failure");
 Need(R::SetFaceFilterAssetRootForTesting(assets.wstring())&&bool(R::LoadFaceFilter(5,error)),"new valid root invalidates failed cache and restores crown");
}
void CrownPresets(){Need(ExistingPresetMain()==0,"all standard six-style preset fit/bounds/legacy checks pass");Need(ExistingLocalPresetMain()==0,"all six-style local snapshot/identity/rigid-fit persistence checks pass");}
}
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;assets=std::filesystem::absolute(argv[1]);scratch=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(scratch);try{
#define CROWN_GROUP(name,expr) ++groups;std::cout<<"GROUP " name<<std::endl;expr;std::cout<<"PASS " name<<std::endl;
 CROWN_GROUP("private crown cache / exact baked screenshot fit / rigid metric proportions",CrownCacheAndPose());CROWN_GROUP("neutral built-in equals accepted raw local screenshot fit",CrownAcceptedLocalFit());CROWN_GROUP("depth-only skull / actual CPU output / tracking loss / retained ownership",CrownDepthAndCpu());CROWN_GROUP("actual hardware draw / completed pixel agreement / released resources",CrownGpu(true));CROWN_GROUP("actual WARP draw / completed pixel agreement / tracking loss",CrownGpu(false));CROWN_GROUP("optional private asset missing / original models preserved / cache recovery",CrownMissing());CROWN_GROUP("all six styles preset roundtrip / bounds / local identity retention",CrownPresets());
 R::SetFaceFilterAssetRootForTesting(L"");std::cout<<"ROYAL_CROWN_RENDER_PASS groups="<<groups<<" assertions="<<assertions<<" presetChecks="<<checks<<" localPresetChecks="<<LocalPresetChecks<<std::endl;return 0;
 }catch(const std::exception& e){R::SetFaceFilterAssetRootForTesting(L"");std::cerr<<"ROYAL_CROWN_RENDER_FAIL groups="<<groups<<" assertions="<<assertions<<" reason="<<e.what()<<std::endl;return 1;}}
