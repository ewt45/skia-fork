/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrCCCoverageProcessor.h"

#include "GrMesh.h"
#include "glsl/GrGLSLVertexGeoBuilder.h"

static constexpr int kAttribIdx_X = 0;
static constexpr int kAttribIdx_Y = 1;
static constexpr int kAttribIdx_VertexData = 2;

static constexpr int kVertexData_LeftNeighborIdShift = 10;
static constexpr int kVertexData_RightNeighborIdShift = 8;
static constexpr int kVertexData_BloatIdxShift = 6;
static constexpr int kVertexData_InvertNegativeCoverageBit = 1 << 5;
static constexpr int kVertexData_IsCornerBit = 1 << 4;
static constexpr int kVertexData_IsEdgeBit = 1 << 3;
static constexpr int kVertexData_IsHullBit = 1 << 2;

/**
 * Vertex data tells the shader how to offset vertices for conservative raster, and how/whether to
 * calculate coverage values. See VSHullAndEdgeImpl.
 */
static constexpr int32_t pack_vertex_data(int32_t leftNeighborID, int32_t rightNeighborID,
                                          int32_t bloatIdx, int32_t cornerID,
                                          int32_t extraData = 0) {
    return (leftNeighborID << kVertexData_LeftNeighborIdShift) |
           (rightNeighborID << kVertexData_RightNeighborIdShift) |
           (bloatIdx << kVertexData_BloatIdxShift) |
           cornerID | extraData;
}

static constexpr int32_t hull_vertex_data(int32_t cornerID, int32_t bloatIdx, int n) {
    return pack_vertex_data((cornerID + n - 1) % n, (cornerID + 1) % n, bloatIdx, cornerID,
                            kVertexData_IsHullBit);
}

static constexpr int32_t edge_vertex_data(int32_t leftID, int rightID, int32_t bloatIdx,
                                          int32_t extraData = 0) {
    return pack_vertex_data(leftID, leftID, bloatIdx, rightID, kVertexData_IsEdgeBit | extraData);
}

static constexpr int32_t triangle_corner_vertex_data(int32_t cornerID, int32_t bloatIdx) {
    return pack_vertex_data((cornerID + 2) % 3, (cornerID + 1) % 3, bloatIdx, cornerID,
                            kVertexData_IsCornerBit);
}

static constexpr int32_t kTriangleVertices[] = {
    hull_vertex_data(0, 0, 3),
    hull_vertex_data(0, 1, 3),
    hull_vertex_data(0, 2, 3),
    hull_vertex_data(1, 0, 3),
    hull_vertex_data(1, 1, 3),
    hull_vertex_data(1, 2, 3),
    hull_vertex_data(2, 0, 3),
    hull_vertex_data(2, 1, 3),
    hull_vertex_data(2, 2, 3),

    edge_vertex_data(0, 1, 0),
    edge_vertex_data(0, 1, 1),
    edge_vertex_data(0, 1, 2),
    edge_vertex_data(1, 0, 0, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(1, 0, 1, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(1, 0, 2, kVertexData_InvertNegativeCoverageBit),

    edge_vertex_data(1, 2, 0),
    edge_vertex_data(1, 2, 1),
    edge_vertex_data(1, 2, 2),
    edge_vertex_data(2, 1, 0, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(2, 1, 1, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(2, 1, 2, kVertexData_InvertNegativeCoverageBit),

    edge_vertex_data(2, 0, 0),
    edge_vertex_data(2, 0, 1),
    edge_vertex_data(2, 0, 2),
    edge_vertex_data(0, 2, 0, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(0, 2, 1, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(0, 2, 2, kVertexData_InvertNegativeCoverageBit),

    triangle_corner_vertex_data(0, 0),
    triangle_corner_vertex_data(0, 1),
    triangle_corner_vertex_data(0, 2),
    triangle_corner_vertex_data(0, 3),

    triangle_corner_vertex_data(1, 0),
    triangle_corner_vertex_data(1, 1),
    triangle_corner_vertex_data(1, 2),
    triangle_corner_vertex_data(1, 3),

    triangle_corner_vertex_data(2, 0),
    triangle_corner_vertex_data(2, 1),
    triangle_corner_vertex_data(2, 2),
    triangle_corner_vertex_data(2, 3),
};

GR_DECLARE_STATIC_UNIQUE_KEY(gTriangleVertexBufferKey);

static constexpr uint16_t kRestartStrip = 0xffff;

static constexpr uint16_t kTriangleIndicesAsStrips[] =  {
    1, 2, 0, 3, 8, kRestartStrip, // First corner and main body of the hull.
    4, 5, 3, 6, 8, 7, kRestartStrip, // Opposite side and corners of the hull.
    10, 9, 11, 14, 12, 13, kRestartStrip, // First edge.
    16, 15, 17, 20, 18, 19, kRestartStrip, // Second edge.
    22, 21, 23, 26, 24, 25, kRestartStrip, // Third edge.
    27, 28, 30, 29, kRestartStrip, // First corner.
    31, 32, 34, 33, kRestartStrip, // Second corner.
    35, 36, 38, 37 // Third corner.
};

static constexpr uint16_t kTriangleIndicesAsTris[] =  {
    // First corner and main body of the hull.
    1, 2, 0,
    2, 3, 0,
    0, 3, 8, // Main body.

    // Opposite side and corners of the hull.
    4, 5, 3,
    5, 6, 3,
    3, 6, 8,
    6, 7, 8,

    // First edge.
    10,  9, 11,
     9, 14, 11,
    11, 14, 12,
    14, 13, 12,

    // Second edge.
    16, 15, 17,
    15, 20, 17,
    17, 20, 18,
    20, 19, 18,

    // Third edge.
    22, 21, 23,
    21, 26, 23,
    23, 26, 24,
    26, 25, 24,

    // First corner.
    27, 28, 30,
    28, 29, 30,

    // Second corner.
    31, 32, 34,
    32, 33, 34,

    // Third corner.
    35, 36, 38,
    36, 37, 38,
};

GR_DECLARE_STATIC_UNIQUE_KEY(gTriangleIndexBufferKey);

static constexpr int32_t kHull4AndEdgeVertices[] = {
    hull_vertex_data(0, 0, 4),
    hull_vertex_data(0, 1, 4),
    hull_vertex_data(0, 2, 4),
    hull_vertex_data(1, 0, 4),
    hull_vertex_data(1, 1, 4),
    hull_vertex_data(1, 2, 4),
    hull_vertex_data(2, 0, 4),
    hull_vertex_data(2, 1, 4),
    hull_vertex_data(2, 2, 4),
    hull_vertex_data(3, 0, 4),
    hull_vertex_data(3, 1, 4),
    hull_vertex_data(3, 2, 4),

    edge_vertex_data(0, 3, 0, kVertexData_InvertNegativeCoverageBit),
    edge_vertex_data(0, 3, 1),
    edge_vertex_data(0, 3, 2),
    edge_vertex_data(3, 0, 0),
    edge_vertex_data(3, 0, 1),
    edge_vertex_data(3, 0, 2, kVertexData_InvertNegativeCoverageBit),
};

GR_DECLARE_STATIC_UNIQUE_KEY(gHull4AndEdgeVertexBufferKey);

static constexpr uint16_t kHull4AndEdgeIndicesAsStrips[] =  {
    1, 0, 2, 11, 3, 5, 4, kRestartStrip, // First half of the hull (split diagonally).
    7, 6, 8, 5, 9, 11, 10, kRestartStrip, // Second half of the hull.
    13, 12, 14, 17, 15, 16 // Shared edge.
};

static constexpr uint16_t kHull4AndEdgeIndicesAsTris[] =  {
    // First half of the hull (split diagonally).
     1,  0,  2,
     0, 11,  2,
     2, 11,  3,
    11,  5,  3,
     3,  5,  4,

    // Second half of the hull.
    7,  6,  8,
    6,  5,  8,
    8,  5,  9,
    5, 11,  9,
    9, 11, 10,

    // Shared edge.
    13, 12, 14,
    12, 17, 14,
    14, 17, 15,
    17, 16, 15,
};

GR_DECLARE_STATIC_UNIQUE_KEY(gHull4AndEdgeIndexBufferKey);


/**
 * This class and its subclasses implement the coverage processor with vertex shaders.
 */
class GrCCCoverageProcessor::VSImpl : public GrGLSLGeometryProcessor {
public:
    VSImpl(std::unique_ptr<Shader> shader) : fShader(std::move(shader)) {}

private:
    void setData(const GrGLSLProgramDataManager& pdman, const GrPrimitiveProcessor&,
                 FPCoordTransformIter&& transformIter) override {
        this->setTransformDataHelper(SkMatrix::I(), pdman, &transformIter);
    }

    void onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) override;

    const char* emitVertexPosition(const GrCCCoverageProcessor&, GrGLSLVertexBuilder*,
                                   GrGPArgs*) const;

    const std::unique_ptr<Shader> fShader;
};

void GrCCCoverageProcessor::VSImpl::onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) {
    const GrCCCoverageProcessor& proc = args.fGP.cast<GrCCCoverageProcessor>();

    // Vertex shader.
    GrGLSLVertexBuilder* v = args.fVertBuilder;
    int numInputPoints = proc.numInputPoints();

    const char* swizzle = (4 == numInputPoints) ? "xyzw" : "xyz";
    v->codeAppendf("float%ix2 pts = transpose(float2x%i(%s.%s, %s.%s));",
                   numInputPoints, numInputPoints, proc.getAttrib(kAttribIdx_X).fName, swizzle,
                   proc.getAttrib(kAttribIdx_Y).fName, swizzle);

    if (WindMethod::kCrossProduct == proc.fWindMethod) {
        v->codeAppend ("float area_x2 = determinant(float2x2(pts[0] - pts[1], "
                                                            "pts[0] - pts[2]));");
        if (4 == numInputPoints) {
            v->codeAppend ("area_x2 += determinant(float2x2(pts[0] - pts[2], "
                                                           "pts[0] - pts[3]));");
        }
        v->codeAppend ("half wind = sign(area_x2);");
    } else {
        SkASSERT(WindMethod::kInstanceData == proc.fWindMethod);
        SkASSERT(3 == numInputPoints);
        SkASSERT(kFloat4_GrVertexAttribType == proc.getAttrib(kAttribIdx_X).fType);
        v->codeAppendf("half wind = %s.w;", proc.getAttrib(kAttribIdx_X).fName);
    }

    float bloat = kAABloatRadius;
#ifdef SK_DEBUG
    if (proc.debugVisualizationsEnabled()) {
        bloat *= proc.debugBloat();
    }
#endif
    v->defineConstant("bloat", bloat);

    const char* coverage = this->emitVertexPosition(proc, v, gpArgs);
    SkASSERT(kFloat2_GrSLType == gpArgs->fPositionVar.getType());

    GrGLSLVaryingHandler* varyingHandler = args.fVaryingHandler;
    SkString varyingCode;
    fShader->emitVaryings(varyingHandler, GrGLSLVarying::Scope::kVertToFrag, &varyingCode,
                          gpArgs->fPositionVar.c_str(), coverage, "wind");
    v->codeAppend(varyingCode.c_str());

    varyingHandler->emitAttributes(proc);
    SkASSERT(!args.fFPCoordTransformHandler->nextCoordTransform());

    // Fragment shader.
    fShader->emitFragmentCode(proc, args.fFragBuilder, args.fOutputColor, args.fOutputCoverage);
}

/**
 * Generates a conservative raster hull around a triangle or curve. For triangles we generate
 * additional conservative rasters with coverage ramps around the edges. For curves we generate an
 * additional raster with coverage ramps around its shared edge.
 *
 * Triangles are drawn in three steps: (1) Draw a conservative raster of the entire triangle, with a
 * coverage of +1. (2) Draw conservative rasters around each edge, with a coverage ramp from -1 to
 * 0. These edge coverage values convert jagged conservative raster edges into smooth, antialiased
 * ones. (3) Draw conservative rasters (aka pixel-size boxes) around each corner, replacing the
 * previous coverage values with ones that ramp to zero in the bloat vertices that fall outside the
 * triangle.
 *
 * Curves are drawn in two steps: (1) Draw a conservative raster around the input points, passing
 * coverage=+1 to the Shader. (2) Draw an additional conservative raster around the curve's shared
 * edge, using coverage=-1 at bloat vertices that fall outside the input points. This erases what
 * the hull just wrote and ramps coverage to zero.
 */
const char* GrCCCoverageProcessor::VSImpl::emitVertexPosition(const GrCCCoverageProcessor& proc,
                                                              GrGLSLVertexBuilder* v,
                                                              GrGPArgs* gpArgs) const {
    int numSides = (RenderPass::kTriangles == proc.fRenderPass) ? 3 : 4;
    const char* hullPts = fShader->emitSetupCode(v, "pts");
    if (!hullPts) {
        SkASSERT(numSides == proc.numInputPoints());
        hullPts = "pts";
    }

    // Reverse all indices if the wind is counter-clockwise: [0, 1, 2] -> [2, 1, 0].
    v->codeAppendf("int clockwise_indices = wind > 0 ? %s : 0x%x - %s;",
                   proc.getAttrib(kAttribIdx_VertexData).fName,
                   ((numSides - 1) << kVertexData_LeftNeighborIdShift) |
                   ((numSides - 1) << kVertexData_RightNeighborIdShift) |
                   (((1 << kVertexData_RightNeighborIdShift) - 1) ^ 3) |
                   (numSides - 1),
                   proc.getAttrib(kAttribIdx_VertexData).fName);

    // Here we generate conservative raster geometry for the input polygon. It is the convex hull of
    // N pixel-size boxes, one centered on each the input points. Each corner has three vertices,
    // where one or two may cause degenerate triangles. The vertex data tells us how to offset each
    // vertex. Edges are also handled here using the same concept. For more details on conservative
    // raster, see: https://developer.nvidia.com/gpugems/GPUGems2/gpugems2_chapter42.html
    v->codeAppendf("float2 corner = %s[clockwise_indices & 3];", hullPts);
    v->codeAppendf("float2 left = %s[clockwise_indices >> %i];",
                   hullPts, kVertexData_LeftNeighborIdShift);
    v->codeAppendf("float2 right = %s[(clockwise_indices >> %i) & 3];",
                   hullPts, kVertexData_RightNeighborIdShift);

    v->codeAppend ("float2 leftbloat = sign(corner - left);");
    v->codeAppend ("leftbloat = float2(0 != leftbloat.y ? leftbloat.y : leftbloat.x, "
                                      "0 != leftbloat.x ? -leftbloat.x : -leftbloat.y);");

    v->codeAppend ("float2 rightbloat = sign(right - corner);");
    v->codeAppend ("rightbloat = float2(0 != rightbloat.y ? rightbloat.y : rightbloat.x, "
                                       "0 != rightbloat.x ? -rightbloat.x : -rightbloat.y);");

    v->codeAppend ("bool2 left_right_notequal = notEqual(leftbloat, rightbloat);");

    v->codeAppend ("float2 bloatdir = leftbloat;");

    if (RenderPass::kTriangles == proc.fRenderPass) { // Only triangles emit corner boxes.
        v->codeAppendf("if (0 != (%s & %i)) {", // Are we a corner?
                       proc.getAttrib(kAttribIdx_VertexData).fName, kVertexData_IsCornerBit);

                           // For corner boxes, we hack 'left_right_notequal' to [true, true].  This
                           // causes the upcoming code to always rotate, which is the right thing
                           // for corners.
        v->codeAppendf(    "left_right_notequal = bool2(true, true);");

                           // In corner boxes, all 4 coverage values will not map linearly, so it is
                           // important to rotate the box so its diagonal shared edge points out of
                           // the triangle, in the direction that ramps to zero.
        v->codeAppend (    "float2 bisect = normalize(corner - right) + normalize(corner - left);");
        v->codeAppend (    "if (sign(bisect) == sign(leftbloat)) {");
        v->codeAppend (        "bloatdir = float2(+bloatdir.y, -bloatdir.x);");
        v->codeAppend (    "}");
        v->codeAppend ("}");
    }

    // At each corner of the polygon, our hull will have either 1, 2, or 3 vertices (or 4 if it's a
    // corner box). We begin with the first hull vertex (leftbloat), then continue rotating 90
    // degrees clockwise until we reach the desired vertex for this invocation.  Corners with less
    // than 3 corresponding hull vertices will result in redundant vertices and degenerate
    // triangles.
    v->codeAppendf("int bloatidx = (%s >> %i) & 3;",
                   proc.getAttrib(kAttribIdx_VertexData).fName, kVertexData_BloatIdxShift);
    v->codeAppend ("switch (bloatidx) {");
    if (RenderPass::kTriangles == proc.fRenderPass) { // Only triangles emit corner boxes.
        v->codeAppend (    "case 3:");
                                // Only corners will have bloatidx=3, and corners always rotate.
        v->codeAppend (        "bloatdir = float2(-bloatdir.y, +bloatdir.x);"); // 90 deg CW.
                               // fallthru.
    }
    v->codeAppend (    "case 2:");
    v->codeAppendf(        "if (all(left_right_notequal)) {");
    v->codeAppend (            "bloatdir = float2(-bloatdir.y, +bloatdir.x);"); // 90 deg CW.
    v->codeAppend (        "}");
                           // fallthru.
    v->codeAppend (    "case 1:");
    v->codeAppendf(        "if (any(left_right_notequal)) {");
    v->codeAppend (            "bloatdir = float2(-bloatdir.y, +bloatdir.x);"); // 90 deg CW.
    v->codeAppend (        "}");
                           // fallthru.
    v->codeAppend ("}");

    v->codeAppend ("float2 vertex = corner + bloatdir * bloat;");
    gpArgs->fPositionVar.set(kFloat2_GrSLType, "vertex");

    // The hull has a coverage of +1 all around.
    v->codeAppend ("half coverage = +1;");

    if (RenderPass::kTriangles == proc.fRenderPass) {
        v->codeAppendf("if (0 != (%s & %i)) {", // Are we an edge OR corner?
                       proc.getAttrib(kAttribIdx_VertexData).fName,
                       kVertexData_IsEdgeBit | kVertexData_IsCornerBit);
        Shader::CalcEdgeCoverageAtBloatVertex(v, "left", "corner", "bloatdir", "coverage");
        v->codeAppend ("}");

        v->codeAppendf("if (0 != (%s & %i)) {", // Are we a corner?
                       proc.getAttrib(kAttribIdx_VertexData).fName, kVertexData_IsCornerBit);
                           // Corner boxes erase whatever coverage was written previously, and
                           // replace it with linearly-interpolated values that ramp to zero in the
                           // diagonal that points out of the triangle, and ramp from left-edge
                           // coverage to right-edge coverage in the other diagonal.
        v->codeAppend (    "half left_coverage = coverage;");
        v->codeAppend (    "half right_coverage;");
        Shader::CalcEdgeCoverageAtBloatVertex(v, "corner", "right", "bloatdir",
                                              "right_coverage");
        v->codeAppend (    "coverage = (1 == bloatidx) ? -1 : 0;");
        v->codeAppend (    "if (((bloatidx + 3) & 3) < 2) {");
        v->codeAppend (        "coverage -= left_coverage;");
        v->codeAppend (    "}");
        v->codeAppend (    "if (bloatidx < 2) {");
        v->codeAppend (        "coverage -= right_coverage;");
        v->codeAppend (    "}");
        v->codeAppend ("}");
    } else {
        v->codeAppendf("if (0 != (%s & %i)) {", // Are we an edge?
                       proc.getAttrib(kAttribIdx_VertexData).fName, kVertexData_IsEdgeBit);
        v->codeAppend (    "coverage = -1;");
        v->codeAppend ("}");
    }

    v->codeAppendf("if (0 != (%s & %i)) {", // Invert coverage?
                   proc.getAttrib(kAttribIdx_VertexData).fName,
                   kVertexData_InvertNegativeCoverageBit);
    v->codeAppend (    "coverage = -1 - coverage;");
    v->codeAppend ("}");

    return "coverage";
}

void GrCCCoverageProcessor::initVS(GrResourceProvider* rp) {
    SkASSERT(Impl::kVertexShader == fImpl);
    const GrCaps& caps = *rp->caps();

    switch (fRenderPass) {
        case RenderPass::kTriangles: {
            GR_DEFINE_STATIC_UNIQUE_KEY(gTriangleVertexBufferKey);
            fVertexBuffer = rp->findOrMakeStaticBuffer(kVertex_GrBufferType,
                                                       sizeof(kTriangleVertices),
                                                       kTriangleVertices,
                                                       gTriangleVertexBufferKey);
            GR_DEFINE_STATIC_UNIQUE_KEY(gTriangleIndexBufferKey);
            if (caps.usePrimitiveRestart()) {
                fIndexBuffer = rp->findOrMakeStaticBuffer(kIndex_GrBufferType,
                                                          sizeof(kTriangleIndicesAsStrips),
                                                          kTriangleIndicesAsStrips,
                                                          gTriangleIndexBufferKey);
                fNumIndicesPerInstance = SK_ARRAY_COUNT(kTriangleIndicesAsStrips);
            } else {
                fIndexBuffer = rp->findOrMakeStaticBuffer(kIndex_GrBufferType,
                                                          sizeof(kTriangleIndicesAsTris),
                                                          kTriangleIndicesAsTris,
                                                          gTriangleIndexBufferKey);
                fNumIndicesPerInstance = SK_ARRAY_COUNT(kTriangleIndicesAsTris);
            }
            break;
        }

        case RenderPass::kQuadratics:
        case RenderPass::kCubics: {
            GR_DEFINE_STATIC_UNIQUE_KEY(gHull4AndEdgeVertexBufferKey);
            fVertexBuffer = rp->findOrMakeStaticBuffer(kVertex_GrBufferType,
                                                       sizeof(kHull4AndEdgeVertices),
                                                       kHull4AndEdgeVertices,
                                                       gHull4AndEdgeVertexBufferKey);
            GR_DEFINE_STATIC_UNIQUE_KEY(gHull4AndEdgeIndexBufferKey);
            if (caps.usePrimitiveRestart()) {
                fIndexBuffer = rp->findOrMakeStaticBuffer(kIndex_GrBufferType,
                                                          sizeof(kHull4AndEdgeIndicesAsStrips),
                                                          kHull4AndEdgeIndicesAsStrips,
                                                          gHull4AndEdgeIndexBufferKey);
                fNumIndicesPerInstance = SK_ARRAY_COUNT(kHull4AndEdgeIndicesAsStrips);
            } else {
                fIndexBuffer = rp->findOrMakeStaticBuffer(kIndex_GrBufferType,
                                                          sizeof(kHull4AndEdgeIndicesAsTris),
                                                          kHull4AndEdgeIndicesAsTris,
                                                          gHull4AndEdgeIndexBufferKey);
                fNumIndicesPerInstance = SK_ARRAY_COUNT(kHull4AndEdgeIndicesAsTris);
            }
            break;
        }
    }

    if (RenderPass::kCubics == fRenderPass || WindMethod::kInstanceData == fWindMethod) {
        SkASSERT(WindMethod::kCrossProduct == fWindMethod || 3 == this->numInputPoints());

        SkASSERT(kAttribIdx_X == this->numAttribs());
        this->addInstanceAttrib("X", kFloat4_GrVertexAttribType);

        SkASSERT(kAttribIdx_Y == this->numAttribs());
        this->addInstanceAttrib("Y", kFloat4_GrVertexAttribType);

        SkASSERT(offsetof(QuadPointInstance, fX) == this->getAttrib(kAttribIdx_X).fOffsetInRecord);
        SkASSERT(offsetof(QuadPointInstance, fY) == this->getAttrib(kAttribIdx_Y).fOffsetInRecord);
        SkASSERT(sizeof(QuadPointInstance) == this->getInstanceStride());
    } else {
        SkASSERT(kAttribIdx_X == this->numAttribs());
        this->addInstanceAttrib("X", kFloat3_GrVertexAttribType);

        SkASSERT(kAttribIdx_Y == this->numAttribs());
        this->addInstanceAttrib("Y", kFloat3_GrVertexAttribType);

        SkASSERT(offsetof(TriPointInstance, fX) == this->getAttrib(kAttribIdx_X).fOffsetInRecord);
        SkASSERT(offsetof(TriPointInstance, fY) == this->getAttrib(kAttribIdx_Y).fOffsetInRecord);
        SkASSERT(sizeof(TriPointInstance) == this->getInstanceStride());
    }

    if (fVertexBuffer) {
        SkASSERT(kAttribIdx_VertexData == this->numAttribs());
        this->addVertexAttrib("vertexdata", kInt_GrVertexAttribType);

        SkASSERT(sizeof(int32_t) == this->getVertexStride());
    }

    if (caps.usePrimitiveRestart()) {
        this->setWillUsePrimitiveRestart();
        fPrimitiveType = GrPrimitiveType::kTriangleStrip;
    } else {
        fPrimitiveType = GrPrimitiveType::kTriangles;
    }
}

void GrCCCoverageProcessor::appendVSMesh(GrBuffer* instanceBuffer, int instanceCount,
                                         int baseInstance, SkTArray<GrMesh>* out) const {
    SkASSERT(Impl::kVertexShader == fImpl);
    GrMesh& mesh = out->emplace_back(fPrimitiveType);
    mesh.setIndexedInstanced(fIndexBuffer.get(), fNumIndicesPerInstance, instanceBuffer,
                             instanceCount, baseInstance);
    if (fVertexBuffer) {
        mesh.setVertexData(fVertexBuffer.get(), 0);
    }
}

GrGLSLPrimitiveProcessor* GrCCCoverageProcessor::createVSImpl(std::unique_ptr<Shader> shadr) const {
    return new VSImpl(std::move(shadr));
}
