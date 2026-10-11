#include "studio_preset.h"
#include <fstream>
#include <sstream>
#include <limits>
#include <cstdlib>
#include <regex>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace PrismForge::Effects::Presets {

namespace fs = std::filesystem;

namespace {

inline bool IsFinite(float v) {
    return !std::isnan(v) && !std::isinf(v);
}

inline float SafeClamp(float v, float lo, float hi, float def) {
    if (!IsFinite(v)) return def;
    return std::max(lo, std::min(hi, v));
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

} // anonymous namespace

// -------------------------------------------------------------
// Built-In Presets
// -------------------------------------------------------------

StudioPreset StudioPreset::Neutral() {
    StudioPreset p;
    p.schemaVersion = 1;
    p.presetId = "builtin.neutral";
    p.displayName = "Neutral Studio Passthrough";
    p.description = "Exact bit-level bypass of all studio effects";
    p.bypassAll = true;

    p.lighting = Lighting::LightingConfig::Neutral();
    p.appearance = Appearance::AppearanceConfig::Neutral();
    p.overlays = Overlays::OverlayStackConfig::Neutral();
    p.background.mode = BackgroundMode::Original;
    p.lutIntensity = 0.0f;
    return p;
}

StudioPreset StudioPreset::WarmKeyCoolFill() {
    StudioPreset p;
    p.schemaVersion = 1;
    p.presetId = "builtin.warm_key_cool_fill";
    p.displayName = "Warm Key & Cool Fill Studio";
    p.description = "Dual-light cinematic warmth with gentle skin smoothing and broadcast border";
    p.bypassAll = false;

    p.lighting = Lighting::LightingConfig::DefaultTwoLight();

    p.appearance.smoothingStrength = 0.35f;
    p.appearance.detailPreservation = 0.60f;
    p.appearance.sharpenStrength = 0.20f;
    p.appearance.bypass = false;

    p.overlays.bypass = false;
    p.overlays.glasses.enabled = true;
    p.overlays.glasses.scaleMultiplier = 1.0f;
    p.overlays.studioFrame.enabled = true;
    p.overlays.studioFrame.opacity = 0.65f;

    p.background.mode = BackgroundMode::Original;
    p.lutIntensity = 0.0f;
    return p;
}

StudioPreset StudioPreset::SubtleTouchup() {
    StudioPreset p;
    p.schemaVersion = 1;
    p.presetId = "builtin.subtle_touchup";
    p.displayName = "Subtle Broadcast Touchup";
    p.description = "Restrained natural skin smoothing with feature protection and soft studio fill";
    p.bypassAll = false;

    p.lighting.bypass = false;
    p.lighting.light1.enabled = false;
    p.lighting.light2.enabled = false;
    p.lighting.ambientIntensity = 0.90f;
    p.lighting.fillIntensity = 0.12f;
    p.lighting.globalStrength = 1.0f;

    p.appearance.smoothingStrength = 0.40f;
    p.appearance.detailPreservation = 0.55f;
    p.appearance.sharpenStrength = 0.15f;
    p.appearance.bypass = false;

    p.overlays.bypass = false;
    p.overlays.studioFrame.enabled = true;
    p.overlays.studioFrame.opacity = 0.45f;

    p.background.mode = BackgroundMode::Original;
    p.lutIntensity = 0.0f;
    return p;
}

// -------------------------------------------------------------
// Serialization & Deserialization
// -------------------------------------------------------------

std::string PresetStore::Serialize(const StudioPreset& p) {
    std::ostringstream ss;
    ss.precision(std::numeric_limits<float>::max_digits10);
    ss << "{\n";
    ss << "  \"schemaVersion\": " << p.schemaVersion << ",\n";
    ss << "  \"presetId\": \"" << EscapeJson(p.presetId) << "\",\n";
    ss << "  \"displayName\": \"" << EscapeJson(p.displayName) << "\",\n";
    ss << "  \"description\": \"" << EscapeJson(p.description) << "\",\n";
    ss << "  \"bypassAll\": " << (p.bypassAll ? "true" : "false") << ",\n";
    ss << "  \"realisticAccessories\": " << (p.realisticAccessories ? "true" : "false") << ",\n";
    ss << "  \"realisticCapStyle\": " << p.realisticCapStyle << ",\n";
    ss << "  \"realisticGlassesStyle\": " << p.realisticGlassesStyle << ",\n";
    ss << "  \"realisticCapScale\": " << p.realisticCapScale << ",\n";
    ss << "  \"realisticCapVerticalOffset\": " << p.realisticCapVerticalOffset << ",\n";
    ss << "  \"faceFilterStyle\": " << p.faceFilterStyle << ",\n";
    ss << "  \"faceFilterScale\": " << p.faceFilterScale << ",\n";
    ss << "  \"faceFilterRenderEdge\": " << p.faceFilterRenderEdge << ",\n";
    ss << "  \"faceFilterVerticalOffset\": " << p.faceFilterVerticalOffset << ",\n";
    ss << "  \"faceFilterCustomPath\": \"" << EscapeJson(p.faceFilterCustomPath) << "\",\n";
    ss << "  \"faceFilterCustomName\": \"" << EscapeJson(p.faceFilterCustomName) << "\",\n";
    ss << "  \"faceFilterCustomX\": " << p.faceFilterCustomFit.xCm << ",\n";
    ss << "  \"faceFilterCustomY\": " << p.faceFilterCustomFit.yCm << ",\n";
    ss << "  \"faceFilterCustomZ\": " << p.faceFilterCustomFit.zCm << ",\n";
    ss << "  \"faceFilterCustomTilt\": " << p.faceFilterCustomFit.tiltDegrees << ",\n";
    ss << "  \"faceFilterCustomTurn\": " << p.faceFilterCustomFit.turnDegrees << ",\n";
    ss << "  \"faceFilterCustomRoll\": " << p.faceFilterCustomFit.rollDegrees << ",\n";
    ss << "  \"faceFilterCustomOcclusion\": " << (p.faceFilterCustomFit.headOcclusion?"true":"false") << ",\n";


    ss << "  \"inputColour\": { \"look\": " << p.inputLook
       << ", \"strength\": " << p.inputLookStrength
       << ", \"lutPath\": \"" << EscapeJson(p.selectedInputLutPath)
       << "\", \"lutIntensity\": " << p.inputLutIntensity << " },\n";

    // Lighting
    ss << "  \"lighting\": {\n";
    ss << "    \"bypass\": " << (p.lighting.bypass ? "true" : "false") << ",\n";
    ss << "    \"globalStrength\": " << p.lighting.globalStrength << ",\n";
    ss << "    \"ambientIntensity\": " << p.lighting.ambientIntensity << ",\n";
    ss << "    \"fillIntensity\": " << p.lighting.fillIntensity << ",\n";
    ss << "    \"lightingMode\": " << static_cast<int>(p.lighting.lightingMode) << ",\n";
    ss << "    \"light1\": {\n";
    ss << "      \"enabled\": " << (p.lighting.light1.enabled ? "true" : "false") << ",\n";
    ss << "      \"posX\": " << p.lighting.light1.posX << ", \"posY\": " << p.lighting.light1.posY << ", \"posZ\": " << p.lighting.light1.posZ << ",\n";
    ss << "      \"colorR\": " << p.lighting.light1.colorR << ", \"colorG\": " << p.lighting.light1.colorG << ", \"colorB\": " << p.lighting.light1.colorB << ",\n";
    ss << "      \"colorTemperature\": " << p.lighting.light1.colorTemperature << ", \"tint\": " << p.lighting.light1.tint << ",\n";
    ss << "      \"intensity\": " << p.lighting.light1.intensity << "\n";
    ss << "    },\n";
    ss << "    \"light2\": {\n";
    ss << "      \"enabled\": " << (p.lighting.light2.enabled ? "true" : "false") << ",\n";
    ss << "      \"posX\": " << p.lighting.light2.posX << ", \"posY\": " << p.lighting.light2.posY << ", \"posZ\": " << p.lighting.light2.posZ << ",\n";
    ss << "      \"colorR\": " << p.lighting.light2.colorR << ", \"colorG\": " << p.lighting.light2.colorG << ", \"colorB\": " << p.lighting.light2.colorB << ",\n";
    ss << "      \"colorTemperature\": " << p.lighting.light2.colorTemperature << ", \"tint\": " << p.lighting.light2.tint << ",\n";
    ss << "      \"intensity\": " << p.lighting.light2.intensity << "\n";
    ss << "    },\n";
    ss << "    \"shadows\": {\n";
    ss << "      \"enabled\": " << (p.lighting.shadowConfig.enabled ? "true" : "false") << ",\n";
    ss << "      \"maxSteps\": " << p.lighting.shadowConfig.maxSteps << ",\n";
    ss << "      \"maxDistance\": " << p.lighting.shadowConfig.maxDistance << ",\n";
    ss << "      \"rayElevation\": " << p.lighting.shadowConfig.rayElevation << ",\n";
    ss << "      \"shadowIntensity\": " << p.lighting.shadowConfig.shadowIntensity << ",\n";
    ss << "      \"shadowSoftness\": " << p.lighting.shadowConfig.shadowSoftness << "\n";
    ss << "    }\n";
    ss << "  },\n";

    // Appearance
    ss << "  \"appearance\": {\n";
    ss << "    \"bypass\": " << (p.appearance.bypass ? "true" : "false") << ",\n";
    ss << "    \"smoothingStrength\": " << p.appearance.smoothingStrength << ",\n";
    ss << "    \"touchupBoost\": " << p.appearance.touchupBoost << ",\n";
    ss << "    \"detailPreservation\": " << p.appearance.detailPreservation << ",\n";
    ss << "    \"sharpenStrength\": " << p.appearance.sharpenStrength << "\n";
    ss << "  },\n";

    // Overlays
    ss << "  \"overlays\": {\n";
    ss << "    \"bypass\": " << (p.overlays.bypass ? "true" : "false") << ",\n";
    ss << "    \"glasses\": { \"enabled\": " << (p.overlays.glasses.enabled ? "true" : "false")
       << ", \"opacity\": " << p.overlays.glasses.baseOpacity << " },\n";
    ss << "    \"hat\": { \"enabled\": " << (p.overlays.hat.enabled ? "true" : "false")
       << ", \"opacity\": " << p.overlays.hat.baseOpacity << " },\n";
    ss << "    \"studioFrame\": { \"enabled\": " << (p.overlays.studioFrame.enabled ? "true" : "false")
       << ", \"opacity\": " << p.overlays.studioFrame.opacity << " }\n";
    ss << "  },\n";

    // Background & LUT
    int bgModeVal = (p.backgroundMode != StudioBackgroundMode::Original)
        ? static_cast<int>(p.backgroundMode)
        : static_cast<int>(p.background.mode);
    ss << "  \"background\": { \"mode\": " << bgModeVal
       << ", \"fitMode\": " << static_cast<int>(p.backgroundFitMode) << " },\n";
    ss << "  \"lut\": {\n";
    ss << "    \"path\": \"" << EscapeJson(p.selectedLutPath) << "\",\n";
    ss << "    \"intensity\": " << p.lutIntensity << "\n";
    ss << "  },\n";

    // References
    ss << "  \"packReference\": {\n";
    ss << "    \"packId\": \"" << EscapeJson(p.packIdReference) << "\",\n";
    ss << "    \"version\": \"" << EscapeJson(p.packVersionReference) << "\"\n";
    ss << "  }\n";
    ss << "}\n";

    return ss.str();
}

bool StudioPreset::Validate(std::string& outError) const {
    if(faceFilterStyle<0||faceFilterStyle>5){outError="Unsupported 3D face filter";return false;}
    if(faceFilterRenderEdge<256||faceFilterRenderEdge>Overlays::Realistic::HardModelRenderEdge){outError="Unsupported face filter raster budget";return false;}
    if(!IsFinite(faceFilterScale)||faceFilterScale<.75f||faceFilterScale>1.5f||
       !IsFinite(faceFilterVerticalOffset)||faceFilterVerticalOffset<-3.f||faceFilterVerticalOffset>3.f){outError="Unsupported face filter fit";return false;}
    const auto& custom=faceFilterCustomFit;
    for(float value:{custom.xCm,custom.yCm,custom.zCm})if(!IsFinite(value)||value<-20.f||value>20.f){outError="Unsupported local model position";return false;}
    for(float value:{custom.tiltDegrees,custom.turnDegrees,custom.rollDegrees})if(!IsFinite(value)||value<-180.f||value>180.f){outError="Unsupported local model rotation";return false;}
    auto validText=[](const std::string& text,size_t limit){
        if(text.size()>limit)return false;
        for(size_t i=0;i<text.size();){const unsigned char lead=text[i++];if(lead<32||lead==127)return false;
            if(lead<128)continue;unsigned count=0,code=0,minimum=0;
            if(lead>=0xc2&&lead<=0xdf){count=1;code=lead&31;minimum=0x80;}
            else if(lead>=0xe0&&lead<=0xef){count=2;code=lead&15;minimum=0x800;}
            else if(lead>=0xf0&&lead<=0xf4){count=3;code=lead&7;minimum=0x10000;}else return false;
            if(text.size()-i<count)return false;while(count--){unsigned char ch=text[i++];if((ch&0xc0)!=0x80)return false;code=(code<<6)|(ch&63);}
            if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
        }return true;};
    if(!validText(faceFilterCustomPath,4096)||!validText(faceFilterCustomName,160)){outError="Invalid local model identity";return false;}
    if(realisticCapStyle<0||realisticCapStyle>2){outError="Unsupported 3D cap style";return false;}
    if(realisticGlassesStyle<0||realisticGlassesStyle>3){outError="Unsupported 3D glasses style";return false;}
    if(!IsFinite(realisticCapScale)||realisticCapScale<.75f||realisticCapScale>1.5f||
       !IsFinite(realisticCapVerticalOffset)||realisticCapVerticalOffset<-2.f||realisticCapVerticalOffset>3.f){outError="Unsupported cap fit";return false;}
    if (presetId.empty()) {
        outError = "Missing presetId";
        return false;
    }
    auto checkFinite = [&](float v, const char* name) -> bool {
        if (!IsFinite(v)) {
            outError = std::string("Non-finite value for ") + name;
            return false;
        }
        return true;
    };

    if (!checkFinite(lighting.globalStrength, "lighting.globalStrength")) return false;
    if (!checkFinite(lighting.ambientIntensity, "lighting.ambientIntensity")) return false;
    if (!checkFinite(lighting.fillIntensity, "lighting.fillIntensity")) return false;
    if (!checkFinite(lighting.light1.posX, "light1.posX")) return false;
    if (!checkFinite(lighting.light1.posY, "light1.posY")) return false;
    if (!checkFinite(lighting.light1.posZ, "light1.posZ")) return false;
    if (!checkFinite(lighting.light1.colorR, "light1.colorR")) return false;
    if (!checkFinite(lighting.light1.colorG, "light1.colorG")) return false;
    if (!checkFinite(lighting.light1.colorB, "light1.colorB")) return false;
    if (!checkFinite(lighting.light1.colorTemperature, "light1.colorTemperature")) return false;
    if (!checkFinite(lighting.light1.tint, "light1.tint")) return false;
    if (!checkFinite(lighting.light1.intensity, "light1.intensity")) return false;
    if (!checkFinite(lighting.light2.posX, "light2.posX")) return false;
    if (!checkFinite(lighting.light2.posY, "light2.posY")) return false;
    if (!checkFinite(lighting.light2.posZ, "light2.posZ")) return false;
    if (!checkFinite(lighting.light2.colorR, "light2.colorR")) return false;
    if (!checkFinite(lighting.light2.colorG, "light2.colorG")) return false;
    if (!checkFinite(lighting.light2.colorB, "light2.colorB")) return false;
    if (!checkFinite(lighting.light2.colorTemperature, "light2.colorTemperature")) return false;
    if (!checkFinite(lighting.light2.tint, "light2.tint")) return false;
    if (!checkFinite(lighting.light2.intensity, "light2.intensity")) return false;
    if (!checkFinite(lighting.shadowConfig.maxDistance, "lighting.shadowConfig.maxDistance")) return false;
    if (!checkFinite(lighting.shadowConfig.rayElevation, "lighting.shadowConfig.rayElevation")) return false;
    if (!checkFinite(lighting.shadowConfig.shadowIntensity, "lighting.shadowConfig.shadowIntensity")) return false;
    if (!checkFinite(lighting.shadowConfig.shadowSoftness, "lighting.shadowConfig.shadowSoftness")) return false;

    if (!checkFinite(appearance.smoothingStrength, "appearance.smoothingStrength")) return false;
    if (!checkFinite(appearance.touchupBoost, "appearance.touchupBoost")) return false;
    if (!checkFinite(appearance.detailPreservation, "appearance.detailPreservation")) return false;
    if (!checkFinite(appearance.sharpenStrength, "appearance.sharpenStrength")) return false;

    if (!checkFinite(overlays.glasses.scaleMultiplier, "overlays.glasses.scaleMultiplier")) return false;
    if (!checkFinite(overlays.glasses.baseOpacity, "overlays.glasses.baseOpacity")) return false;
    if (!checkFinite(overlays.hat.scaleMultiplier, "overlays.hat.scaleMultiplier")) return false;
    if (!checkFinite(overlays.hat.baseOpacity, "overlays.hat.baseOpacity")) return false;
    if (!checkFinite(overlays.studioFrame.opacity, "overlays.studioFrame.opacity")) return false;

    if (inputLook < 0 || inputLook > 3 || !std::isfinite(inputLookStrength) ||
        inputLookStrength < 0 || inputLookStrength > 1 || !std::isfinite(inputLutIntensity) ||
        inputLutIntensity < 0 || inputLutIntensity > 1) {
        outError="Invalid input colour look or LUT strength"; return false;
    }
    if (!checkFinite(lutIntensity, "lutIntensity")) return false;
    if (lutIntensity < 0.0f || lutIntensity > 1.0f) {
        outError = "lutIntensity must be in [0.0, 1.0]";
        return false;
    }

    int bgModeInt = static_cast<int>(backgroundMode);
    if (bgModeInt < 0 || bgModeInt > 3) {
        outError = "backgroundMode out of range [0, 3]";
        return false;
    }
    int fitModeInt = static_cast<int>(backgroundFitMode);
    if (fitModeInt < 0 || fitModeInt > 2) {
        outError = "backgroundFitMode out of range [0, 2]";
        return false;
    }

    return true;
}

bool PresetStore::Deserialize(const std::string& jsonText, StudioPreset& destination, std::string& outError) {
    StudioPreset outPreset;
    if (jsonText.empty()) {
        outError = "Empty preset JSON";
        return false;
    }

    outPreset = StudioPreset::Neutral();

    auto findStringIn = [](const std::string& block, const std::string& key) -> std::string {
        std::regex pattern("\\\""+key+"\\\"\\s*:");
        std::smatch match; if(!std::regex_search(block,match,pattern)) return {};
        size_t pos=static_cast<size_t>(match.position()+match.length());
        while(pos<block.size() && std::isspace(static_cast<unsigned char>(block[pos]))) ++pos;
        if(pos>=block.size() || block[pos++]!='"') return {};
        std::string out;
        while(pos<block.size()) {
            char ch=block[pos++]; if(ch=='"') return out;
            if(ch=='\\') {
                if(pos>=block.size()) return {};
                ch=block[pos++];
                switch(ch) { case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;
                    case 'b':ch='\b';break;case 'f':ch='\f';break;case '\\':case '/':case '"':break;default:return {}; }
            }
            out+=ch;
        }
        return {};
    };

    auto findFloatIn = [](const std::string& block, const std::string& key, float def) -> float {
        std::string pattern = "\"" + key + "\":";
        size_t pos = block.find(pattern);
        if (pos == std::string::npos) return def;
        pos += pattern.size();
        while (pos < block.size() && (std::isspace(static_cast<unsigned char>(block[pos])) || block[pos] == ':')) pos++;
        size_t endPos = pos;
        while (endPos < block.size() && (std::isdigit(static_cast<unsigned char>(block[endPos])) ||
               block[endPos] == '.' || block[endPos] == '-' || block[endPos] == '+' || block[endPos] == 'e' || block[endPos] == 'E')) {
            endPos++;
        }
        if (endPos > pos) {
            try {
                float val = std::stof(block.substr(pos, endPos - pos));
                return IsFinite(val) ? val : def;
            } catch (...) {
                return def;
            }
        }
        return def;
    };

    auto findBoolIn = [](const std::string& block, const std::string& key, bool def) -> bool {
        std::string pattern = "\"" + key + "\":";
        size_t pos = block.find(pattern);
        if (pos == std::string::npos) return def;
        pos += pattern.size();
        while (pos < block.size() && std::isspace(static_cast<unsigned char>(block[pos]))) pos++;
        if (block.substr(pos, 4) == "true") return true;
        if (block.substr(pos, 5) == "false") return false;
        return def;
    };

    auto findBlock = [&](const std::string& key) -> std::string {
        std::string pattern = "\"" + key + "\":";
        size_t pos = jsonText.find(pattern);
        if (pos == std::string::npos) return "";
        pos = jsonText.find('{', pos + pattern.size());
        if (pos == std::string::npos) return "";
        int depth = 1;
        size_t endPos = pos + 1;
        while (endPos < jsonText.size() && depth > 0) {
            if (jsonText[endPos] == '{') depth++;
            else if (jsonText[endPos] == '}') depth--;
            endPos++;
        }
        return jsonText.substr(pos, endPos - pos);
    };

    int schemaVer = static_cast<int>(findFloatIn(jsonText, "schemaVersion", 1.0f));
    if (schemaVer < 1 || schemaVer > 2) {
        outError = "Unsupported preset schemaVersion: " + std::to_string(schemaVer);
        return false;
    }

    std::string id = findStringIn(jsonText, "presetId");
    if (id.empty()) {
        outError = "Missing required 'presetId'";
        return false;
    }

    outPreset.schemaVersion = schemaVer;
    outPreset.presetId = id;
    outPreset.displayName = findStringIn(jsonText, "displayName");
    outPreset.description = findStringIn(jsonText, "description");
    outPreset.bypassAll = findBoolIn(jsonText, "bypassAll", false);
    outPreset.realisticAccessories = findBoolIn(jsonText, "realisticAccessories", false);
    // This optional selector is an integer enum; never silently coerce malformed values.
    outPreset.realisticCapStyle = 0;
    const std::string capKey="\"realisticCapStyle\"";
    const auto capKeyAt=jsonText.find(capKey);
    if(capKeyAt!=std::string::npos) {
        size_t pos=capKeyAt+capKey.size();
        auto skipSpace=[&]{while(pos<jsonText.size()&&(jsonText[pos]==' '||jsonText[pos]=='\t'||jsonText[pos]=='\r'||jsonText[pos]=='\n'))++pos;};
        skipSpace();if(pos>=jsonText.size()||jsonText[pos++]!=':'){outError="Malformed 3D cap style";return false;}
        skipSpace();if(pos>=jsonText.size()||jsonText[pos]<'0'||jsonText[pos]>'2'){outError="Unsupported 3D cap style";return false;}
        outPreset.realisticCapStyle=jsonText[pos++]-'0';skipSpace();
        if(pos>=jsonText.size()||(jsonText[pos]!=','&&jsonText[pos]!='}')||jsonText.find(capKey,pos)!=std::string::npos){outError="Malformed 3D cap style";return false;}
    }

    // Additive fit/style fields are strict JSON numbers; absent fields preserve old presets.
    auto optionalNumber=[&](const std::string& key,float fallback,float& value)->bool {
        value=fallback;const std::string quoted="\""+key+"\"";const auto at=jsonText.find(quoted);
        if(at==std::string::npos)return true;
        size_t pos=at+quoted.size();while(pos<jsonText.size()&&std::isspace(static_cast<unsigned char>(jsonText[pos])))++pos;
        if(pos>=jsonText.size()||jsonText[pos++]!=':'||jsonText.find(quoted,pos)!=std::string::npos){outError="Malformed "+key;return false;}
        const std::string tail=jsonText.substr(pos);std::smatch match;
        const std::regex number(R"(\s*(-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)\s*[,}])");
        if(!std::regex_search(tail,match,number,std::regex_constants::match_continuous)){outError="Malformed "+key;return false;}
        try{value=std::stof(match[1].str());}catch(...){outError="Invalid "+key;return false;}
        if(!IsFinite(value)){outError="Non-finite "+key;return false;}return true;
    };
    float glassesStyle=0;
    if(!optionalNumber("realisticGlassesStyle",0,glassesStyle)||glassesStyle<0||glassesStyle>3||std::floor(glassesStyle)!=glassesStyle){outError="Unsupported 3D glasses style";return false;}
    outPreset.realisticGlassesStyle=static_cast<int>(glassesStyle);
    if(!optionalNumber("realisticCapScale",1,outPreset.realisticCapScale)||!optionalNumber("realisticCapVerticalOffset",0,outPreset.realisticCapVerticalOffset))return false;
    float faceFilterStyle=0;
    if(!optionalNumber("faceFilterStyle",0,faceFilterStyle)||faceFilterStyle<0||faceFilterStyle>5||std::floor(faceFilterStyle)!=faceFilterStyle){outError="Unsupported 3D face filter";return false;}
    outPreset.faceFilterStyle=static_cast<int>(faceFilterStyle);
    float renderEdge=640;
    if(!optionalNumber("faceFilterRenderEdge",640,renderEdge)||renderEdge<256||renderEdge>Overlays::Realistic::HardModelRenderEdge||std::floor(renderEdge)!=renderEdge){outError="Unsupported face filter raster budget";return false;}
    outPreset.faceFilterRenderEdge=static_cast<uint32_t>(renderEdge);
    if(!optionalNumber("faceFilterScale",1,outPreset.faceFilterScale)||!optionalNumber("faceFilterVerticalOffset",0,outPreset.faceFilterVerticalOffset))return false;

    auto optionalString=[&](const std::string& key,std::string& value)->bool {
        value.clear();const std::string quoted="\""+key+"\"";const auto at=jsonText.find(quoted);if(at==std::string::npos)return true;
        size_t pos=at+quoted.size();auto space=[&]{while(pos<jsonText.size()&&std::isspace(static_cast<unsigned char>(jsonText[pos])))++pos;};
        space();if(pos>=jsonText.size()||jsonText[pos++]!=':'){outError="Malformed "+key;return false;}space();
        if(pos>=jsonText.size()||jsonText[pos++]!='"'){outError="Malformed "+key;return false;}
        auto utf8=[&](unsigned code){if(code<128)value+=char(code);else if(code<2048){value+=char(0xc0|(code>>6));value+=char(0x80|(code&63));}
            else if(code<65536){value+=char(0xe0|(code>>12));value+=char(0x80|((code>>6)&63));value+=char(0x80|(code&63));}
            else{value+=char(0xf0|(code>>18));value+=char(0x80|((code>>12)&63));value+=char(0x80|((code>>6)&63));value+=char(0x80|(code&63));}};
        auto hex4=[&](unsigned& code){code=0;for(int i=0;i<4;++i){if(pos>=jsonText.size())return false;const unsigned char h=jsonText[pos++];
            const int v=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;if(v<0)return false;code=(code<<4)|unsigned(v);}return true;};
        bool ended=false;while(pos<jsonText.size()){unsigned char ch=jsonText[pos++];if(ch=='"'){ended=true;break;}if(ch<32){outError="Invalid "+key;return false;}
            if(ch=='\\'){if(pos>=jsonText.size()){outError="Malformed "+key;return false;}ch=jsonText[pos++];
                if(ch=='u'){unsigned code;if(!hex4(code)){outError="Malformed Unicode "+key;return false;}
                    if(code>=0xd800&&code<=0xdbff){if(pos+2>jsonText.size()||jsonText[pos++]!='\\'||jsonText[pos++]!='u'){outError="Invalid Unicode "+key;return false;}
                        unsigned low;if(!hex4(low)||low<0xdc00||low>0xdfff){outError="Invalid Unicode "+key;return false;}code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}
                    else if(code>=0xdc00&&code<=0xdfff){outError="Invalid Unicode "+key;return false;}utf8(code);continue;}
                if(ch=='n')ch='\n';else if(ch=='r')ch='\r';else if(ch=='t')ch='\t';else if(ch=='b')ch='\b';else if(ch=='f')ch='\f';
                else if(ch!='\\'&&ch!='/'&&ch!='"'){outError="Invalid escape "+key;return false;}}
            value+=char(ch);if(value.size()>4096){outError="Oversized "+key;return false;}}
        space();if(!ended||pos>=jsonText.size()||(jsonText[pos]!=','&&jsonText[pos]!='}')||jsonText.find(quoted,pos)!=std::string::npos){outError="Malformed "+key;return false;}return true;
    };
    if(!optionalString("faceFilterCustomPath",outPreset.faceFilterCustomPath)||!optionalString("faceFilterCustomName",outPreset.faceFilterCustomName))return false;
    if(!optionalNumber("faceFilterCustomX",0,outPreset.faceFilterCustomFit.xCm)||!optionalNumber("faceFilterCustomY",0,outPreset.faceFilterCustomFit.yCm)||
       !optionalNumber("faceFilterCustomZ",0,outPreset.faceFilterCustomFit.zCm)||!optionalNumber("faceFilterCustomTilt",0,outPreset.faceFilterCustomFit.tiltDegrees)||
       !optionalNumber("faceFilterCustomTurn",0,outPreset.faceFilterCustomFit.turnDegrees)||!optionalNumber("faceFilterCustomRoll",0,outPreset.faceFilterCustomFit.rollDegrees))return false;
    const std::string occlusionKey="\"faceFilterCustomOcclusion\"";const auto occlusionAt=jsonText.find(occlusionKey);
    if(occlusionAt!=std::string::npos){size_t pos=occlusionAt+occlusionKey.size();while(pos<jsonText.size()&&std::isspace(static_cast<unsigned char>(jsonText[pos])))++pos;
        if(pos>=jsonText.size()||jsonText[pos++]!=':'||jsonText.find(occlusionKey,pos)!=std::string::npos){outError="Malformed local occlusion";return false;}
        while(pos<jsonText.size()&&std::isspace(static_cast<unsigned char>(jsonText[pos])))++pos;
        if(jsonText.compare(pos,4,"true")==0){outPreset.faceFilterCustomFit.headOcclusion=true;pos+=4;}
        else if(jsonText.compare(pos,5,"false")==0){outPreset.faceFilterCustomFit.headOcclusion=false;pos+=5;}else{outError="Malformed local occlusion";return false;}
        while(pos<jsonText.size()&&std::isspace(static_cast<unsigned char>(jsonText[pos])))++pos;
        if(pos>=jsonText.size()||(jsonText[pos]!=','&&jsonText[pos]!='}')){outError="Malformed local occlusion";return false;}}

    std::string colourBlock=findBlock("inputColour");
    if(!colourBlock.empty()) {
        auto strictNumber = [&](const char* key, float defaultValue, float& value) {
            const std::string pattern="\""+std::string(key)+"\":";
            size_t pos=colourBlock.find(pattern); if(pos==std::string::npos){value=defaultValue;return true;}
            pos+=pattern.size(); while(pos<colourBlock.size() && std::isspace(static_cast<unsigned char>(colourBlock[pos])))++pos;
            size_t end=colourBlock.find_first_of(",}",pos);
            if(end==std::string::npos) return false;
            std::string token=colourBlock.substr(pos,end-pos);
            std::smatch numeric;
            static const std::regex grammar("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?[ \\t\\r\\n]*");
            if(!std::regex_match(token,numeric,grammar)) return false;
            try { value=std::stof(token); return std::isfinite(value); } catch(...) {return false;}
        };
        float look=0;
        if(!strictNumber("look",0,look) || look<0 || look>3 || std::floor(look)!=look ||
            !strictNumber("strength",1,outPreset.inputLookStrength) ||
            !strictNumber("lutIntensity",0,outPreset.inputLutIntensity)) {
            outError="Malformed/non-finite inputColour numeric field"; return false;
        }
        outPreset.inputLook=static_cast<int>(look);
        outPreset.selectedInputLutPath=findStringIn(colourBlock,"lutPath");
    }

    // Appearance
    std::string appBlock = findBlock("appearance");
    if (!appBlock.empty()) {
        outPreset.appearance.bypass = findBoolIn(appBlock, "bypass", false);
        outPreset.appearance.smoothingStrength = SafeClamp(findFloatIn(appBlock, "smoothingStrength", 0.0f), 0.0f, 1.0f, 0.0f);
        outPreset.appearance.touchupBoost = SafeClamp(findFloatIn(appBlock, "touchupBoost", 0.0f), 0.0f, 1.0f, 0.0f);
        outPreset.appearance.detailPreservation = SafeClamp(findFloatIn(appBlock, "detailPreservation", 0.5f), 0.0f, 1.0f, 0.5f);
        outPreset.appearance.sharpenStrength = SafeClamp(findFloatIn(appBlock, "sharpenStrength", 0.0f), 0.0f, 1.0f, 0.0f);
    }

    // Lighting
    std::string lightBlock = findBlock("lighting");
    if (!lightBlock.empty()) {
        outPreset.lighting.bypass = findBoolIn(lightBlock, "bypass", false);
        outPreset.lighting.globalStrength = SafeClamp(findFloatIn(lightBlock, "globalStrength", 1.0f), 0.0f, 2.0f, 1.0f);
        outPreset.lighting.ambientIntensity = SafeClamp(findFloatIn(lightBlock, "ambientIntensity", 1.0f), 0.0f, 2.0f, 1.0f);
        outPreset.lighting.fillIntensity = SafeClamp(findFloatIn(lightBlock, "fillIntensity", 0.0f), 0.0f, 2.0f, 0.0f);

        outPreset.lighting.lightingMode = static_cast<Lighting::LightingMode>(
            static_cast<int>(findFloatIn(lightBlock, "lightingMode", 0.0f))
        );

        auto parseLight = [&](const std::string& parent, const std::string& lightKey, Lighting::StudioLight& l) {
            std::string pattern = "\"" + lightKey + "\":";
            size_t pos = parent.find(pattern);
            if (pos != std::string::npos) {
                pos = parent.find('{', pos);
                size_t endPos = parent.find('}', pos);
                if (pos != std::string::npos && endPos != std::string::npos) {
                    std::string lb = parent.substr(pos, endPos - pos + 1);
                    l.enabled = findBoolIn(lb, "enabled", false);
                    l.posX = SafeClamp(findFloatIn(lb, "posX", 0.0f), -1.0f, 1.0f, 0.0f);
                    l.posY = SafeClamp(findFloatIn(lb, "posY", 0.0f), -1.0f, 1.0f, 0.0f);
                    l.posZ = SafeClamp(findFloatIn(lb, "posZ", 1.0f), 0.1f, 5.0f, 1.0f);
                    l.colorR = SafeClamp(findFloatIn(lb, "colorR", 1.0f), 0.0f, 1.0f, 1.0f);
                    l.colorG = SafeClamp(findFloatIn(lb, "colorG", 1.0f), 0.0f, 1.0f, 1.0f);
                    l.colorB = SafeClamp(findFloatIn(lb, "colorB", 1.0f), 0.0f, 1.0f, 1.0f);
                    l.colorTemperature = SafeClamp(findFloatIn(lb, "colorTemperature", 0.0f), 0.0f, 20000.0f, 0.0f);
                    l.tint = SafeClamp(findFloatIn(lb, "tint", 0.0f), -1.0f, 1.0f, 0.0f);
                    l.intensity = SafeClamp(findFloatIn(lb, "intensity", 0.0f), 0.0f, 2.0f, 0.0f);
                }
            }
        };

        parseLight(lightBlock, "light1", outPreset.lighting.light1);
        parseLight(lightBlock, "light2", outPreset.lighting.light2);

        std::string shadowPattern = "\"shadows\":";
        size_t sPos = lightBlock.find(shadowPattern);
        if (sPos != std::string::npos) {
            sPos = lightBlock.find('{', sPos);
            size_t sendPos = lightBlock.find('}', sPos);
            if (sPos != std::string::npos && sendPos != std::string::npos) {
                std::string sb = lightBlock.substr(sPos, sendPos - sPos + 1);
                outPreset.lighting.shadowConfig.enabled = findBoolIn(sb, "enabled", false);
                outPreset.lighting.shadowConfig.maxSteps = static_cast<int>(SafeClamp(findFloatIn(sb, "maxSteps", 16.0f), 4.0f, 32.0f, 16.0f));
                outPreset.lighting.shadowConfig.maxDistance = SafeClamp(findFloatIn(sb, "maxDistance", 0.25f), 0.05f, 0.50f, 0.25f);
                outPreset.lighting.shadowConfig.rayElevation = SafeClamp(findFloatIn(sb, "rayElevation", 0.05f), 0.01f, 0.20f, 0.05f);
                outPreset.lighting.shadowConfig.shadowIntensity = SafeClamp(findFloatIn(sb, "shadowIntensity", 0.60f), 0.0f, 1.0f, 0.60f);
                outPreset.lighting.shadowConfig.shadowSoftness = SafeClamp(findFloatIn(sb, "shadowSoftness", 0.50f), 0.1f, 1.0f, 0.50f);
            }
        }
    }

    // Overlays
    std::string ovBlock = findBlock("overlays");
    if (!ovBlock.empty()) {
        outPreset.overlays.bypass = findBoolIn(ovBlock, "bypass", false);

        auto parseOvItem = [&](const std::string& parent, const std::string& itemKey, bool& enabled, float& opacity) {
            std::string pattern = "\"" + itemKey + "\":";
            size_t pos = parent.find(pattern);
            if (pos != std::string::npos) {
                pos = parent.find('{', pos);
                size_t endPos = parent.find('}', pos);
                if (pos != std::string::npos && endPos != std::string::npos) {
                    std::string ob = parent.substr(pos, endPos - pos + 1);
                    enabled = findBoolIn(ob, "enabled", false);
                    opacity = SafeClamp(findFloatIn(ob, "opacity", 1.0f), 0.0f, 1.0f, 1.0f);
                }
            }
        };

        parseOvItem(ovBlock, "glasses", outPreset.overlays.glasses.enabled, outPreset.overlays.glasses.baseOpacity);
        parseOvItem(ovBlock, "hat", outPreset.overlays.hat.enabled, outPreset.overlays.hat.baseOpacity);
        parseOvItem(ovBlock, "studioFrame", outPreset.overlays.studioFrame.enabled, outPreset.overlays.studioFrame.opacity);
    }

    // Background & LUT
    std::string bgBlock = findBlock("background");
    if (!bgBlock.empty()) {
        int mode = static_cast<int>(findFloatIn(bgBlock, "mode", 0.0f));
        int fitMode = static_cast<int>(findFloatIn(bgBlock, "fitMode", 0.0f));
        outPreset.backgroundMode = static_cast<StudioBackgroundMode>(std::clamp(mode, 0, 3));
        outPreset.backgroundFitMode = static_cast<BackgroundFitMode>(std::clamp(fitMode, 0, 2));
        if (mode <= 2) {
            outPreset.background.mode = static_cast<BackgroundMode>(mode);
        } else {
            outPreset.background.mode = BackgroundMode::Original;
        }
    }

    std::string lutBlock = findBlock("lut");
    if (!lutBlock.empty()) {
        outPreset.selectedLutPath = findStringIn(lutBlock, "path");
        outPreset.lutIntensity = SafeClamp(findFloatIn(lutBlock, "intensity", 0.0f), 0.0f, 1.0f, 0.0f);
    }

    // Pack reference
    std::string packBlock = findBlock("packReference");
    if (!packBlock.empty()) {
        outPreset.packIdReference = findStringIn(packBlock, "packId");
        outPreset.packVersionReference = findStringIn(packBlock, "version");
    }

    if (!outPreset.Validate(outError)) return false;
    destination=std::move(outPreset);
    return true;
}

bool PresetStore::SavePreset(const std::string& storeDir, const StudioPreset& preset, std::string& outError) {
    if (preset.presetId.empty()) {
        outError = "Cannot save preset with empty presetId";
        return false;
    }

    fs::path dir(storeDir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        outError = "Failed to create preset store directory: " + ec.message();
        return false;
    }

    std::string json = Serialize(preset);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmpPath = dir / (preset.presetId + ".tmp_" + std::to_string(now));
    fs::path finalPath = dir / (preset.presetId + ".json");

    {
        std::ofstream file(tmpPath.string(), std::ios::binary);
        if (!file.is_open()) {
            outError = "Failed to open temporary file for write: " + tmpPath.string();
            return false;
        }
        file.write(json.data(), json.size());
        file.flush();
    }

    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        outError = "Failed to atomically commit preset file: " + ec.message();
        return false;
    }

    return true;
}

bool PresetStore::LoadPreset(const std::string& filePath, StudioPreset& outPreset, std::string& outError) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        outError = "Cannot open preset file: " + filePath;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return Deserialize(content, outPreset, outError);
}

bool PresetStore::DeletePreset(const std::string& storeDir, const std::string& presetId, std::string& outError) {
    fs::path target = fs::path(storeDir) / (presetId + ".json");
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        outError = "Preset file not found: " + presetId;
        return false;
    }
    fs::remove(target, ec);
    if (ec) {
        outError = "Failed to delete preset file: " + ec.message();
        return false;
    }
    return true;
}

std::vector<StudioPreset> PresetStore::EnumeratePresets(const std::string& storeDir) {
    std::vector<StudioPreset> results;
    fs::path dir(storeDir);
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return results;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".json") {
            StudioPreset p;
            std::string err;
            if (LoadPreset(entry.path().string(), p, err)) {
                results.push_back(p);
            }
        }
    }
    return results;
}

} // namespace PrismForge::Effects::Presets
