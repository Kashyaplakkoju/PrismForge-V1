#include "face_geometry.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace PrismForge::Providers {
namespace {
constexpr size_t kSurfaceVertices = 468;
constexpr size_t kObservationVertices = 478;
constexpr size_t kMaxTriangles = 2048;
constexpr size_t kMaxObjBytes = 1024 * 1024;
constexpr size_t kMaxObjLine = 1024;
constexpr int kMaxSourceDimension = 8192;
constexpr int kMaxRasterDimension = 2048;
constexpr size_t kMaxRasterPixels = 4 * 1024 * 1024;
constexpr double kDepthOffset = 0.5;
constexpr double kMinProjectedArea = 1e-12;
struct Vector3 { double x = 0, y = 0, z = 0; };

bool FiniteNumber(const std::string& token, double& out) {
    if (token.empty() || token.size() > 128) return false;
    char* end = nullptr; errno = 0;
    out = std::strtod(token.c_str(), &end);
    return end == token.c_str() + token.size() && errno != ERANGE && std::isfinite(out) && std::abs(out) <= 1e6;
}
bool Index(const std::string& text, uint32_t& out) {
    if (text.empty() || text.size() > 10) return false;
    uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<unsigned>(ch - '0');
        if (value > 2048) return false;
    }
    if (!value) return false;
    out = static_cast<uint32_t>(value - 1); return true;
}
struct ObjReference { uint32_t vertex = 0, texture = 0, normal = 0; bool hasTexture = false, hasNormal = false; };
bool Reference(const std::string& token, ObjReference& out) {
    const size_t first = token.find('/');
    if (first == std::string::npos) return Index(token, out.vertex);
    if (!Index(token.substr(0, first), out.vertex)) return false;
    const size_t second = token.find('/', first + 1);
    if (second == std::string::npos) {
        out.hasTexture = true; return Index(token.substr(first + 1), out.texture);
    }
    if (token.find('/', second + 1) != std::string::npos) return false;
    if (second > first + 1) {
        out.hasTexture = true;
        if (!Index(token.substr(first + 1, second - first - 1), out.texture)) return false;
    }
    out.hasNormal = true;
    return Index(token.substr(second + 1), out.normal);
}
uint32_t TriangleKey(std::array<uint32_t, 3> triangle) {
    std::sort(triangle.begin(), triangle.end());
    return (triangle[0] * 468 + triangle[1]) * 468 + triangle[2];
}
Vector3 Unit(Vector3 n) {
    const double length = std::hypot(n.x, n.y, n.z);
    if (!(length > 1e-15) || !std::isfinite(length)) return {};
    n.x /= length; n.y /= length; n.z /= length; return n;
}
bool Coordinate(const FacePoint3D& p) {
    // Moderate off-screen vertices are valid and are clipped during rasterization.
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
        std::abs(p.x) <= 16 && std::abs(p.y) <= 16 && std::abs(p.z) <= 16;
}
Effects::AnalysisBridge::Landmark2D Anchor(const FaceObservation& observation, size_t first, size_t second) {
    const auto& a = observation.vertices[first]; const auto& b = observation.vertices[second];
    Effects::AnalysisBridge::Landmark2D result;
    result.x = std::clamp((a.x + b.x) * 0.5f, 0.0f, 1.0f);
    result.y = std::clamp((a.y + b.y) * 0.5f, 0.0f, 1.0f);
    // The pinned runtime reports detection, not a calibrated per-point probability.
    result.confidence = 0;
    return result;
}

// Feature indices follow MediaPipe v0.10.32 FACEMESH eye/lip/brow connections:
// https://github.com/google-ai-edge/mediapipe/blob/v0.10.32/mediapipe/python/solutions/face_mesh_connections.py
// Copyright 2021 The MediaPipe Authors, Apache-2.0 (bundled runtime license).
struct Point2 { double x=0,y=0; };
double Cross(Point2 a,Point2 b,Point2 c) {return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
std::vector<Point2> ConvexHull(std::vector<Point2> points) {
    std::sort(points.begin(),points.end(),[](Point2 a,Point2 b){return a.x==b.x?a.y<b.y:a.x<b.x;});
    points.erase(std::unique(points.begin(),points.end(),[](Point2 a,Point2 b){return a.x==b.x&&a.y==b.y;}),points.end());
    if(points.size()<3)return points;
    std::vector<Point2> hull;
    for(const auto& p:points) {while(hull.size()>1&&Cross(hull[hull.size()-2],hull.back(),p)<=0)hull.pop_back();hull.push_back(p);}
    const auto lower=hull.size();
    for(size_t i=points.size()-1;i-->0;) {const auto p=points[i];while(hull.size()>lower&&Cross(hull[hull.size()-2],hull.back(),p)<=0)hull.pop_back();hull.push_back(p);}
    hull.pop_back();return hull;
}
double SegmentDistance(Point2 p,Point2 a,Point2 b) {
    const double dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
    const double t=length>1e-12?std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/length,0.0,1.0):0.0;
    return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
template<size_t N> void ProtectFeature(const FaceObservation& o,const std::array<size_t,N>& indices,
    Effects::AnalysisBridge::MaskBuffer& mask,double padding,bool hull=false) {
    std::vector<Point2> polygon;polygon.reserve(N);
    double minX=mask.width,maxX=0,minY=mask.height,maxY=0;
    for(size_t i:indices) {Point2 p{double(o.vertices[i].x)*mask.width,double(o.vertices[i].y)*mask.height};
        polygon.push_back(p);minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
    if(hull)polygon=ConvexHull(std::move(polygon));
    if(polygon.empty()||maxX+padding<0||maxY+padding<0||minX-padding>=mask.width||minY-padding>=mask.height)return;
    const int left=int(std::max(0.0,std::floor(minX-padding))),right=int(std::min(double(mask.width-1),std::ceil(maxX+padding)));
    const int top=int(std::max(0.0,std::floor(minY-padding))),bottom=int(std::min(double(mask.height-1),std::ceil(maxY+padding)));
    for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x) {
        Point2 p{x+.5,y+.5};bool inside=false;double distance=std::numeric_limits<double>::max();
        for(size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
            const auto a=polygon[i],b=polygon[j];distance=std::min(distance,SegmentDistance(p,a,b));
            if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
        }
        if(inside||distance<=padding)mask.data[size_t(y)*mask.strideFloats+x]=1.f;
    }
}
void TouchupMasks(const FaceObservation& observation,FaceGeometry& geometry) {
    const auto& coverage=geometry.coverage;
    auto& skin=geometry.touchupSurface;skin=coverage;
    auto& protect=geometry.featureProtection;protect=coverage;std::fill(protect.data.begin(),protect.data.end(),0.f);
    // Padded protection survives the later bilinear scaling to the source frame.
    // Relative to inter-eye distance, capped independently of source/raster size.
    const double eyes=std::hypot(double(observation.vertices[33].x-observation.vertices[263].x)*coverage.width,
        double(observation.vertices[33].y-observation.vertices[263].y)*coverage.height);
    const double padding=std::clamp(eyes*.035,1.5,12.0);
    ProtectFeature(observation,std::array<size_t,16>{33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246},protect,padding);
    ProtectFeature(observation,std::array<size_t,16>{263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466},protect,padding);
    ProtectFeature(observation,std::array<size_t,20>{61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185},protect,padding);
    ProtectFeature(observation,std::array<size_t,10>{46,53,52,65,55,70,63,105,66,107},protect,padding,true);
    ProtectFeature(observation,std::array<size_t,10>{276,283,282,295,285,300,293,334,296,336},protect,padding,true);
    // A bounded inward silhouette margin avoids leaking smoothing onto room/hair
    // pixels through bilinear interpolation. This is not hair segmentation.
    for(int y=0;y<coverage.height;++y)for(int x=0;x<coverage.width;++x) {
        const size_t at=size_t(y)*coverage.strideFloats+x;
        if(coverage.data[at]<=0||protect.data[at]>=1.f) {skin.data[at]=0.f;continue;}
        float margin=1.f;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) {
            const int nx=x+dx,ny=y+dy;
            if(nx<0||ny<0||nx>=coverage.width||ny>=coverage.height||coverage.data[size_t(ny)*coverage.strideFloats+nx]<=0) {
                const float distance=float(std::max(std::abs(dx),std::abs(dy)));
                margin=std::min(margin,std::max(0.f,(distance-1.f)*.5f));
            }
        }
        skin.data[at]=coverage.data[at]*margin;
    }
}

void Anchors(const FaceObservation& observation, Effects::AnalysisBridge::FaceLandmarkBundle& anchors) {
    anchors.detected = true; anchors.overallConfidence = 0;
    // Left/right here are preview-screen anchors for the unmirrored canonical face.
    anchors.leftEye = Anchor(observation, 33, 133);
    anchors.rightEye = Anchor(observation, 263, 362);
    anchors.noseBridge = Anchor(observation, 168, 168);
    anchors.noseTip = Anchor(observation, 1, 1);
    anchors.mouthCenter = Anchor(observation, 13, 14);
    anchors.mouthLeft = Anchor(observation, 61, 61);
    anchors.mouthRight = Anchor(observation, 291, 291);
    anchors.chinTip = Anchor(observation, 152, 152);
    anchors.foreheadCenter = Anchor(observation, 10, 10);
    // Do not turn coordinate clamping into accessory tracking evidence. Every
    // original anchor must be on-screen before accepting this optional route.
    bool onScreen = true;
    for (size_t index : {size_t(33),size_t(133),size_t(263),size_t(362),size_t(168),size_t(10)}) {
        const auto& p=observation.vertices[index];
        onScreen = onScreen && p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1;
    }
    if (onScreen) {
        anchors.evidence=Effects::AnalysisBridge::FaceLandmarkEvidence::ValidatedNativeGeometry;
        anchors.geometryRevision=1;
        anchors.geometrySourceWidth=observation.imageWidth;anchors.geometrySourceHeight=observation.imageHeight;
        anchors.geometrySourceGeneration=observation.tags.generation;anchors.geometrySequenceId=observation.tags.sequence;
        anchors.geometryTimestampPts=observation.tags.microPts;
        if (!anchors.HasValidatedNativeGeometry(observation.imageWidth,observation.imageHeight)) {
            anchors.evidence=Effects::AnalysisBridge::FaceLandmarkEvidence::ConfidenceScores;
            anchors.geometryRevision=0;
        }
    }
    constexpr std::array<size_t, 36> oval = {10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,
        152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109};
    anchors.perimeterContour.reserve(oval.size());
    for (size_t index : oval) anchors.perimeterContour.push_back(Anchor(observation, index, index));
}
}

bool LoadFaceTopology(const std::wstring& path, FaceTopology& outTopology, std::string& error) {
    outTopology = {}; error.clear();
    try {
        std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
        if (!input) { error = "Could not open local canonical face topology"; return false; }
        const auto size = input.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(kMaxObjBytes)) { error = "Canonical OBJ exceeds bounded size"; return false; }
        std::string text(static_cast<size_t>(size), '\0'); input.seekg(0);
        if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) || text.find('\0') != std::string::npos) {
            error = "Canonical OBJ contains incomplete or invalid text"; return false;
        }
        size_t vertexCount = 0, textureCount = 0, normalCount = 0;
        std::vector<std::array<ObjReference, 3>> faces;
        std::set<uint32_t> uniqueTriangles;
        std::istringstream lines(text); std::string line;
        while (std::getline(lines, line)) {
            if (line.size() > kMaxObjLine) { error = "Canonical OBJ line exceeds bounded size"; return false; }
            const auto comment = line.find('#'); if (comment != std::string::npos) line.resize(comment);
            std::istringstream fields(line); std::string kind; if (!(fields >> kind)) continue;
            std::vector<std::string> tokens; std::string token;
            while (fields >> token) { if (tokens.size() >= 4) { error = "Canonical OBJ record has too many fields"; return false; } tokens.push_back(token); }
            if (kind == "v" || kind == "vt" || kind == "vn") {
                const size_t expected = kind == "vt" ? 2 : 3;
                if (tokens.size() != expected) { error = "Invalid canonical OBJ coordinate record"; return false; }
                double ignored; for (const auto& value : tokens) if (!FiniteNumber(value, ignored)) { error = "Nonfinite canonical OBJ coordinate"; return false; }
                if (kind == "v") { if (++vertexCount > kSurfaceVertices) { error = "Canonical face must have exactly 468 surface vertices"; return false; } }
                else if (kind == "vt") { if (++textureCount > 2048) { error = "Canonical OBJ texture coordinates exceed bound"; return false; } }
                else if (++normalCount > 2048) { error = "Canonical OBJ normal coordinates exceed bound"; return false; }
            } else if (kind == "f") {
                if (tokens.size() != 3 || faces.size() >= kMaxTriangles) { error = "Canonical face requires bounded triangular faces"; return false; }
                std::array<ObjReference, 3> face;
                for (size_t i = 0; i < 3; ++i) if (!Reference(tokens[i], face[i]) || face[i].vertex >= kSurfaceVertices) {
                    error = "Invalid canonical OBJ face index"; return false;
                }
                std::array<uint32_t, 3> indices = {face[0].vertex, face[1].vertex, face[2].vertex};
                if (indices[0] == indices[1] || indices[0] == indices[2] || indices[1] == indices[2] ||
                    !uniqueTriangles.insert(TriangleKey(indices)).second) { error = "Canonical OBJ contains repeated or degenerate face indices"; return false; }
                faces.push_back(face);
            } else { error = "Unsupported canonical OBJ directive"; return false; }
        }
        if (vertexCount != kSurfaceVertices || faces.empty()) { error = "Canonical face requires 468 vertices and triangular surface faces"; return false; }
        FaceTopology candidate; candidate.triangles.reserve(faces.size());
        for (const auto& face : faces) {
            for (const auto& ref : face) if (ref.vertex >= vertexCount || (ref.hasTexture && ref.texture >= textureCount) ||
                (ref.hasNormal && ref.normal >= normalCount)) { error = "Canonical OBJ face references missing data"; return false; }
            candidate.triangles.push_back({face[0].vertex, face[1].vertex, face[2].vertex});
        }
        outTopology = std::move(candidate); return true;
    } catch (...) { outTopology = {}; error = "Could not allocate or parse canonical face topology"; return false; }
}

bool BuildFaceGeometry(const FaceObservation& observation, const FaceTopology& topology,
    int width, int height, FaceGeometry& outGeometry, std::string& error) {
    outGeometry = {}; error.clear();
    if (!observation.detected) { error = "No face detected"; return false; }
    if (observation.imageWidth <= 0 || observation.imageHeight <= 0 || observation.imageWidth > kMaxSourceDimension ||
        observation.imageHeight > kMaxSourceDimension || width <= 0 || height <= 0 || width > kMaxRasterDimension ||
        height > kMaxRasterDimension || static_cast<size_t>(width) * height > kMaxRasterPixels ||
        observation.vertices.size() != kObservationVertices || topology.triangles.empty() || topology.triangles.size() > kMaxTriangles) {
        error = "Invalid bounded face observation or raster dimensions"; return false;
    }
    for (const auto& p : observation.vertices) if (!Coordinate(p)) { error = "Invalid face landmark coordinate"; return false; }
    for (const auto& tri : topology.triangles) for (uint32_t index : tri) if (index >= kSurfaceVertices) {
        error = "Face topology references an unsupported surface vertex"; return false;
    }
    try {
        std::array<Vector3, kSurfaceVertices> sums{};
        for (const auto& tri : topology.triangles) {
            const auto& p = observation.vertices[tri[0]];
            const auto& q = observation.vertices[tri[1]];
            const auto& r = observation.vertices[tri[2]];
            const double ax = (double(q.x) - p.x) * observation.imageWidth;
            const double ay = (double(q.y) - p.y) * observation.imageHeight;
            const double az = -(double(q.z) - p.z) * observation.imageWidth;
            const double bx = (double(r.x) - p.x) * observation.imageWidth;
            const double by = (double(r.y) - p.y) * observation.imageHeight;
            const double bz = -(double(r.z) - p.z) * observation.imageWidth;
            Vector3 normal{ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
            // Camera-facing screen surface; either OBJ winding produces identical normals.
            if (normal.z < 0) { normal.x = -normal.x; normal.y = -normal.y; normal.z = -normal.z; }
            if (normal.z <= kMinProjectedArea) continue;
            for (uint32_t index : tri) { sums[index].x += normal.x; sums[index].y += normal.y; sums[index].z += normal.z; }
        }
        for (auto& n : sums) n = Unit(n);
        FaceGeometry candidate; candidate.tags = observation.tags;
        const size_t pixels = static_cast<size_t>(width) * height;
        auto& normals = candidate.normals; normals.width = width; normals.height = height; normals.strideFloats = width * 3;
        normals.data.assign(pixels * 3, 0);
        auto& depth = candidate.depth; depth.width = width; depth.height = height; depth.strideFloats = width;
        depth.format = 0; depth.depthScale = 1;
        depth.sourceGeneration = observation.tags.generation; depth.sequenceId = observation.tags.sequence; depth.timestampPts = observation.tags.microPts;
        depth.data.assign(pixels, Effects::AnalysisBridge::DepthBuffer::kDefaultInvalidSentinel); depth.confidence.assign(pixels, 0);
        auto& coverage = candidate.coverage; coverage.width = width; coverage.height = height; coverage.strideFloats = width;
        coverage.data.assign(pixels, 0);
        std::vector<uint32_t> winners(pixels, std::numeric_limits<uint32_t>::max());
        size_t covered = 0;
        for (const auto& tri : topology.triangles) {
            const auto& p = observation.vertices[tri[0]]; const auto& q = observation.vertices[tri[1]]; const auto& r = observation.vertices[tri[2]];
            const double denominator = (double(q.y) - r.y) * (double(p.x) - r.x) + (double(r.x) - q.x) * (double(p.y) - r.y);
            if (std::abs(denominator) <= kMinProjectedArea) continue;
            const double minU = std::max(0.0, std::min({double(p.x), double(q.x), double(r.x)}));
            const double maxU = std::min(1.0, std::max({double(p.x), double(q.x), double(r.x)}));
            const double minV = std::max(0.0, std::min({double(p.y), double(q.y), double(r.y)}));
            const double maxV = std::min(1.0, std::max({double(p.y), double(q.y), double(r.y)}));
            if (minU > maxU || minV > maxV) continue;
            const int minX = std::clamp(static_cast<int>(std::ceil(minU * width - 0.5)), 0, width - 1);
            const int maxX = std::clamp(static_cast<int>(std::floor(maxU * width - 0.5)), 0, width - 1);
            const int minY = std::clamp(static_cast<int>(std::ceil(minV * height - 0.5)), 0, height - 1);
            const int maxY = std::clamp(static_cast<int>(std::floor(maxV * height - 0.5)), 0, height - 1);
            const uint32_t key = TriangleKey(tri);
            for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
                const double u = (x + 0.5) / width, v = (y + 0.5) / height;
                const double a = ((double(q.y) - r.y) * (u - r.x) + (double(r.x) - q.x) * (v - r.y)) / denominator;
                const double b = ((double(r.y) - p.y) * (u - r.x) + (double(p.x) - r.x) * (v - r.y)) / denominator;
                const double c = 1 - a - b;
                if (a < -1e-10 || b < -1e-10 || c < -1e-10) continue;
                const double relative = kDepthOffset + a * p.z + b * q.z + c * r.z;
                if (!std::isfinite(relative) || relative < 0 || relative > 1) continue;
                const float sampleDepth = static_cast<float>(relative);
                const size_t at = static_cast<size_t>(y) * width + x;
                // Stable index tie-break makes coincident surfaces independent of face iteration order.
                if (coverage.data[at] && (sampleDepth > depth.data[at] ||
                    (sampleDepth == depth.data[at] && key >= winners[at]))) continue;
                Vector3 n{a * sums[tri[0]].x + b * sums[tri[1]].x + c * sums[tri[2]].x,
                    a * sums[tri[0]].y + b * sums[tri[1]].y + c * sums[tri[2]].y,
                    a * sums[tri[0]].z + b * sums[tri[1]].z + c * sums[tri[2]].z};
                n = Unit(n); if (!(n.z > 0)) continue;
                if (!coverage.data[at]) ++covered;
                coverage.data[at] = depth.confidence[at] = 1; depth.data[at] = sampleDepth; winners[at] = key;
                normals.data[at * 3] = static_cast<float>(n.x); normals.data[at * 3 + 1] = static_cast<float>(n.y); normals.data[at * 3 + 2] = static_cast<float>(n.z);
            }
        }
        if (!covered) { error = "No visible non-degenerate face surface in supported depth range"; return false; }
        TouchupMasks(observation, candidate);
        Anchors(observation, candidate.anchors);
        if(observation.hasTransform) {
            auto mesh=std::make_shared<Effects::AnalysisBridge::FaceAccessoryGeometry>();
            mesh->vertices.reserve(observation.vertices.size());
            for(const auto& p:observation.vertices)mesh->vertices.push_back({p.x,p.y,p.z});
            mesh->triangles=topology.triangles;mesh->transform=observation.transform;mesh->hasTransform=true;
            mesh->sourceWidth=observation.imageWidth;mesh->sourceHeight=observation.imageHeight;
            mesh->sourceGeneration=observation.tags.generation;mesh->sequenceId=observation.tags.sequence;mesh->timestampPts=observation.tags.microPts;
            if(mesh->Matches(observation.imageWidth,observation.imageHeight,observation.tags.generation,
                observation.tags.sequence,observation.tags.microPts))candidate.anchors.accessoryMesh=std::move(mesh);
        }
        outGeometry = std::move(candidate); return true;
    } catch (...) { outGeometry = {}; error = "Could not allocate bounded face geometry"; return false; }
}
}