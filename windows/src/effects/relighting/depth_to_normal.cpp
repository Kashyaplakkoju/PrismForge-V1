#include "depth_to_normal.h"
#include <cmath>
#include <algorithm>
#include <array>

namespace PrismForge::Effects::Relighting {

bool DepthToNormal::ReconstructNormals(
    const DepthBuffer& depth,
    AnalysisBridge::NormalBuffer& outNormals,
    const DepthToNormalConfig& config
) {
    outNormals = AnalysisBridge::NormalBuffer{};
    if (!depth.IsValid() || !config.IsValid()) return false;
    AnalysisBridge::NormalBuffer candidate;

    const int width = depth.width;
    const int height = depth.height;
    const int inStride = depth.strideFloats;

    candidate.width = width;
    candidate.height = height;
    candidate.strideFloats = width * 3;
    try {
        candidate.data.assign(static_cast<size_t>(candidate.strideFloats) * height, 0.0f);
    } catch (...) {
        return false;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const bool hasConf = !depth.confidence.empty();

    auto sampleValidDepth = [&](int sx, int sy, float centerVal) -> float {
        int cx = std::clamp(sx, 0, width - 1);
        int cy = std::clamp(sy, 0, height - 1);
        float d = depth.data[static_cast<size_t>(cy) * inStride + cx];
        float c = hasConf ? depth.confidence[static_cast<size_t>(cy) * inStride + cx] : 1.0f;
        if (IsValidSample(d, config.invalidDepthMin, config.invalidDepthMax, c, config.minConfidence)) {
            return d;
        }
        return centerVal;
    };

    for (int y = 0; y < height; ++y) {
        const float* depthRow = depth.data.data() + static_cast<size_t>(y) * inStride;
        const float* confRow = hasConf ? (depth.confidence.data() + static_cast<size_t>(y) * inStride) : nullptr;
        float* outRow = candidate.data.data() + static_cast<size_t>(y) * candidate.strideFloats;

        for (int x = 0; x < width; ++x) {
            float centerD = depthRow[x];
            float centerC = confRow ? confRow[x] : 1.0f;

            if (!IsValidSample(centerD, config.invalidDepthMin, config.invalidDepthMax, centerC, config.minConfidence)) {
                outRow[3 * x + 0] = 0.0f;
                outRow[3 * x + 1] = 0.0f;
                outRow[3 * x + 2] = 1.0f;
                continue;
            }

            float dx = 0.0f;
            float dy = 0.0f;

            if (config.useSobel) {
                float tl = sampleValidDepth(x - 1, y - 1, centerD);
                float tc = sampleValidDepth(x,     y - 1, centerD);
                float tr = sampleValidDepth(x + 1, y - 1, centerD);
                float ml = sampleValidDepth(x - 1, y,     centerD);
                float mr = sampleValidDepth(x + 1, y,     centerD);
                float bl = sampleValidDepth(x - 1, y + 1, centerD);
                float bc = sampleValidDepth(x,     y + 1, centerD);
                float br = sampleValidDepth(x + 1, y + 1, centerD);

                dx = ((tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl)) * 0.125f;
                dy = ((bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr)) * 0.125f;
            } else {
                float l = sampleValidDepth(x - 1, y, centerD);
                float r = sampleValidDepth(x + 1, y, centerD);
                float t = sampleValidDepth(x, y - 1, centerD);
                float b = sampleValidDepth(x, y + 1, centerD);

                dx = (r - l) * 0.5f;
                dy = (b - t) * 0.5f;
            }

            float nx = 0.0f;
            float ny = 0.0f;
            const float scaleX = config.depthScale * width;
            const float scaleY = config.depthScale * height;
            if (config.format == DepthFormat::RelativeDepth) {
                nx = dx * scaleX;
                ny = dy * scaleY;
            } else {
                nx = -dx * scaleX;
                ny = -dy * scaleY;
            }

            if (config.flipX) nx = -nx;
            if (config.flipY) ny = -ny;

            float nz = (config.fallbackZ > 0.0f) ? config.fallbackZ : 1.0f;

            float lenSq = nx * nx + ny * ny + nz * nz;
            if (lenSq > 1e-6f && std::isfinite(lenSq)) {
                float invLen = 1.0f / std::sqrt(lenSq);
                nx *= invLen;
                ny *= invLen;
                nz *= invLen;
            } else {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            }

            if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)) {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            }

            outRow[3 * x + 0] = nx;
            outRow[3 * x + 1] = ny;
            outRow[3 * x + 2] = nz;
        }
    }

    outNormals = std::move(candidate);
    return true;
}

bool DepthToNormal::EvaluateMeshNormals(
    const TriangleMesh3D& inMesh,
    std::vector<MeshVertex3D>& outVerticesWithNormals
) {
    // Keep source immutable until commit, including when output aliases mesh.vertices.
    if (!inMesh.IsValid()) { outVerticesWithNormals.clear(); return false; }
    std::vector<MeshVertex3D> candidate;
    std::vector<std::array<double,3>> sums;
    try { candidate = inMesh.vertices; sums.resize(candidate.size()); }
    catch (...) { outVerticesWithNormals.clear(); return false; }
    for (const auto& tri : inMesh.indices) {
        const auto& p0 = inMesh.vertices[tri.v0];
        const auto& p1 = inMesh.vertices[tri.v1];
        const auto& p2 = inMesh.vertices[tri.v2];
        const double e1x = double(p1.x)-p0.x, e1y = double(p1.y)-p0.y, e1z = double(p1.z)-p0.z;
        const double e2x = double(p2.x)-p0.x, e2y = double(p2.y)-p0.y, e2z = double(p2.z)-p0.z;
        const double nx = e1y*e2z-e1z*e2y, ny = e1z*e2x-e1x*e2z, nz = e1x*e2y-e1y*e2x;
        for (uint32_t index : {tri.v0,tri.v1,tri.v2}) { sums[index][0]+=nx; sums[index][1]+=ny; sums[index][2]+=nz; }
    }
    for (size_t i=0; i<candidate.size(); ++i) {
        auto& vertex = candidate[i];
        const double lenSq = sums[i][0]*sums[i][0]+sums[i][1]*sums[i][1]+sums[i][2]*sums[i][2];
        if (lenSq > 1e-6 && std::isfinite(lenSq)) {
            const double inv = 1.0/std::sqrt(lenSq);
            vertex.nx = static_cast<float>(sums[i][0]*inv);
            vertex.ny = static_cast<float>(sums[i][1]*inv);
            vertex.nz = static_cast<float>(sums[i][2]*inv);
        } else { vertex.nx=0.0f; vertex.ny=0.0f; vertex.nz=1.0f; }
    }
    outVerticesWithNormals = std::move(candidate);
    return true;
}

bool DepthToNormal::RasterizeMeshNormals(
    const TriangleMesh3D& mesh,
    int width,
    int height,
    AnalysisBridge::NormalBuffer& outNormals
) {
    outNormals = AnalysisBridge::NormalBuffer{};
    if (!mesh.IsValid() || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        return false;
    }

    std::vector<MeshVertex3D> verticesWithNormals;
    if (!EvaluateMeshNormals(mesh, verticesWithNormals)) {
        return false;
    }

    outNormals.width = width;
    outNormals.height = height;
    outNormals.strideFloats = width * 3;
    try {
        outNormals.data.assign(static_cast<size_t>(outNormals.strideFloats) * height, 0.0f);
    } catch (...) {
        return false;
    }

    // Initialize with default (0, 0, 1)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            outNormals.data[3 * (y * width + x) + 2] = 1.0f;
        }
    }

    // Rasterize each triangle into the normal buffer
    for (const auto& tri : mesh.indices) {
        const auto& v0 = verticesWithNormals[tri.v0];
        const auto& v1 = verticesWithNormals[tri.v1];
        const auto& v2 = verticesWithNormals[tri.v2];

        // Screen space coordinates [0, width-1] x [0, height-1]
        float p0x = v0.u * (width - 1);
        float p0y = v0.v * (height - 1);
        float p1x = v1.u * (width - 1);
        float p1y = v1.v * (height - 1);
        float p2x = v2.u * (width - 1);
        float p2y = v2.v * (height - 1);

        int minX = std::clamp(static_cast<int>(std::floor(std::min({p0x, p1x, p2x}))), 0, width - 1);
        int maxX = std::clamp(static_cast<int>(std::ceil(std::max({p0x, p1x, p2x}))), 0, width - 1);
        int minY = std::clamp(static_cast<int>(std::floor(std::min({p0y, p1y, p2y}))), 0, height - 1);
        int maxY = std::clamp(static_cast<int>(std::ceil(std::max({p0y, p1y, p2y}))), 0, height - 1);

        float denom = (p1y - p2y) * (p0x - p2x) + (p2x - p1x) * (p0y - p2y);
        if (std::abs(denom) < 1e-6f) continue;
        float invDenom = 1.0f / denom;

        for (int py = minY; py <= maxY; ++py) {
            for (int px = minX; px <= maxX; ++px) {
                float w0 = ((p1y - p2y) * (px - p2x) + (p2x - p1x) * (py - p2y)) * invDenom;
                float w1 = ((p2y - p0y) * (px - p2x) + (p0x - p2x) * (py - p2y)) * invDenom;
                float w2 = 1.0f - w0 - w1;

                if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                    float nx = w0 * v0.nx + w1 * v1.nx + w2 * v2.nx;
                    float ny = w0 * v0.ny + w1 * v1.ny + w2 * v2.ny;
                    float nz = w0 * v0.nz + w1 * v1.nz + w2 * v2.nz;

                    float lenSq = nx * nx + ny * ny + nz * nz;
                    if (lenSq > 1e-6f && std::isfinite(lenSq)) {
                        float inv = 1.0f / std::sqrt(lenSq);
                        nx *= inv; ny *= inv; nz *= inv;
                    } else {
                        nx = 0.0f; ny = 0.0f; nz = 1.0f;
                    }

                    int idx = 3 * (py * width + px);
                    outNormals.data[idx + 0] = nx;
                    outNormals.data[idx + 1] = ny;
                    outNormals.data[idx + 2] = nz;
                }
            }
        }
    }

    return true;
}

} // namespace PrismForge::Effects::Relighting
