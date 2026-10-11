#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/packs/pack_preview_decoder.h"
#include "windows/src/effects/packs/sha256.h"
#include "windows/src/effects/studiostack/studio_stack.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <windows.h>
using namespace PrismForge::Effects;
namespace P = PrismForge::Effects::Packs;
namespace S = PrismForge::Effects::StudioStack;
namespace fs = std::filesystem;
static int assertions=0,groups=0;
static void Check(bool ok,const std::string& why){++assertions;if(!ok)throw std::runtime_error(why);}
static void Write(const fs::path& p,const std::string& bytes){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f.write(bytes.data(),bytes.size());f.close();Check(!f.fail(),"fixture write");}
static P::PackAssetEntry Asset(P::PackAssetType type,const std::string& id,const std::string& path,const std::string& data){return {type,id,path,P::Sha256::ComputeHex(reinterpret_cast<const uint8_t*>(data.data()),data.size()),data.size()};}
static std::string Grade(bool blue){std::ostringstream s;s<<"LUT_3D_SIZE 2\n";for(int b=0;b<2;++b)for(int g=0;g<2;++g)for(int r=0;r<2;++r)s<<(blue?0:1)<<" 0 "<<(blue?1:0)<<"\n";return s.str();}
static P::PackManifest Fixture(const fs::path& dir,const std::string& version,bool blue){
 P::PackManifest m;m.packId="com.prismforge.producttest";m.version=version;m.displayName="Verified grade";m.previewImagePath="preview.raw";
 const auto cube=Grade(blue);std::string rgba(64*64*4,'\0');for(size_t i=0;i<rgba.size();i+=4){rgba[i+(blue?2:0)]=char(255);rgba[i+3]=char(255);}
 m.assets={Asset(P::PackAssetType::CubeLut,"grade","looks/grade.cube",cube),Asset(P::PackAssetType::PreviewImage,"thumbnail","preview.raw",rgba)};
 Write(dir/"looks/grade.cube",cube);Write(dir/"preview.raw",rgba);std::string e;Check(P::ManifestJson::WriteToFile((dir/"manifest.json").u8string(),m,e),e);return m;
}
static void ManifestBoundaries(const fs::path& root){
 auto m=Fixture(root/"manifest-source","1.0.0",false);m.assets.clear();m.previewImagePath.clear();
 for(int i=0;i<50;++i)m.assets.push_back(Asset(P::PackAssetType::CubeLut,"grade"+std::to_string(i),"looks/"+std::to_string(i)+".cube",Grade(false)));
 P::PackManifest parsed;std::string error;auto json=P::ManifestJson::Serialize(m);Check(P::ManifestJson::Deserialize(json,parsed,error),"all50 declared assets must parse: "+error);Check(parsed.assets.size()==50,"50-asset manifest was truncated");
 m.assets.push_back(Asset(P::PackAssetType::CubeLut,"extra","looks/extra.cube",Grade(false)));Check(!P::ManifestJson::Deserialize(P::ManifestJson::Serialize(m),parsed,error),"51assets must be rejected");
 m.assets.pop_back();m.displayName=std::string("bad ")+std::string("\xF5\x80\x80\x80",4);Check(!P::ManifestJson::Deserialize(P::ManifestJson::Serialize(m),parsed,error),"invalid UTF8 aboveU10FFFF accepted");
 m.displayName="Valid";m.description="Valid";json=P::ManifestJson::Serialize(m);auto field=json.find("\"description\":");Check(field!=std::string::npos,"description fixture");auto begin=json.find('"',field+14);auto end=json.find('"',begin+1);json.replace(begin,end-begin+1,"42");Check(!P::ManifestJson::Deserialize(json,parsed,error),"wrong-typed description accepted");
 Write(root/"huge.json",std::string(1024*1024+1,' '));Check(!P::ManifestJson::ReadFromFile((root/"huge.json").u8string(),parsed,error),"oversized manifest accepted");++groups;
}
static void PinnedVersionAndActiveLifetime(const fs::path& root){
 auto source1=root/fs::u8path(u8"source1-é"),source2=root/"source2",library=root/fs::u8path(u8"library-é");Fixture(source1,"1.0.0",false);Fixture(source2,"1.0.1",true);std::string error;
 Check(P::PackManager::InstallPack(source1.u8string(),library.u8string(),error),"installv1: "+error);Check(P::PackManager::InstallPack(source2.u8string(),library.u8string(),error),"installv2: "+error);
 auto preset=Presets::StudioPreset::Neutral();preset.bypassAll=false;preset.presetId="test.pack";preset.displayName="Pinnedv2";preset.packIdReference="com.prismforge.producttest";preset.packVersionReference="1.0.1";preset.selectedInputLutPath="grade";preset.inputLutIntensity=1;
 S::StudioStack stack;Check(stack.ApplyPreset(preset,library.u8string(),error),"apply exactv2: "+error);
 AVFrame* f=av_frame_alloc();Check(f!=nullptr,"frame alloc");f->width=8;f->height=8;f->format=AV_PIX_FMT_BGR24;Check(av_frame_get_buffer(f,32)>=0,"buffer alloc");for(int y=0;y<f->height;++y)std::fill(f->data[0]+y*f->linesize[0],f->data[0]+y*f->linesize[0]+f->width*3,77);
 auto blue=[&](uint64_t seq){S::StackExecutionReport report;auto* out=stack.ProcessFrame(f,seq,seq*1000,&report);Check(out&&out->data[0],"no processed output");for(int y=0;y<8;++y)for(int x=0;x<8;++x){auto* pixel=out->data[0]+y*out->linesize[0]+x*3;Check(pixel[0]==255&&pixel[1]==0&&pixel[2]==0,"exact-version grade changed");}};
 blue(1);auto lease=P::PackManager::LoadPack((library/"com.prismforge.producttest"/"1.0.1").u8string(),error);Check(bool(lease),"retained lease load");auto before=lease->ReadAssetBytes("grade");
 Check(P::PackManager::UninstallPack(library.u8string(),preset.packIdReference,preset.packVersionReference,error),"uninstall: "+error);Check(lease->ReadAssetBytes("grade")==before,"lease changed on uninstall");blue(2);
 auto bad=preset;bad.packVersionReference="9.9.9";Check(!stack.ApplyPreset(bad,library.u8string(),error),"missing pinned version accepted");Check(stack.GetPreset().displayName=="Pinnedv2","failed apply changed preset");blue(3);
 bad.packVersionReference="1.0.0";bad.selectedInputLutPath="../../outside.cube";Check(!stack.ApplyPreset(bad,library.u8string(),error),"undeclared traversal LUT accepted");blue(4);
 bad.selectedInputLutPath="thumbnail";Check(!stack.ApplyPreset(bad,library.u8string(),error),"preview accepted as LUT");blue(5);
 bad=preset;Check(!stack.ApplyPreset(bad,"",error),"pack preset with no library opened local path");blue(6);
 av_frame_free(&f);++groups;
}
static void AmbiguousVersions(const fs::path& root){
 auto a=root/"source-a",b=root/"source-b",library=root/"library-ambiguous";Fixture(a,"1.0.0",false);Fixture(b,"1.0.1",true);std::string e;
 Check(P::PackManager::InstallPack(a.u8string(),library.u8string(),e),e);Check(P::PackManager::InstallPack(b.u8string(),library.u8string(),e),e);
 auto p=Presets::StudioPreset::Neutral();p.presetId="test.ambiguous";p.packIdReference="com.prismforge.producttest";p.selectedLutPath="grade";p.lutIntensity=1;S::StudioStack stack;
 Check(!stack.ApplyPreset(p,library.u8string(),e),"ambiguous versionless pack picked arbitrary grade");p.packVersionReference="1.0.0";Check(stack.ApplyPreset(p,library.u8string(),e),"explicit version should work: "+e);p.selectedLutPath="looks/grade.cube";Check(stack.ApplyPreset(p,library.u8string(),e),"declared relative path should work: "+e);++groups;
}

static void LaunchArchiveWorkflow(const fs::path& root){
 const auto delivery=fs::absolute(fs::path("deliveries")/"launch-packs-20261009");
 const auto library=root/fs::u8path(u8"launch-library-é");std::string e;
 struct Named{const char* name;};const Named names[]={{"warm_portrait"},{"soft_daylight"},{"clean_studio"},{"cinema_teal"},{"soft_mono"},{"evening_warmth"}};
 for(const auto& name:names){
  auto archive=delivery/"archives"/(std::string(name.name)+"-1.0.0.zip");
  Check(P::PackManager::InstallPackFromArchive(archive.u8string(),library.u8string(),e),std::string("real launch ZIP ")+name.name+": "+e);
 }
 auto cat=P::PackManager::GetCatalogue(library.u8string());Check(cat.size()==6,"six launch packs must be catalogued");
 for(const auto& entry:cat){
  Check(entry.isUsable&&entry.integrityState==P::PackIntegrityState::Valid,"launch pack integrity: "+entry.actionableError);
  auto loaded=P::PackManager::LoadPack((library/entry.packId/entry.version).u8string(),e);Check(bool(loaded),e);
  auto* preview=loaded->GetPreviewDescriptor();Check(preview&&preview->assetType==P::PackAssetType::PreviewImage,"typed declared preview");
  int w=0,h=0;Check(loaded->GetPreviewDimensions(w,h)&&w==512&&h==512,"real512x512 preview decode");
  const auto* manifest=loaded->GetVerifiedManifest();Check(manifest!=nullptr,"verified manifest");
  const P::PackAssetEntry* grade=nullptr;for(const auto& a:manifest->assets)if(a.assetType==P::PackAssetType::CubeLut)grade=&a;
  Check(grade!=nullptr,"declared launch LUT");auto bytes=loaded->ReadAssetBytes(grade->assetId);
  auto parsed=CubeLut::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),&e);Check(bool(parsed),"production LUT parse: "+e);
  auto preset=Presets::StudioPreset::Neutral();preset.bypassAll=false;preset.packIdReference=entry.packId;preset.packVersionReference=entry.version;preset.selectedInputLutPath=grade->assetId;preset.inputLutIntensity=1;
  S::StudioStack stack;Check(stack.ApplyPreset(preset,library.u8string(),e),"actual pack apply: "+e);
  AVFrame* f=av_frame_alloc();Check(f!=nullptr,"launch frame alloc");f->format=AV_PIX_FMT_BGR24;f->width=8;f->height=8;Check(av_frame_get_buffer(f,32)>=0,"launch buffer alloc");
  for(int y=0;y<8;++y)for(int x=0;x<8;++x){auto* v=f->data[0]+y*f->linesize[0]+3*x;v[0]=40;v[1]=100;v[2]=170;}
  S::StackExecutionReport report;auto* output=stack.ProcessFrame(f,1,1000,&report);Check(output&&output->data[0],"actual launch grade output");
  const auto expectedPixel=parsed->Sample(170.f/255,100.f/255,40.f/255);
  auto expected=[](float v){return int(std::round(std::clamp(v,0.f,1.f)*255));};
  for(int y=0;y<8;++y)for(int x=0;x<8;++x){auto* v=output->data[0]+y*output->linesize[0]+3*x;Check(std::abs(int(v[0])-expected(expectedPixel.b))<=1&&std::abs(int(v[1])-expected(expectedPixel.g))<=1&&std::abs(int(v[2])-expected(expectedPixel.r))<=1,"pipeline pixel differs from immutable launch LUT");}
  av_frame_free(&f);
 }
 const auto wp=library/"com.prismforge.packs.warm_portrait";
 auto retained=P::PackManager::LoadPack((wp/"1.0.0").u8string(),e);Check(bool(retained),e);std::vector<uint8_t> originalBytes;
 // Asset identifiers are data: select the actual typed LUT if authors chose a different ID.
 auto* oldManifest=retained->GetVerifiedManifest();std::string id;for(const auto& a:oldManifest->assets)if(a.assetType==P::PackAssetType::CubeLut)id=a.assetId;
 originalBytes=retained->ReadAssetBytes(id);Check(!originalBytes.empty(),"retained original bytes");
 auto update=delivery/"update-demo"/"archives"/"warm_portrait-1.0.1.zip";
 Check(P::PackManager::InstallPackFromArchive(update.u8string(),library.u8string(),e),"real update ZIP: "+e);
 Check(P::PackManager::GetCatalogue(library.u8string()).size()==7,"both exact versions remain selectable");
 auto newer=P::PackManager::LoadPack((wp/"1.0.1").u8string(),e);Check(bool(newer),e);Check(newer->ReadAssetBytes(id)!=originalBytes,"update contains a distinct grade");
 Check(retained->ReadAssetBytes(id)==originalBytes,"active lease changed during update");
 Check(P::PackManager::RecoverInterruptedTransactions(library.u8string(),e),"normal recovery should be clean: "+e);
 Check(P::PackManager::UninstallPack(library.u8string(),"com.prismforge.packs.warm_portrait","1.0.0",e),e);
 Check(fs::exists(wp/"1.0.1"),"exact-version uninstall removed update");Check(retained->ReadAssetBytes(id)==originalBytes,"retained lease changed on removal");
 for(const auto& entry:P::PackManager::GetCatalogue(library.u8string()))Check(P::PackManager::UninstallPack(library.u8string(),entry.packId,entry.version,e),"remove launch pack: "+e);
 Check(P::PackManager::GetCatalogue(library.u8string()).empty(),"library did not return to empty after remove");++groups;
}

int main(){auto root=fs::absolute(fs::path("build")/("pack-product-fixtures-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));try{Check(!fs::exists(root),"fixture root collision");fs::create_directories(root);ManifestBoundaries(root);PinnedVersionAndActiveLifetime(root);AmbiguousVersions(root);LaunchArchiveWorkflow(root);std::cout<<"Pack product integration: "<<groups<<" groups, "<<assertions<<" assertions, 0 failures\nOwned fixtures retained: "<<root.u8string()<<"\n";return 0;}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\nOwned fixtures retained: "<<root.u8string()<<"\n";return 1;}}