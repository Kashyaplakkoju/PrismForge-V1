#include "effects/overlays/face_filter_asset.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <cstring>
#include <thread>
#include <array>
#include <functional>
#include <stdexcept>
namespace R=PrismForge::Effects::Overlays::Realistic;
namespace {
int assertions=0,groups=0;std::filesystem::path assets,scratch;
void Need(bool v,const char* why){++assertions;if(!v)throw std::runtime_error(why);}
void Put(std::vector<uint8_t>& b,size_t at,uint32_t v){for(size_t i=0;i<4;++i)b[at+i]=uint8_t(v>>(i*8));}
void Float(std::vector<uint8_t>& b,size_t at,float v){uint32_t bits;std::memcpy(&bits,&v,4);Put(b,at,bits);}
std::vector<uint8_t> Good(){
 std::vector<uint8_t> b(204);std::memcpy(b.data(),"PFM1",4);
 for(const auto& pair:std::array<std::array<uint32_t,2>,9>{{{{4,1}},{{8,3}},{{12,1}},{{16,1}},{{20,1}},{{24,1}},{{28,1}},{{32,1}},{{36,152}}}})Put(b,pair[0],pair[1]);
 Float(b,40,.064f);Float(b,44,.008f);Float(b,48,.014f);
 for(size_t i=0;i<3;++i){size_t at=52+i*32;Float(b,at,float(i==1)*.05f);Float(b,at+4,float(i==2)*.05f);Float(b,at+20,1.f);}
 Put(b,148,0);Put(b,152,1);Put(b,156,2);Put(b,160,0);
 for(size_t i=0;i<6;++i)Float(b,164+i*4,1.f);Put(b,188,2);Put(b,192,0);
 for(size_t i=196;i<204;++i)b[i]=255;return b;
}
void Write(const std::filesystem::path& p,const std::vector<uint8_t>& b){
 std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));f.close();Need(bool(f),"fixture complete write");
}
void Builtins(){
 Need(R::SetFaceFilterAssetRootForTesting(assets.wstring()),"actual absolute asset root");std::string error;
 const std::array<size_t,3> vc{11418,10906,19279},tc{22800,21752,38328};
 std::shared_ptr<const R::NativeFaceFilter> held;
 for(int style=1;style<=3;++style){
  auto m=R::LoadFaceFilter(style,error);Need(bool(m)&&error.empty(),"actual builtin asset loaded");
  Need(m->vertices.size()==vc[style-1]&&m->triangles.size()==tc[style-1],"actual complete source geometry counts");
  Need(m->baseTexture&&m->metalRoughTexture&&m->baseTexture->width==2048&&m->metalRoughTexture->height==2048,"both actual2048 PBR images retained");
  Need(m->baseTexture->pixels.size()==16777216&&m->metalRoughTexture->pixels.size()==16777216,"exact bounded decoded images");
  Need(R::ValidateFaceFilter(*m,error),"builtin validates independently");
  Need(R::LoadFaceFilter(style,error)==m,"immutable cache exact same identity");
  Need(std::abs(m->authoredIpdMeters-(style==2?.076f:.064f))<1e-7,"actual owl authored aperture spacing correction");
  Need(std::abs(m->authoredEyeHeightMeters-.008f)<1e-7,"authored eye height preserved");
  if(style==3){Need(m->materials.size()==2&&m->materials[1].blend&&!m->materials[1].textured,"helmet separate transparent untextured visor");
   Need(std::abs(m->materials[1].baseColor[3]-.35f)<1e-7&&std::abs(m->materials[1].roughness-.06f)<1e-7,"original visor alpha and material roughness");}
  else Need(m->materials.size()==1&&m->materials[0].textured&&!m->materials[0].blend,"textured opaque mask material preserved");
  if(style==1)held=m;
 }
 Need(!R::LoadFaceFilter(0,error)&&!R::LoadFaceFilter(4,error),"off and unsupported style do not resolve asset");
 Need(!R::SetFaceFilterAssetRootForTesting(L"relative"),"relative asset roots rejected without cache replacement");
 std::array<std::shared_ptr<const R::NativeFaceFilter>,8> concurrent{};
 std::array<std::thread,8> threads;for(size_t i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{std::string e;concurrent[i]=R::LoadFaceFilter(1,e);});
 for(auto& t:threads)t.join();for(auto& m:concurrent)Need(m==held,"concurrent consumers retain single immutable model");
 Need(R::SetFaceFilterAssetRootForTesting(scratch.wstring()),"cache reset");
 Need(held->triangles.size()==22800&&held->baseTexture->pixels.size()==16777216,"held native model lives across cache reset");
}
void Rejections(){
 auto good=Good();std::string error;std::vector<std::function<void(std::vector<uint8_t>&)>> cases{
  [](auto& b){b[0]='X';},[](auto& b){Put(b,4,2);},[](auto& b){Put(b,8,0);},[](auto& b){Put(b,8,60001);},
  [](auto& b){Put(b,12,60001);},[](auto& b){Put(b,16,9);},[](auto& b){Put(b,20,2049);},[](auto& b){Put(b,24,0);},
  [](auto& b){Put(b,28,0);},[](auto& b){Put(b,36,151);},[](auto& b){b.pop_back();},[](auto& b){b.push_back(1);},
  [](auto& b){Float(b,40,.001f);},[](auto& b){Float(b,40,std::numeric_limits<float>::quiet_NaN());},
  [](auto& b){Float(b,44,1.f);},[](auto& b){Float(b,48,std::numeric_limits<float>::infinity());},
  [](auto& b){Float(b,52,2.01f);},[](auto& b){Float(b,52,std::numeric_limits<float>::infinity());},
  [](auto& b){Float(b,72,0.f);},[](auto& b){Float(b,76,10001.f);},[](auto& b){Float(b,80,std::numeric_limits<float>::quiet_NaN());},
  [](auto& b){Put(b,148,3);},[](auto& b){Put(b,160,1);},[](auto& b){Float(b,164,-.01f);},
  [](auto& b){Float(b,180,1.1f);},[](auto& b){Float(b,184,std::numeric_limits<float>::quiet_NaN());},
  [](auto& b){Put(b,188,4);},[](auto& b){Put(b,192,1);},[](auto& b){Float(b,176,.5f);}
 };
 for(size_t i=0;i<cases.size();++i){
  auto d=scratch/(L"malformed-"+std::to_wstring(i));std::filesystem::create_directory(d);
  auto mutated=good;cases[i](mutated);Write(d/L"aurora-fox-mask.pfmodel",mutated);
  Need(R::SetFaceFilterAssetRootForTesting(d.wstring()),"malformed ordinary root accepted");
  Need(!R::LoadFaceFilter(1,error)&&!error.empty(),"malformed resource rejected");
 }
 auto d=scratch/L"large";std::filesystem::create_directory(d);Write(d/L"aurora-fox-mask.pfmodel",good);
 std::filesystem::resize_file(d/L"aurora-fox-mask.pfmodel",64ull*1024*1024+1);
 Need(R::SetFaceFilterAssetRootForTesting(d.wstring())&&!R::LoadFaceFilter(1,error),"over64MiB file rejected before allocation");
 auto missing=scratch/L"missing";std::filesystem::create_directory(missing);
 Need(R::SetFaceFilterAssetRootForTesting(missing.wstring())&&!R::LoadFaceFilter(1,error),"missing resource rejected");
 std::filesystem::create_directory(missing/L"aurora-fox-mask.pfmodel");Need(R::SetFaceFilterAssetRootForTesting(missing.wstring())&&!R::LoadFaceFilter(1,error),"directory resource rejected");
}
void CacheRecovery(){
 auto d=scratch/L"cache";std::filesystem::create_directory(d);std::string error;
 Need(R::SetFaceFilterAssetRootForTesting(d.wstring())&&!R::LoadFaceFilter(1,error),"negative cache first miss");
 Write(d/L"aurora-fox-mask.pfmodel",Good());Need(!R::LoadFaceFilter(1,error),"negative cache stable until explicit generation reset");
 Need(R::SetFaceFilterAssetRootForTesting(d.wstring()),"explicit root cache generation reset");auto m=R::LoadFaceFilter(1,error);
 Need(bool(m)&&error.empty()&&m->vertices.size()==3,"repaired resource successfully loads");
 auto b=Good();b[0]='X';Write(d/L"aurora-fox-mask.pfmodel",b);
 Need(R::LoadFaceFilter(1,error)==m,"cached bytes unaffected by later on-disk mutation");
 Need(R::SetFaceFilterAssetRootForTesting(d.wstring())&&!R::LoadFaceFilter(1,error),"new generation detects changed malformed bytes");
 Need(m->baseTexture->pixels[0]==255&&m->vertices[1].x==.05f,"held immutable snapshot survives changed file");
}
void ReparseGuard(){
 auto d=scratch/L"links";std::filesystem::create_directory(d);auto target=d/L"target.pfmodel";Write(target,Good());
 auto link=d/L"aurora-fox-mask.pfmodel";std::string error;
 if(CreateSymbolicLinkW(link.c_str(),target.c_str(),2)){
  Need(R::SetFaceFilterAssetRootForTesting(d.wstring())&&!R::LoadFaceFilter(1,error),"leaf reparse link rejected");
  auto rootlink=scratch/L"linked-root";Need(CreateSymbolicLinkW(rootlink.c_str(),d.c_str(),3)!=0,"test directory link create");
  Need(!R::SetFaceFilterAssetRootForTesting(rootlink.wstring()),"reparse asset root rejected");
 }else std::cout<<"SKIP symbolic link fixture: privilege unavailable error="<<GetLastError()<<"\n";
}
}
int wmain(int argc,wchar_t** argv){
 try{Need(argc==3,"assets and scratch supplied");assets=std::filesystem::absolute(argv[1]);scratch=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(scratch);
  for(const auto& group:std::array<std::pair<const char*,void(*)()>,4>{{{"builtin-materials-lifetime",Builtins},{"malformed-resource-bounds",Rejections},{"cache-recovery",CacheRecovery},{"reparse-boundary",ReparseGuard}}}){
   group.second();++groups;std::cout<<"PASS "<<group.first<<"\n";}
  Need(R::SetFaceFilterAssetRootForTesting(L""),"production root restored");std::cout<<"NATIVE_FILTER_ASSETS "<<groups<<" groups "<<assertions<<" assertions\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
