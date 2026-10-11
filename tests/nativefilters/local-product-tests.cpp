#include "windows/src/effects/presets/studio_preset.h"
#include <cmath>
#include <iostream>
#include <limits>
using namespace PrismForge::Effects::Presets;
int checks=0,failures=0;
void Check(bool value,const char* why){++checks;if(!value){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
std::string With(const std::string& key,const std::string& value){return "{\"presetId\":\"local\",\""+key+"\":"+value+"}";}
int main(){
 std::string error;StudioPreset parsed;
 Check(PresetStore::Deserialize("{\"presetId\":\"legacy\"}",parsed,error)&&parsed.faceFilterStyle==0&&parsed.faceFilterCustomPath.empty()&&parsed.faceFilterCustomFit.headOcclusion,"legacy preserves neutral local defaults");
 for(int style=0;style<=5;++style){auto p=StudioPreset::Neutral();p.faceFilterStyle=style;p.faceFilterCustomPath=u8"C:/User Models/用户/abc.pfmodel";p.faceFilterCustomName=u8"Local 猫 🦊";
 p.faceFilterCustomFit.xCm=-20;p.faceFilterCustomFit.yCm=20;p.faceFilterCustomFit.zCm=1.23f;p.faceFilterCustomFit.tiltDegrees=180;p.faceFilterCustomFit.turnDegrees=-180;p.faceFilterCustomFit.rollDegrees=32;p.faceFilterCustomFit.headOcclusion=false;
 p.faceFilterCustomModel=std::make_shared<PrismForge::Effects::Overlays::Realistic::NativeFaceFilter>();
 Check(p.Validate(error),"all six filter modes accept additive bounded local fields");
 Check(PresetStore::Deserialize(PresetStore::Serialize(p),parsed,error),"local preset roundtrip succeeds");
 Check(parsed.faceFilterStyle==style&&parsed.faceFilterCustomPath==p.faceFilterCustomPath&&parsed.faceFilterCustomName==p.faceFilterCustomName,"UTF8 identity and style roundtrip exact");
 Check(!parsed.faceFilterCustomModel&&parsed.faceFilterCustomFit.xCm==-20&&parsed.faceFilterCustomFit.yCm==20&&parsed.faceFilterCustomFit.zCm==p.faceFilterCustomFit.zCm&&parsed.faceFilterCustomFit.tiltDegrees==180&&parsed.faceFilterCustomFit.turnDegrees==-180&&parsed.faceFilterCustomFit.rollDegrees==32&&!parsed.faceFilterCustomFit.headOcclusion,"pointer is transient and full rigid fitting persists");}
 for(const auto& key:{"faceFilterCustomX","faceFilterCustomY","faceFilterCustomZ"})for(const auto& bad:{"-20.001","20.001","\"0\"","true","null","NaN","Infinity","1e999","01","0junk"})Check(!PresetStore::Deserialize(With(key,bad),parsed,error),"malformed or excessive translation rejected");
 for(const auto& key:{"faceFilterCustomTilt","faceFilterCustomTurn","faceFilterCustomRoll"})for(const auto& bad:{"-180.001","180.001","\"0\"","true","null","NaN","Infinity","1e999","01","0junk"})Check(!PresetStore::Deserialize(With(key,bad),parsed,error),"malformed or excessive degree rotation rejected");
 for(const auto& key:{"faceFilterCustomPath","faceFilterCustomName"})for(const auto& bad:{"true","null","25","\"unfinished","\"bad\\q\"","\"bad\\uD800\"","\"\\u0000\"","\"bad\\nname\""})Check(!PresetStore::Deserialize(With(key,bad),parsed,error),"malformed local identity rejected");
 for(const auto& bad:{"null","1","\"true\"","truejunk"})Check(!PresetStore::Deserialize(With("faceFilterCustomOcclusion",bad),parsed,error),"malformed occlusion boolean rejected");
 for(const auto& key:{"faceFilterCustomPath","faceFilterCustomName","faceFilterCustomX","faceFilterCustomY","faceFilterCustomZ","faceFilterCustomTilt","faceFilterCustomTurn","faceFilterCustomRoll","faceFilterCustomOcclusion"})Check(!PresetStore::Deserialize("{\"presetId\":\"dup\",\""+std::string(key)+"\":\"x\",\""+key+"\":\"x\"}",parsed,error),"duplicate local fields rejected");
 Check(PresetStore::Deserialize(With("faceFilterCustomName","\"\\u732b \\uD83E\\uDD8A\""),parsed,error)&&parsed.faceFilterCustomName==u8"猫 🦊","Unicode escape pairs decode to UTF8");
 for(float bad:{-21.f,21.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){auto p=StudioPreset::Neutral();p.faceFilterCustomFit.xCm=bad;Check(!p.Validate(error),"direct preset translations guarded");}
 for(float bad:{-181.f,181.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){auto p=StudioPreset::Neutral();p.faceFilterCustomFit.turnDegrees=bad;Check(!p.Validate(error),"direct preset rotations guarded");}
 auto p=StudioPreset::Neutral();p.faceFilterCustomName=std::string(161,'x');Check(!p.Validate(error),"display text bounded");p=StudioPreset::Neutral();p.faceFilterCustomPath=std::string(4097,'x');Check(!p.Validate(error),"identity path bounded");p=StudioPreset::Neutral();p.faceFilterCustomName=std::string("\xc0\xaf",2);Check(!p.Validate(error),"overlong UTF8 rejected");
 Check(PresetStore::Deserialize("{\"presetId\":\"legacy-again\"}",parsed,error)&&parsed.faceFilterCustomName.empty()&&parsed.faceFilterCustomFit.xCm==0&&parsed.faceFilterCustomFit.headOcclusion,"legacy parse clears prior local state");
 std::cout<<"Local native product checks "<<checks<<", failures "<<failures<<'\n';return failures?1:0;
}
