// Synthetic raster/adapter verification only. This executable performs no model
// inference and makes no claim about facial quality or live performance.
#include "providers/face_geometry.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace P = PrismForge::Providers;
namespace fs = std::filesystem;
namespace {
struct Suite {
    int checks = 0, failures = 0, groups = 0, failedGroups = 0;
    void Check(bool pass, const std::string& message) {
        ++checks;
        if (!pass) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
    void Require(bool pass, const std::string& message) {
        Check(pass, message);
        if (!pass) throw std::runtime_error(message);
    }
    void Group(const char* name, const std::function<void()>& run) {
        ++groups; const int before = failures;
        try { run(); }
        catch (const std::exception& e) {
            if (before == failures) { ++failures; std::cerr << "EXCEPTION: " << e.what() << '\n'; }
        }
        if (failures != before) ++failedGroups;
        std::cout << (failures == before ? "PASS " : "FAIL ") << name << '\n';
    }
};
bool Near(float a, double b, double tolerance = 2e-5) { return std::isfinite(a) && std::abs(a-b) <= tolerance; }
P::FaceObservation Observation() {
    P::FaceObservation o; o.detected = true; o.imageWidth = 800; o.imageHeight = 400;
    o.tags = {0x123456789abcdef0ULL, 0xfedcba9876543210ULL, 1234567890123LL};
    o.vertices.assign(478, {0.5f, 0.5f, 0.0f}); return o;
}
P::FaceObservation Plane(float sx = 0, float sy = 0, float offset = 0) {
    auto o = Observation();
    o.vertices[0] = {0.125f, 0.125f, offset + 0.125f*sx + 0.125f*sy};
    o.vertices[1] = {0.875f, 0.125f, offset + 0.875f*sx + 0.125f*sy};
    o.vertices[2] = {0.125f, 0.875f, offset + 0.125f*sx + 0.875f*sy}; return o;
}
P::FaceTopology Triangle() { return {{{0,1,2}}}; }
P::FaceGeometry Build(Suite& suite, const P::FaceObservation& o, const P::FaceTopology& t, int w=64, int h=64) {
    P::FaceGeometry g; std::string error;
    suite.Require(P::BuildFaceGeometry(o,t,w,h,g,error), "geometry builds: " + error);
    suite.Require(g.normals.IsValid() && g.depth.IsValid() && g.coverage.IsValid(), "owned output buffers valid");
    suite.Check(error.empty(), "success clears error"); return g;
}
void Unsupported(Suite& suite, const P::FaceGeometry& g, int x, int y) {
    const size_t at = static_cast<size_t>(y)*g.coverage.width+x;
    suite.Check(g.coverage.data[at] == 0 && g.depth.confidence[at] == 0,
                "uncovered sample has no support/confidence");
    suite.Check(g.depth.data[at] == -1, "uncovered relative depth is invalid, not a fabricated surface");
    suite.Check(g.normals.data[at*3] == 0 && g.normals.data[at*3+1] == 0 && g.normals.data[at*3+2] == 0,
                "uncovered normal is zero");
}
void EqualGeometry(Suite& suite, const P::FaceGeometry& a, const P::FaceGeometry& b, const char* why) {
    suite.Require(a.coverage.data.size()==b.coverage.data.size(), why);
    bool equal = a.coverage.data == b.coverage.data && a.depth.confidence == b.depth.confidence;
    for (size_t i=0; i<a.depth.data.size(); ++i) equal = equal && Near(a.depth.data[i],b.depth.data[i]);
    for (size_t i=0; i<a.normals.data.size(); ++i) equal = equal && Near(a.normals.data[i],b.normals.data[i]);
    suite.Check(equal, why);
}
void Reject(Suite& suite, const P::FaceObservation& o, const P::FaceTopology& t, int w, int h, const char* why) {
    auto g = Build(suite,Plane(),Triangle()); std::string error;
    suite.Check(!P::BuildFaceGeometry(o,t,w,h,g,error), why);
    suite.Check(!error.empty(), "rejection explains failure");
    suite.Check(g.normals.data.empty() && g.depth.data.empty() && g.coverage.data.empty() && !g.anchors.detected,
                "failure removes previously successful output");
}
struct TempFiles {
    fs::path root;
    TempFiles() {
        root = fs::temp_directory_path() / ("prismforge-facegeometry-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(root)) throw std::runtime_error("Could not create isolated parser fixtures");
    }
    ~TempFiles() { std::error_code ec; fs::remove_all(root,ec); }
    fs::path Write(const std::string& name, const std::string& text) const {
        const auto file = root/name; std::ofstream stream(file,std::ios::binary);
        stream.write(text.data(),static_cast<std::streamsize>(text.size()));
        if (!stream) throw std::runtime_error("Could not write parser fixture"); return file;
    }
};
std::string ObjVertices(int count=468) {
    std::ostringstream out;
    for (int i=0;i<count;++i) out << "v 0.1 0.2 0.3\n";
    return out.str();
}
void RejectObj(Suite& suite, const TempFiles& files, const char* name, const std::string& contents) {
    P::FaceTopology topology = Triangle(); std::string error;
    suite.Check(!P::LoadFaceTopology(files.Write(name,contents).wstring(),topology,error), name);
    suite.Check(!error.empty() && topology.triangles.empty(), "parser rejects without old topology");
}
}
int main(int argc, char** argv) {
    Suite suite;
    suite.Group("flat triangle, exact frame identity and unsupported pixels", [&] {
        const auto o = Plane(); const auto g = Build(suite,o,Triangle());
        const size_t at = 16*64+16;
        suite.Check(g.tags.generation==o.tags.generation && g.tags.sequence==o.tags.sequence && g.tags.microPts==o.tags.microPts,
                    "outer identity preserves all 64-bit values");
        suite.Check(g.depth.sourceGeneration==o.tags.generation && g.depth.sequenceId==o.tags.sequence &&
                    g.depth.timestampPts==o.tags.microPts, "depth identity matches exact observation");
        suite.Check(g.depth.format==0 && g.depth.depthScale==1, "fixed relative depth convention");
        suite.Check(g.coverage.data[at]==1 && g.depth.confidence[at]==1 && Near(g.depth.data[at],0.5), "front plane fixed depth");
        suite.Check(Near(g.normals.data[at*3],0) && Near(g.normals.data[at*3+1],0) && Near(g.normals.data[at*3+2],1),
                    "front plane camera-facing unit normal");
        Unsupported(suite,g,0,0); Unsupported(suite,g,63,63);
        suite.Check(g.anchors.detected && g.anchors.perimeterContour.size()==36 && g.anchors.overallConfidence==0,
                    "anchors are detected but geometric support is not calibrated ML probability");
    });
    suite.Group("independent analytic plane slopes and source aspect", [&] {
        for (const auto dims : {std::array<int,2>{800,400}, {400,800}, {640,640}}) {
            auto o = Plane(0.15f,-0.08f,-0.05f); o.imageWidth=dims[0]; o.imageHeight=dims[1];
            for (const auto raster : {std::array<int,2>{64,64}, {80,24}}) {
                auto g = Build(suite,o,Triangle(),raster[0],raster[1]);
                const int x=raster[0]/4, y=raster[1]/4; const size_t at=y*raster[0]+x;
                const double nx=.15, ny=-.08*double(dims[0])/dims[1], length=std::sqrt(nx*nx+ny*ny+1);
                suite.Require(g.coverage.data[at]==1, "analytic plane probe is supported");
                suite.Check(Near(g.normals.data[at*3],nx/length) && Near(g.normals.data[at*3+1],ny/length) &&
                            Near(g.normals.data[at*3+2],1/length), "normal follows isotropic source-pixel geometry, not raster aspect");
                const double u=(x+.5)/raster[0], v=(y+.5)/raster[1];
                suite.Check(Near(g.depth.data[at],.5-.05+.15*u-.08*v), "depth interpolates fixed analytic model coordinates");
            }
        }
    });
    suite.Group("near surface owns depth and normals regardless of triangle order", [&] {
        auto o = Plane(.15f,0,-.25f);
        o.vertices[3]={.1f,.1f,.2f}; o.vertices[4]={.9f,.1f,.2f}; o.vertices[5]={.1f,.9f,.2f};
        P::FaceTopology t{{{0,1,2},{3,4,5}}};
        auto a=Build(suite,o,t); std::reverse(t.triangles.begin(),t.triangles.end()); auto b=Build(suite,o,t);
        EqualGeometry(suite,a,b,"z-buffer ownership is independent of far/near draw order");
        const size_t at=16*64+16; const double u=16.5/64, length=std::sqrt(.15*.15+1);
        suite.Check(Near(a.depth.data[at],.25+.15*u), "near sloped surface beats flat far surface");
        suite.Check(Near(a.normals.data[at*3],.15/length) && Near(a.normals.data[at*3+2],1/length),
                    "visible normal belongs to near surface, not last drawn triangle");
        // Same depth at the pixel centre, different surface normals. Stable owner
        // must not depend on which triangle happens to be submitted first.
        o.vertices[0].z=0; o.vertices[1].z=0; o.vertices[2].z=0;
        o.vertices[3]={.125f,.125f,-.033203125f};
        o.vertices[4]={.875f,.125f,.154296875f};
        o.vertices[5]={.125f,.875f,-.033203125f};
        auto c=Build(suite,o,t); std::reverse(t.triangles.begin(),t.triangles.end()); auto d=Build(suite,o,t);
        EqualGeometry(suite,c,d,"coincident-depth tie remains deterministic");
        suite.Check(c.depth.data[at]==.5f && c.normals.data[at*3]==0, "coincident probe selects deterministic flat-surface owner");
    });
    suite.Group("surface holes and exterior never receive invented geometry", [&] {
        auto o=Observation();
        const std::array<P::FacePoint3D,8> points={{{.1f,.1f,0},{.9f,.1f,0},{.9f,.9f,0},{.1f,.9f,0},
                                                  {.4f,.4f,0},{.6f,.4f,0},{.6f,.6f,0},{.4f,.6f,0}}};
        std::copy(points.begin(),points.end(),o.vertices.begin());
        P::FaceTopology t{{{0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}}};
        auto g=Build(suite,o,t);
        Unsupported(suite,g,32,32); Unsupported(suite,g,31,31); Unsupported(suite,g,0,32); Unsupported(suite,g,63,32);
        suite.Check(g.coverage.data[16*64+32]==1 && g.coverage.data[32*64+16]==1, "ring remains covered around central hole");
        bool contract=true;
        for (size_t i=0;i<g.coverage.data.size();++i) {
            contract=contract && (g.coverage.data[i]==0 || g.coverage.data[i]==1) && g.coverage.data[i]==g.depth.confidence[i];
            if (g.coverage.data[i]) {
                const double x=g.normals.data[i*3],y=g.normals.data[i*3+1],z=g.normals.data[i*3+2];
                contract=contract && std::isfinite(g.depth.data[i]) && Near(float(x*x+y*y+z*z),1) && z>0;
            } else contract=contract && g.depth.data[i]==-1 && g.normals.data[i*3]==0 && g.normals.data[i*3+1]==0 && g.normals.data[i*3+2]==0;
        }
        suite.Check(contract,"all raster samples preserve coverage/depth/normal contract");
    });
    suite.Group("winding reversal, mirror handedness and offscreen clipping", [&] {
        auto o=Plane(.18f,.07f,-.1f); auto t=Triangle(); auto a=Build(suite,o,t);
        std::swap(t.triangles[0][1],t.triangles[0][2]); auto b=Build(suite,o,t);
        EqualGeometry(suite,a,b,"either mesh winding gives same facing geometry");
        for (auto& p:o.vertices) p.x=1-p.x;
        auto mirrored=Build(suite,o,t); bool parity=true;
        for (int y=0;y<64;++y) for (int x=0;x<64;++x) {
            const size_t left=y*64+x, right=y*64+(63-x);
            parity=parity && a.coverage.data[left]==mirrored.coverage.data[right];
            if (a.coverage.data[left]) parity=parity && Near(a.depth.data[left],mirrored.depth.data[right]) &&
                Near(a.normals.data[left*3],-mirrored.normals.data[right*3]) &&
                Near(a.normals.data[left*3+1],mirrored.normals.data[right*3+1]) &&
                Near(a.normals.data[left*3+2],mirrored.normals.data[right*3+2]);
        }
        suite.Check(parity,"horizontal mirror reflects normal X while preserving depth/support");
        o=Observation(); o.vertices[0]={-.4f,-.4f,0};o.vertices[1]={1.4f,-.4f,0};o.vertices[2]={-.4f,1.4f,0};
        auto clipped=Build(suite,o,Triangle());
        suite.Check(clipped.coverage.data[0]==1 && clipped.coverage.data[16*64+16]==1,"offscreen triangle clips onto visible image");
        Unsupported(suite,clipped,63,63);
    });
    suite.Group("degenerate, invalid and allocation-bound failures clear output", [&] {
        auto o=Plane(); auto t=Triangle();
        auto invalid=o; invalid.detected=false; Reject(suite,invalid,t,64,64,"no-face input rejects");
        invalid=o; invalid.vertices.pop_back(); Reject(suite,invalid,t,64,64,"477 observation vertices rejects");
        invalid=o; invalid.vertices.push_back({}); Reject(suite,invalid,t,64,64,"479 observation vertices rejects");
        for (float bad : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),17.0f}) {
            invalid=o; invalid.vertices[477].x=bad; Reject(suite,invalid,t,64,64,"nonfinite/excessive unused landmark rejects");
        }
        for (uint32_t bad : {468U,std::numeric_limits<uint32_t>::max()}) {
            auto badT=t; badT.triangles[0][1]=bad; Reject(suite,o,badT,64,64,"surface index outside canonical468 rejects");
        }
        Reject(suite,o,P::FaceTopology{},64,64,"empty topology rejects");
        auto excessive=t; excessive.triangles.assign(2049,{0,1,2}); Reject(suite,o,excessive,64,64,"triangle budget rejects");
        for (const auto dims : {std::array<int,2>{0,64},{-1,64},{2049,64},{64,2049},{std::numeric_limits<int>::max(),2}})
            Reject(suite,o,t,dims[0],dims[1],"invalid or over-budget raster rejects before allocation");
        for (int bad : {0,-1,8193,std::numeric_limits<int>::max()}) {
            invalid=o; invalid.imageWidth=bad; Reject(suite,invalid,t,64,64,"invalid source width rejects");
            invalid=o; invalid.imageHeight=bad; Reject(suite,invalid,t,64,64,"invalid source height rejects");
        }
        invalid=o; invalid.vertices[2]=invalid.vertices[1]; Reject(suite,invalid,t,64,64,"degenerate projected surface rejects");
        invalid=o; for(auto& p:invalid.vertices) p.x+=2; Reject(suite,invalid,t,64,64,"entire offscreen surface rejects");
        invalid=Plane(0,0,.6f); Reject(suite,invalid,t,64,64,"depth outside fixed calibration rejects without frame normalization");
        invalid=Plane(0,0,-.6f); Reject(suite,invalid,t,64,64,"near depth outside fixed calibration rejects");
    });
    suite.Group("bounded local OBJ parsing and invalid reference rejection", [&] {
        TempFiles files; const std::string vertices=ObjVertices(); P::FaceTopology topology; std::string error;
        const auto valid=files.Write("valid.obj","# bounded fixture\n"+vertices+"vt .1 .2\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1 # inline comment\n");
        suite.Require(P::LoadFaceTopology(valid.wstring(),topology,error),"valid indexed local OBJ parses");
        suite.Check(topology.triangles.size()==1 && topology.triangles[0]==std::array<uint32_t,3>{0,1,2},"OBJ indices become zero-based surface indices");
        RejectObj(suite,files,"few.obj",ObjVertices(467)+"f 1 2 3\n");
        RejectObj(suite,files,"many.obj",ObjVertices(469)+"f 1 2 3\n");
        RejectObj(suite,files,"polygon.obj",vertices+"f 1 2 3 4\n");
        RejectObj(suite,files,"repeated.obj",vertices+"f 1 2 2\n");
        RejectObj(suite,files,"duplicate.obj",vertices+"f 1 2 3\nf 3 1 2\n");
        RejectObj(suite,files,"zero.obj",vertices+"f 0 2 3\n");
        RejectObj(suite,files,"negative.obj",vertices+"f -1 2 3\n");
        RejectObj(suite,files,"overflow.obj",vertices+"f 999999999999999999999999 2 3\n");
        RejectObj(suite,files,"surface469.obj",vertices+"f 1 2 469\n");
        RejectObj(suite,files,"missing-vt.obj",vertices+"f 1/1 2/1 3/1\n");
        RejectObj(suite,files,"missing-vn.obj",vertices+"f 1//1 2//1 3//1\n");
        RejectObj(suite,files,"slash.obj",vertices+"f 1/1/1/1 2 3\n");
        RejectObj(suite,files,"external.obj",vertices+"mtllib external.mtl\nf 1 2 3\n");
        RejectObj(suite,files,"nan.obj","v nan 0 0\n"+ObjVertices(467)+"f 1 2 3\n");
        RejectObj(suite,files,"nul.obj",vertices+std::string("\0",1)+"f 1 2 3\n");
        RejectObj(suite,files,"long-line.obj",std::string(1025,'#')+"\n"+vertices+"f 1 2 3\n");
        RejectObj(suite,files,"oversize.obj",std::string(1024*1024+1,' '));
        topology=Triangle(); suite.Check(!P::LoadFaceTopology((files.root/"absent.obj").wstring(),topology,error) &&
                    topology.triangles.empty() && !error.empty(),"missing topology file clears previous data");
        const auto normalsOnly=files.Write("double-slash.obj",vertices+"vn 0 0 1\nf 1//1 2//1 3//1\n");
        suite.Check(P::LoadFaceTopology(normalsOnly.wstring(),topology,error),"valid vertex//normal references parse");
    });
    if (argc>1) suite.Group("actual canonical face asset has468 vertices and898 triangular faces", [&] {
        P::FaceTopology topology; std::string error;
        suite.Require(P::LoadFaceTopology(fs::path(argv[1]).wstring(),topology,error),"actual canonical OBJ parses: "+error);
        suite.Check(topology.triangles.size()==898,"reviewed canonical asset contains898 triangular faces");
        uint32_t largest=0; for(const auto& triangle:topology.triangles) for(auto index:triangle) largest=std::max(largest,index);
        suite.Check(largest==467,"actual canonical topology spans all468 surface vertices");
    });
    else std::cout << "SKIP actual canonical asset: no optional OBJ filepath supplied\n";
    std::cout << "SYNTHETIC_GEOMETRY checks=" << suite.checks << " groups=" << suite.groups
              << " failures=" << suite.failures << " failed_groups=" << suite.failedGroups
              << "; no real model inference or live-camera performance measured\n";
    return suite.failures ? 1 : 0;
}