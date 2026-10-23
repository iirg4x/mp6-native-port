#pragma once

// This file is injected into StorybookView's public section by the isolated
// full-frame probe.  Keep it class-body-only: the probe supplies <cstdio> and
// <cstdlib>, while this method deliberately uses no renderer or context.
static void verify_direct_mesh_batching() {
    StorybookView actual(Rml::String("storybook-view"));
    StorybookView reference(Rml::String("storybook-view"));

    auto require = [](bool condition, size_t caseNumber, const char* detail) {
        if (condition) return;
        std::fprintf(stderr, "[STORYBOOK-MESH-QA] FAIL case=%zu %s\n", caseNumber, detail);
        std::abort();
    };
    auto fail_mesh = [&](size_t caseNumber, const char* detail, size_t index,
                         size_t actualSize, size_t referenceSize) {
        std::fprintf(stderr,
            "[STORYBOOK-MESH-QA] FAIL case=%zu %s at=%zu actual=%zu reference=%zu\n",
            caseNumber, detail, index, actualSize, referenceSize);
        std::abort();
    };
    auto compare_mesh = [&](size_t caseNumber, const Rml::Mesh& lhs, const Rml::Mesh& rhs) {
        if (lhs.vertices.size() != rhs.vertices.size())
            fail_mesh(caseNumber, "vertex-count mismatch", 0, lhs.vertices.size(), rhs.vertices.size());
        if (lhs.indices.size() != rhs.indices.size())
            fail_mesh(caseNumber, "index-count mismatch", 0, lhs.indices.size(), rhs.indices.size());
        for (size_t i = 0; i < lhs.vertices.size(); ++i) {
            const auto& a = lhs.vertices[i];
            const auto& b = rhs.vertices[i];
            if (a.position.x != b.position.x || a.position.y != b.position.y)
                fail_mesh(caseNumber, "position mismatch", i, lhs.vertices.size(), rhs.vertices.size());
            if (a.colour.red != b.colour.red || a.colour.green != b.colour.green ||
                a.colour.blue != b.colour.blue || a.colour.alpha != b.colour.alpha)
                fail_mesh(caseNumber, "colour mismatch", i, lhs.vertices.size(), rhs.vertices.size());
            if (a.tex_coord.x != b.tex_coord.x || a.tex_coord.y != b.tex_coord.y)
                fail_mesh(caseNumber, "tex_coord mismatch", i, lhs.vertices.size(), rhs.vertices.size());
        }
        for (size_t i = 0; i < lhs.indices.size(); ++i)
            if (lhs.indices[i] != rhs.indices[i])
                fail_mesh(caseNumber, "index mismatch", i, lhs.indices.size(), rhs.indices.size());
    };

    // Generate regular 3/10/48-point convex inputs at one center/radius, so
    // their changing edge-normal input is the point count.  The 5-point case
    // is intentionally retained from the original probe's irregular pentagon.
    const std::array<size_t, 4> polygonCounts{{3, 5, 10, 48}};
    std::array<std::array<Rml::Vector2f, 48>, 4> sourcePolygons{};
    for (size_t polygonIndex = 0; polygonIndex < polygonCounts.size(); ++polygonIndex) {
        const size_t count = polygonCounts[polygonIndex];
        if (count == 5) {
            sourcePolygons[polygonIndex][0] = {240.25f, 180.5f};
            sourcePolygons[polygonIndex][1] = {426.75f, 196.25f};
            sourcePolygons[polygonIndex][2] = {478.5f, 342.75f};
            sourcePolygons[polygonIndex][3] = {334.125f, 451.5f};
            sourcePolygons[polygonIndex][4] = {184.75f, 319.875f};
            continue;
        }
        const float centerX = 331.625f, centerY = 316.25f;
        const float radiusX = 183.5f, radiusY = 137.25f;
        for (size_t i = 0; i < count; ++i) {
            const float angle = -1.5707963f + 6.2831853f * float(i) / float(count);
            sourcePolygons[polygonIndex][i] = {
                centerX + std::cos(angle) * radiusX,
                centerY + std::sin(angle) * radiusY};
        }
    }
    const std::array<Rml::Vector2f, 3> axisScales{{
        {1.31f, .73f}, {.67f, 1.44f}, {1.09f, .91f}}};
    const std::array<float, 3> scales{{.82f, 1.11f, 1.43f}};
    const std::array<Rml::Vector2f, 2> origins{{
        {101.25f, 202.5f}, {704.75f, 418.125f}}};
    const std::array<Layer, 5> layers{{
        Layer::Book, Layer::Day, Layer::Night, Layer::Brighton, Layer::Twila}};
    const std::array<int, 3> sides{{-1, 0, 1}};
    const std::array<float, 2> turns{{.19f, .74f}};
    const std::array<Rml::Vector2f, 2> glowCenters{{
        {572.375f, 271.875f}, {1078.625f, 611.25f}}};

    constexpr size_t glowVertexCount = 50; // center + the 48-slice closure ring
    constexpr size_t glowIndexCount = 144; // 48 fan triangles
    size_t caseNumber = 0;
    for (const bool reverse : {false, true}) {
        for (size_t polygonIndex = 0; polygonIndex < polygonCounts.size(); ++polygonIndex) {
            const size_t polygonPointCount = polygonCounts[polygonIndex];
            std::array<Rml::Vector2f, 48> points{};
            for (size_t i = 0; i < polygonPointCount; ++i)
                points[i] = reverse
                    ? sourcePolygons[polygonIndex][polygonPointCount - 1 - i]
                    : sourcePolygons[polygonIndex][i];
            for (const auto axisScale : axisScales) for (const float scale : scales)
                for (const bool proportional : {false, true}) for (const auto origin : origins)
                    for (const int side : sides) for (const auto layer : layers)
                        for (const bool moving : {false, true}) for (const float turn : turns)
                            for (const bool print : {false, true}) for (const bool glowFirst : {false, true}) {
        ++caseNumber;
        auto configure = [&](StorybookView& view) {
            view.mAxisScale = axisScale;
            view.mScale = scale;
            view.mProportional = proportional;
            view.mShapeOrigin = origin;
            view.mSide = side;
            view.mLayer = layer;
            view.mMoving = moving;
            view.mTurn = turn;
            view.mLayerOffset = {13.25f, -9.75f};
            view.mSkyTravel = 18.5f;
            view.mBookTranslation = 7.25f;
            view.mLeftReveal = .73f;
            view.mRightReveal = .68f;
            view.mBatch = {};
            view.mBatchTexture = {};
            view.mMaskBatch = {};
            view.mMaskBatchTexture = {};
            view.mMaskBatchShader = nullptr;
            view.prepare_mesh();
        };
        configure(actual);
        configure(reference);

        std::vector<PrintDraw> recordedUnprojectedMeshes;
        actual.mRecordingPrint = nullptr;
        reference.mRecordingPrint = &recordedUnprojectedMeshes;
        const auto glowCenter = glowCenters[(caseNumber / 2) % glowCenters.size()];
        auto build = [&](StorybookView& view) {
            if (glowFirst) {
                view.glow(glowCenter.x, glowCenter.y, 37.125f, 24.75f,
                    0xffb71f, .64f, print);
                view.smooth_polygon(points.data(), int(polygonPointCount), 0x5ab3ec, .71f, print);
            } else {
                view.smooth_polygon(points.data(), int(polygonPointCount), 0x5ab3ec, .71f, print);
                view.glow(glowCenter.x, glowCenter.y, 37.125f, 24.75f,
                    0xffb71f, .64f, print);
            }
        };
        build(actual);
        build(reference);

        require(recordedUnprojectedMeshes.size() == 2, caseNumber,
            "reference did not record exactly smooth+glow fallback meshes");
        const size_t smoothVertexCount = 1 + 2 * polygonPointCount;
        const size_t smoothIndexCount = 9 * polygonPointCount;
        const std::array<size_t, 2> expectedVertices = glowFirst
            ? std::array<size_t, 2>{{glowVertexCount, smoothVertexCount}}
            : std::array<size_t, 2>{{smoothVertexCount, glowVertexCount}};
        const std::array<size_t, 2> expectedIndices = glowFirst
            ? std::array<size_t, 2>{{glowIndexCount, smoothIndexCount}}
            : std::array<size_t, 2>{{smoothIndexCount, glowIndexCount}};
        size_t recordedOffset = 0;
        for (size_t drawIndex = 0; drawIndex < recordedUnprojectedMeshes.size(); ++drawIndex) {
            const auto& draw = recordedUnprojectedMeshes[drawIndex];
            require(draw.mesh.vertices.size() == expectedVertices[drawIndex], caseNumber,
                "recorded fallback vertex count changed");
            require(draw.mesh.indices.size() == expectedIndices[drawIndex], caseNumber,
                "recorded fallback index count changed");
            require(draw.print == print && draw.proportional == proportional &&
                    draw.origin.x == origin.x && draw.origin.y == origin.y, caseNumber,
                "recorded projection metadata changed");
            require(draw.boundary.empty() && draw.texture == Rml::Texture{}, caseNumber,
                "fallback unexpectedly recorded textured or boundary geometry");
            bool remainedUnprojected = false;
            for (size_t i = 0; i < draw.mesh.vertices.size(); ++i) {
                const auto& recorded = draw.mesh.vertices[i];
                const auto& projected = reference.mBatch.vertices[recordedOffset + i];
                if (recorded.position.x != projected.position.x ||
                    recorded.position.y != projected.position.y) {
                    remainedUnprojected = true;
                    break;
                }
            }
            require(remainedUnprojected, caseNumber,
                "reference recording did not retain unprojected fallback vertices");
            recordedOffset += draw.mesh.vertices.size();
        }
        require(recordedOffset == reference.mBatch.vertices.size(), caseNumber,
            "recorded fallback meshes do not cover the composed batch");

        compare_mesh(caseNumber, actual.mBatch, reference.mBatch);
        require(actual.mBatchTexture == Rml::Texture{} && reference.mBatchTexture == Rml::Texture{},
            caseNumber, "direct path changed the default untextured batch state");
        require(actual.mMaskBatch.vertices.empty() && actual.mMaskBatch.indices.empty() &&
                reference.mMaskBatch.vertices.empty() && reference.mMaskBatch.indices.empty(),
            caseNumber, "mesh self-test unexpectedly populated a mask batch");
        require(actual.mGeometry.empty() && reference.mGeometry.empty() &&
                actual.mRenderBatches == 0 && reference.mRenderBatches == 0 &&
                actual.mGeometryBuilds == 0 && reference.mGeometryBuilds == 0,
            caseNumber, "mesh self-test flushed or rendered geometry");
        actual.mRecordingPrint = nullptr;
        reference.mRecordingPrint = nullptr;
            }
        }
    }

    // Keep a few degenerate/concave cases separate from the broad convex
    // matrix.  These deliberately reuse the same actual/reference objects so
    // a direct-path edge-normal cache cannot leak between polygon counts.
    struct EdgeTransform {
        Rml::Vector2f axisScale, origin;
        float scale, turn;
        int side;
        Layer layer;
        bool proportional, moving, print;
    };
    const std::array<EdgeTransform, 3> edgeTransforms{{
        {{1.27f, .69f}, {120.5f, 214.25f}, .91f, .23f, -1, Layer::Book, false, false, false},
        {{.73f, 1.38f}, {668.75f, 407.5f}, 1.24f, .61f, 0, Layer::Day, true, true, true},
        {{1.16f, .84f}, {512.125f, 329.75f}, 1.07f, .47f, 1, Layer::Brighton, false, true, false}}};
    auto run_edge_case = [&](size_t id, const Rml::Vector2f* polygon, size_t count,
                             const EdgeTransform& transform) {
        auto configure = [&](StorybookView& view) {
            view.mAxisScale = transform.axisScale;
            view.mScale = transform.scale;
            view.mProportional = transform.proportional;
            view.mShapeOrigin = transform.origin;
            view.mSide = transform.side;
            view.mLayer = transform.layer;
            view.mMoving = transform.moving;
            view.mTurn = transform.turn;
            view.mLayerOffset = {13.25f, -9.75f};
            view.mSkyTravel = 18.5f;
            view.mBookTranslation = 7.25f;
            view.mLeftReveal = .73f;
            view.mRightReveal = .68f;
            view.mBatch = {};
            view.mBatchTexture = {};
            view.mMaskBatch = {};
            view.mMaskBatchTexture = {};
            view.mMaskBatchShader = nullptr;
            view.prepare_mesh();
        };
        configure(actual);
        configure(reference);
        std::vector<PrintDraw> recordedUnprojectedMeshes;
        actual.mRecordingPrint = nullptr;
        reference.mRecordingPrint = &recordedUnprojectedMeshes;
        actual.smooth_polygon(polygon, int(count), 0x5ab3ec, .71f, transform.print);
        reference.smooth_polygon(polygon, int(count), 0x5ab3ec, .71f, transform.print);
        require(recordedUnprojectedMeshes.size() == 1, id,
            "edge case did not record one retained fallback mesh");
        require(recordedUnprojectedMeshes[0].mesh.vertices.size() == 1 + 2 * count &&
                recordedUnprojectedMeshes[0].mesh.indices.size() == 9 * count, id,
            "edge case fallback mesh count changed");
        require(recordedUnprojectedMeshes[0].print == transform.print &&
                recordedUnprojectedMeshes[0].proportional == transform.proportional &&
                recordedUnprojectedMeshes[0].origin.x == transform.origin.x &&
                recordedUnprojectedMeshes[0].origin.y == transform.origin.y, id,
            "edge case fallback projection metadata changed");
        bool remainedUnprojected = false;
        for (size_t i = 0; i < recordedUnprojectedMeshes[0].mesh.vertices.size(); ++i) {
            const auto& recorded = recordedUnprojectedMeshes[0].mesh.vertices[i];
            const auto& projected = reference.mBatch.vertices[i];
            if (recorded.position.x != projected.position.x ||
                recorded.position.y != projected.position.y) {
                remainedUnprojected = true;
                break;
            }
        }
        require(remainedUnprojected, id,
            "edge case reference mesh was not retained in unprojected space");
        compare_mesh(id, actual.mBatch, reference.mBatch);
        require(actual.mBatchTexture == Rml::Texture{} && reference.mBatchTexture == Rml::Texture{},
            id, "edge case changed the default untextured batch state");
        require(actual.mMaskBatch.vertices.empty() && actual.mMaskBatch.indices.empty() &&
                reference.mMaskBatch.vertices.empty() && reference.mMaskBatch.indices.empty(),
            id, "edge case unexpectedly populated a mask batch");
        require(actual.mGeometry.empty() && reference.mGeometry.empty() &&
                actual.mRenderBatches == 0 && reference.mRenderBatches == 0 &&
                actual.mGeometryBuilds == 0 && reference.mGeometryBuilds == 0,
            id, "edge case flushed or rendered geometry");
        actual.mRecordingPrint = nullptr;
        reference.mRecordingPrint = nullptr;
    };

    std::array<Rml::Vector2f, 10> concaveStar{};
    for (size_t i = 0; i < concaveStar.size(); ++i) {
        const float angle = -1.5707963f + 6.2831853f * float(i) / float(concaveStar.size());
        const float radius = i % 2 ? 71.5f : 171.25f;
        concaveStar[i] = {548.75f + std::cos(angle) * radius,
                          356.125f + std::sin(angle) * radius};
    }
    const std::array<Rml::Vector2f, 10> duplicateEdges{{
        {220.f, 180.f}, {500.f, 180.f}, {500.f, 180.f}, {500.f, 420.f},
        {500.f, 420.f}, {220.f, 420.f}, {220.f, 420.f}, {220.f, 180.f},
        {220.f, 180.f}, {220.f, 180.f}}};
    for (const auto& transform : edgeTransforms) {
        ++caseNumber;
        run_edge_case(caseNumber, concaveStar.data(), concaveStar.size(), transform);
        ++caseNumber;
        std::array<Rml::Vector2f, 10> reverseStar{};
        std::array<Rml::Vector2f, 10> reverseDuplicates{};
        for (size_t i = 0; i < reverseStar.size(); ++i) {
            reverseStar[i] = concaveStar[reverseStar.size() - 1 - i];
            reverseDuplicates[i] = duplicateEdges[reverseDuplicates.size() - 1 - i];
        }
        run_edge_case(caseNumber, reverseStar.data(), reverseStar.size(), transform);
        ++caseNumber;
        run_edge_case(caseNumber, duplicateEdges.data(), duplicateEdges.size(), transform);
        ++caseNumber;
        run_edge_case(caseNumber, reverseDuplicates.data(), reverseDuplicates.size(), transform);
    }

    // Strip differential coverage: reproduce the pre-optimization builder
    // independently, then compare its submitted/projection result with the
    // direct append path. Include page warps, local proportional projection,
    // winding/UV flips, a pre-existing batch offset, and retained-print replay.
    auto original_strip = [&](StorybookView& view,float x,float y,float w,float h,
                              float u0,float v0,float u1,float v1,float angle,
                              Rml::Texture texture,bool print,unsigned tint,float alpha) {
        const int slices=view.mSide<0?1:std::max(2,int(w/12));
        Rml::Mesh mesh;const auto c=view.color(tint,alpha);
        mesh.vertices.reserve(2*(slices+1));mesh.indices.reserve(6*slices);
        const float cosine=std::cos(angle),sine=std::sin(angle);
        for(int i=0;i<=slices;++i){const float u=float(i)/slices;
            for(int j=0;j<2;++j){float px=x+w*u,py=y+h*j;
                if(angle!=0){const float dx=(px-x-w/2)*view.mAxisScale.x,dy=(py-y-h/2)*view.mAxisScale.y;
                    px=x+w/2+(cosine*dx-sine*dy)/view.mAxisScale.x;
                    py=y+h/2+(sine*dx+cosine*dy)/view.mAxisScale.y;}
                mesh.vertices.push_back({{px,py},c,{u0+(u1-u0)*u,j?v1:v0}});}
            if(i){const int k=i*2;mesh.indices.insert(mesh.indices.end(),{k-2,k,k+1,k-2,k+1,k-1});}}
        std::vector<int> boundary;boundary.reserve(2*(slices+1));
        for(int i=0;i<=slices;++i)boundary.push_back(i*2);
        for(int i=slices;i>=0;--i)boundary.push_back(i*2+1);
        view.submit(std::move(mesh),texture,print,boundary);
    };
    auto configure_strip = [&](StorybookView& view,const Rml::Vector2f axisScale,float scale,
                               bool proportional,const Rml::Vector2f origin,int side,Layer layer,
                               bool moving,float turn) {
        view.mAxisScale=axisScale;view.mScale=scale;view.mProportional=proportional;
        view.mShapeOrigin=origin;view.mSide=side;view.mLayer=layer;view.mMoving=moving;view.mTurn=turn;
        view.mLayerOffset={13.25f,-9.75f};view.mSkyTravel=18.5f;view.mBookTranslation=7.25f;
        view.mLeftReveal=.73f;view.mRightReveal=.68f;view.mBatch={};view.mBatchTexture={};
        view.mMaskBatch={};view.mMaskBatchTexture={};view.mMaskBatchShader=nullptr;
        view.mRecording=nullptr;view.mRecordingPrint=nullptr;view.prepare_mesh();
    };
    auto seed_strip_batch = [](StorybookView& view) {
        const Rml::ColourbPremultiplied white{255,255,255,255};
        view.mBatch.vertices={{{-17.f,9.f},white,{.25f,.75f}},
                              {{31.f,9.f},white,{.75f,.75f}},
                              {{-17.f,43.f},white,{.25f,.25f}}};
        view.mBatch.indices={0,1,2};view.mBatchTexture={};
    };
    auto compare_print_records = [&](size_t id,const std::vector<PrintDraw>& lhs,const std::vector<PrintDraw>& rhs) {
        require(lhs.size()==rhs.size(),id,"strip fallback record count changed");
        for(size_t i=0;i<lhs.size();++i){
            compare_mesh(id,lhs[i].mesh,rhs[i].mesh);
            require(lhs[i].texture==rhs[i].texture&&lhs[i].print==rhs[i].print&&
                    lhs[i].proportional==rhs[i].proportional&&lhs[i].origin.x==rhs[i].origin.x&&
                    lhs[i].origin.y==rhs[i].origin.y&&lhs[i].boundary==rhs[i].boundary,id,
                    "strip fallback metadata changed");
        }
    };
    struct StripCase {
        float x,y,w,h,u0,v0,u1,v1,angle,alpha,scale,turn;
        Rml::Vector2f axisScale,origin;
        int side;Layer layer;bool proportional,moving,print,seed;
    };
    const std::array<StripCase,6> stripCases{{
        {-220.f,118.f,816.f,126.f,.91f,.08f,.12f,.96f,.19f,.67f,.82f,.23f,{1.31f,.73f},{101.25f,202.5f},-1,Layer::Day,false,false,false,true},
        {946.f,204.f,540.f,224.f,.87f,.12f,.11f,.94f,-.31f,.53f,1.11f,.74f,{.67f,1.44f},{704.75f,418.125f},0,Layer::Book,true,true,true,true},
        {117.f,42.f,503.f,274.f,.08f,.93f,.96f,.07f,.47f,.81f,1.43f,.61f,{1.09f,.91f},{331.625f,316.25f},1,Layer::Twila,false,true,true,false},
        {12.f,77.f,816.f,95.f,0.f,1.f,1.f,0.f,0.f,.44f,.91f,.19f,{.73f,1.38f},{512.125f,329.75f},0,Layer::Book,true,false,false,false},
        {31.f,91.f,287.f,48.f,.2f,.8f,.83f,.17f,-.62f,.36f,1.24f,.47f,{1.16f,.84f},{120.5f,214.25f},1,Layer::Brighton,false,true,false,true},
        {0.f,0.f,12.f*129.f+3.f,64.f,.7f,.3f,.1f,.9f,.27f,.72f,.91f,.23f,{1.27f,.69f},{120.5f,214.25f},0,Layer::Day,false,false,false,false}
    }};
    for(const auto& test:stripCases){
        ++caseNumber;
        configure_strip(actual,test.axisScale,test.scale,test.proportional,test.origin,test.side,test.layer,test.moving,test.turn);
        configure_strip(reference,test.axisScale,test.scale,test.proportional,test.origin,test.side,test.layer,test.moving,test.turn);
        if(test.seed){seed_strip_batch(actual);seed_strip_batch(reference);}
        original_strip(reference,test.x,test.y,test.w,test.h,test.u0,test.v0,test.u1,test.v1,test.angle,{},test.print,0x7ac4ef,test.alpha);
        actual.strip(test.x,test.y,test.w,test.h,{},test.u0,test.v0,test.u1,test.v1,test.angle,test.print,0x7ac4ef,test.alpha);
        compare_mesh(caseNumber,actual.mBatch,reference.mBatch);
        require(actual.mBatchTexture==reference.mBatchTexture&&actual.mBatchTexture==Rml::Texture{},caseNumber,
                "strip direct path changed texture state");
        require(actual.mMaskBatch.vertices.empty()&&actual.mMaskBatch.indices.empty()&&reference.mMaskBatch.vertices.empty()&&
                reference.mMaskBatch.indices.empty(),caseNumber,"strip direct path touched mask batch");
        require(actual.mGeometry.empty()&&reference.mGeometry.empty()&&actual.mRenderBatches==0&&reference.mRenderBatches==0,
                caseNumber,"strip direct path flushed geometry");
    }
    // Retained print replay must stay on the original unprojected mesh path,
    // including the boundary list used to rebuild its one-pixel fringe.
    for(const auto& test:std::array<StripCase,3>{{stripCases[0],stripCases[1],stripCases[2]}}){
        ++caseNumber;
        configure_strip(actual,test.axisScale,test.scale,test.proportional,test.origin,test.side,test.layer,test.moving,test.turn);
        configure_strip(reference,test.axisScale,test.scale,test.proportional,test.origin,test.side,test.layer,test.moving,test.turn);
        if(test.seed){seed_strip_batch(actual);seed_strip_batch(reference);}
        std::vector<PrintDraw> actualRecords,referenceRecords;
        actual.mRecordingPrint=&actualRecords;reference.mRecordingPrint=&referenceRecords;
        actual.strip(test.x,test.y,test.w,test.h,{},test.u0,test.v0,test.u1,test.v1,test.angle,test.print,0x7ac4ef,test.alpha);
        original_strip(reference,test.x,test.y,test.w,test.h,test.u0,test.v0,test.u1,test.v1,test.angle,{},test.print,0x7ac4ef,test.alpha);
        compare_mesh(caseNumber,actual.mBatch,reference.mBatch);compare_print_records(caseNumber,actualRecords,referenceRecords);
        require(actualRecords.size()==1&&!actualRecords[0].mesh.vertices.empty(),caseNumber,
                "strip retained fallback did not preserve local mesh");
        const size_t seedCount=test.seed?3:0;bool differs=false;
        for(size_t i=0;i<actualRecords[0].mesh.vertices.size()&&seedCount+i<actual.mBatch.vertices.size();++i){
            const auto& recorded=actualRecords[0].mesh.vertices[i];const auto& projected=actual.mBatch.vertices[seedCount+i];
            if(recorded.position.x!=projected.position.x||recorded.position.y!=projected.position.y){differs=true;break;}
        }
        require(differs,caseNumber,"strip retained fallback was not kept in local space");
        actual.mRecordingPrint=nullptr;reference.mRecordingPrint=nullptr;
    }
    std::printf("[STORYBOOK-MESH-QA] direct batching matched retained fallback (%zu scenarios)\n", caseNumber);
}
