#include "windows/src/effects/presets/studio_preset.h"
#include <cmath>
#include <iostream>
#include <limits>
using namespace PrismForge::Effects::Presets;
int checks=0,failures=0;
void Check(bool value,const char* why){++checks;if(!value){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
std::string With(const std::string& key,const std::string& value){return "{\"presetId\":\"original\",\""+key+"\":"+value+"}";}
int main(){
 std::string error;StudioPreset parsed;
 Check(PresetStore::Deserialize("{\"presetId\":\"legacy\"}",parsed,error)&&parsed.faceFilterStyle==0&&parsed.faceFilterScale==1&&parsed.faceFilterVerticalOffset==0,"legacy absent fields default Off and natural fit");
 for(int style=0;style<=5;++style)for(float scale:{.75f,1.f,1.5f})for(float height:{-3.f,0.f,3.f}){
  auto p=StudioPreset::Neutral();p.bypassAll=false;p.faceFilterStyle=style;p.faceFilterScale=scale;p.faceFilterVerticalOffset=height;
  p.realisticAccessories=true;p.overlays.bypass=false;
  Check(p.Validate(error),"each model and fit boundary valid");
  const auto text=PresetStore::Serialize(p);Check(PresetStore::Deserialize(text,parsed,error),"serialized native models deserialize");
  Check(parsed.faceFilterStyle==style&&parsed.faceFilterScale==scale&&parsed.faceFilterVerticalOffset==height,"model fit round trips exactly");
 }
 for(const auto& bad:{"-1","6","0.5","1.5","\"1\"","true","null","NaN","Infinity","1e999","01","1.0junk"})
  Check(!PresetStore::Deserialize(With("faceFilterStyle",bad),parsed,error),"malformed or unsupported model rejected");
 for(const auto& bad:{"0","0.749","1.501","-1","\"1\"","true","null","NaN","Infinity","1e999","01","1.0junk"})
  Check(!PresetStore::Deserialize(With("faceFilterScale",bad),parsed,error),"malformed or out of range model size rejected");
 for(const auto& bad:{"-3.001","3.001","\"0\"","true","null","NaN","Infinity","1e999","01","1.0junk"})
  Check(!PresetStore::Deserialize(With("faceFilterVerticalOffset",bad),parsed,error),"malformed or out of range model height rejected");
 for(const auto& key:{"faceFilterStyle","faceFilterScale","faceFilterVerticalOffset"})
  Check(!PresetStore::Deserialize("{\"presetId\":\"duplicate\",\""+std::string(key)+"\":1,\""+key+"\":1}",parsed,error),"duplicate fields rejected");
 for(float bad:{-4.f,4.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
  auto p=StudioPreset::Neutral();p.faceFilterScale=bad;Check(!p.Validate(error),"direct size guard independent of deserializer");
  p=StudioPreset::Neutral();p.faceFilterVerticalOffset=bad;Check(!p.Validate(error),"direct height guard independent of deserializer");
 }
 auto p=StudioPreset::Neutral();p.faceFilterStyle=3;p.faceFilterScale=1.23f;p.faceFilterVerticalOffset=-.7f;
 Check(PresetStore::Deserialize(PresetStore::Serialize(p),parsed,error),"noninteger decimal fit supported");
 Check(parsed.faceFilterScale==p.faceFilterScale&&parsed.faceFilterVerticalOffset==p.faceFilterVerticalOffset,"noninteger fit exact float roundtrip");
 Check(PresetStore::Deserialize("{\"presetId\":\"legacy-again\"}",parsed,error)&&parsed.faceFilterStyle==0&&parsed.faceFilterScale==1&&parsed.faceFilterVerticalOffset==0,"legacy parsing clears prior selected filter fields");
 std::cout<<"Native face filter preset checks "<<checks<<", failures "<<failures<<'\n';return failures?1:0;
}
