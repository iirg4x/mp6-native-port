// Native presentation of docs/design/storybook-generated. The Rml controls own
// settings and input; this element only consumes their current labels/values.
// Intent: browsing MP6's playful day/night book. Ivory layered paper, red ribbon,
// gold cutouts, black Titan One ink and blue descriptions; 73-unit row rhythm.
#pragma once
#include "storybook_art_data.hpp"
#include "storybook_geometry.hpp"
#include "storybook_sky_mesh.hpp"
#include "ui.hpp"
#include <aurora/rmlui.hpp>
#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/RenderManager.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace mp6::ui {

struct StorybookRow {
    Rml::String label, value;
    bool adjustable = false, disabled = false, canLeft = true, canRight = true;
    bool operator==(const StorybookRow &) const = default;
};
struct StorybookState {
    std::vector<StorybookRow> rows;
    std::vector<Rml::String> chapters;
    Rml::String chapter = "Picture", description;
    int chapterIndex = 0, selected = 0;
    bool motion = true, inGame = false, detail = false;
    bool launcher = false, linked = false, wordmark = false;
    Rml::String version;
    bool operator==(const StorybookState &) const = default;
};

class StorybookView final : public Rml::Element {
public:
    explicit StorybookView(const Rml::String &tag) : Rml::Element(tag) {}
    // Called during UI startup, before either launcher or gameplay is presented.
    // Rml's file-texture database owns these entries for the renderer lifetime;
    // merely calling LoadTexture does not decode them, but GetDimensions does.
    static std::vector<Rml::Texture> preload_resources(Rml::Context &context) {
        auto &manager = context.GetRenderManager();
        std::vector<Rml::Texture> textures;
        const auto load = [&](const Rml::String &name) {
            auto texture = manager.LoadTexture(
                resource_path("menu/storybook/" + name + ".png").generic_string());
            texture.GetDimensions();
            textures.push_back(texture);
        };
        for (const auto &sprite : storybook_art::sprites) {
            if (Rml::String(sprite.name) == "book-half")
                continue; // superseded source art
            load(sprite.name);
        }
        const auto viewport = context.GetDimensions();
        const float scale = std::min(viewport.x / width, viewport.y / height);
        auto *engine = Rml::GetFontEngineInterface();
        const Rml::String
            glyphs =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 %.,:;!?+-/()[]'\"&<>×…–",
            language = "en";
        for (float size : {15.f, 20.f, 23.f, 25.f, 26.f, 30.f, 32.f, 34.f, 35.f, 46.f}) {
            const int pixels = std::max(8, int(std::round(size * scale)));
            const auto face = engine->GetFontFaceHandle("titan one", Rml::Style::FontStyle::Normal,
                                                        Rml::Style::FontWeight::Normal, pixels);
            if (!face)
                continue;
            Rml::TexturedMeshList meshes;
            engine->GenerateString(manager, face, 0, glyphs, {0, 0}, {255, 255, 255, 255}, 1,
                                   {language}, meshes);
            for (const auto &mesh : meshes) {
                mesh.texture.GetDimensions();
                textures.push_back(mesh.texture);
            }
        }
        return textures;
    }
    static constexpr float width = storybook_geometry::design_width,
                           height = storybook_geometry::design_height, half = 816, bookY = 82,
                           bookH = 779, spine = 836;
    static constexpr float launcherRowTop = 298, launcherRowStride = 112, launcherRowHeight = 84;
    // Preserve the approved stagger, at half the original intro/outro speed.
    // Page turns and navigation feedback retain their independent timings.
    static constexpr double introOutroTimeScale = 2.0;
    static constexpr double turnDuration = .28, entranceDuration = .68 * introOutroTimeScale,
                            exitDuration = .40 * introOutroTimeScale;
    double presentation_progress() const {
        return mPresentation.progress(clock(), entranceDuration, exitDuration);
    }
    void spring_chapters(bool motion, double delay = 0) {
        mChaptersStart = motion ? clock() + delay : -100;
    }
    void enter(bool motion) {
        const bool fresh = !mPresentation.started || presentation_progress() == 0;
        mPresentation.target(true, motion, clock(), entranceDuration, exitDuration);
        if (fresh)
            spring_chapters(motion, .26);
    }
    void leave(bool motion) {
        mPresentation.target(false, motion, clock(), entranceDuration, exitDuration);
    }
    bool exit_finished() const {
        return !mPresentation.opening && (!mState.motion || presentation_progress() == 0);
    }
    // Book is interactive at 600 ms; the decorative hosts finish their later pops.
    bool entering() const {
        return mState.motion && mPresentation.opening &&
               presentation_progress() * entranceDuration < .30 * introOutroTimeScale;
    }
    // Copy only the printed spread, never the source's scene/entrance lifecycle.
    // The destination begins its turn when its first real settings state arrives.
    void turn_from(const StorybookView &source) {
        mPrevious = source.mState;
        mPendingHandoff = source.mInitialized;
    }
    bool handing_off() const { return mPendingHandoff || (mHandoff && turning()); }
    float chapter_offset(int index) const {
        return mState.motion
                   ? storybook_geometry::chapter_tab_offset(float(clock() - mChaptersStart), index)
                   : 0;
    }
    void set_state(StorybookState next) {
        const double now = clock();
        if (!mPendingHandoff && mInitialized && next == mState)
            return;
        ++mStateRevision;
        if (mPendingHandoff) {
            mPendingHandoff = false;
            mHandoff = true;
            mForward = true;
            mTurnStart = mChaptersStart = next.motion ? now : -100;
            mSelectionFrom = float(next.selected % 6);
            mSelectionStart = mValueStart = mDescriptionStart = mTabStart = -100;
            mState = std::move(next);
            mInitialized = true;
            return;
        }
        if (mHandoff && !turning())
            mHandoff = false;
        if (mInitialized && next.description != mState.description) {
            mOldDescription = mState.description;
            mDescriptionStart = now;
        }
        if (mInitialized && next.chapterIndex != mState.chapterIndex)
            mTabStart = now;
        if (mInitialized && (next.chapterIndex != mState.chapterIndex ||
                             next.selected / 6 != mState.selected / 6)) {
            mPrevious = mState;
            mForward = next.chapterIndex != mState.chapterIndex
                           ? next.chapterIndex > mState.chapterIndex
                           : next.selected > mState.selected;
            mTurnStart = next.motion && !next.detail ? now : -100;
        } else if (mInitialized && next.selected != mState.selected) {
            mSelectionFrom = row_position(now);
            mSelectionStart = now;
        }
        if (mInitialized && next.selected < int(next.rows.size()) &&
            mState.selected < int(mState.rows.size()) &&
            next.rows[next.selected].value != mState.rows[mState.selected].value)
            mValueStart = now;
        if (!mInitialized || next.selected / 6 != mState.selected / 6 ||
            next.chapterIndex != mState.chapterIndex) {
            mSelectionFrom = float(next.selected % 6);
            mSelectionStart = -100;
        }
        if (!next.motion) {
            mTurnStart = mSelectionStart = mValueStart = mDescriptionStart = mTabStart = -100;
        }
        mState = std::move(next);
        mInitialized = true;
    }
    bool turning() const { return mState.motion && clock() - mTurnStart < turnDuration; }
    const StorybookState &state() const { return mState; }
    size_t render_batch_count() const { return mRenderBatches; }
    size_t geometry_build_count() const { return mGeometryBuilds; }
    size_t geometry_reuse_count() const { return mGeometryReuses; }
    size_t text_generation_count() const { return mTextGenerations; }
    size_t text_cache_count() const { return mText.size(); }
    size_t render_call_count() const { return mRenderCalls; }
    // Design-space y shift of the prompt strip; footer hit tests add it.
    float footer_lift() const { return mFooterLift; }
    static constexpr float footerBottom = 933;
    Rml::Vector2f design_point(float x, float y) {
        const auto p = GetAbsoluteOffset(Rml::BoxArea::Content);
        const auto size = GetBox().GetSize(Rml::BoxArea::Content);
        const auto point =
            storybook_geometry::fit_book(size.x, size.y).unproject({x - p.x, y - p.y});
        return {point.x, point.y};
    }
    static float unwarp_y(float x, float y, int side = 0) {
        const float u = std::clamp((x - 20 - side * half) / half, 0.f, 1.f);
        const float drop = 27 * std::pow(side ? 1 - u : u, 10.f);
        y = bookY + (y - bookY - drop) * bookH / (bookH - drop);
        const auto band = paper_band(side, x);
        return bookY + (y - bookY - band.top) * bookH / (band.bottom - band.top);
    }
    struct PreviewLayout {
        storybook_geometry::Rect photo, mount, caption;
        float captionBaseline = 0;
    };
    // Photo uses unwarped print coordinates; mount/caption bounds include paper
    // curvature, in design units. One layout drives both drawing and QA bounds.
    PreviewLayout preview_layout(float sceneWidth, float sceneHeight, const Rml::String &caption) {
        return framed_layout(
            storybook_geometry::preview_photo(mAxisScale.x, mAxisScale.y, sceneWidth, sceneHeight),
            caption);
    }
    PreviewLayout framed_layout(storybook_geometry::Rect photo, const Rml::String &caption) {
        PreviewLayout layout{};
        layout.photo = photo;
        const auto p = layout.photo;
        float top = height, bottom = 0;
        for (int i = 0; i <= 128; ++i) {
            const float x = p.x - 14 + (p.w + 28) * i / 128;
            top = std::min(top, paper_y(1, x, p.y - 14));
            bottom = std::max(bottom, paper_y(1, x, p.y + p.h + 18));
        }
        layout.mount = {p.x - 14, top, p.w + 28, bottom - top};
        const float captionTop = bottom + (14 * mScale + 2) / mAxisScale.y;
        const auto placed = place_text(caption, 1216, 26, 1, 580);
        for (const auto &item : placed.run->meshes)
            for (const auto &vertex : item.mesh.vertices) {
                const float x = placed.left + vertex.position.x * placed.scale.x;
                layout.captionBaseline =
                    std::max(layout.captionBaseline,
                             unwarp_y(x, captionTop, 1) - vertex.position.y * placed.scale.y);
            }
        float left = width, right = 0;
        top = height;
        bottom = 0;
        for (const auto &item : placed.run->meshes)
            for (const auto &vertex : item.mesh.vertices) {
                const float x = placed.left + vertex.position.x * placed.scale.x;
                const float y =
                    paper_y(1, x, layout.captionBaseline + vertex.position.y * placed.scale.y);
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        layout.caption = {left, top, right - left, bottom - top};
        return layout;
    }

protected:
    void OnRender() override {
        if (!mInitialized || !GetRenderManager())
            return;
        const auto size = GetBox().GetSize(Rml::BoxArea::Content);
        const float nextScale = std::min(size.x / width, size.y / height);
        if (nextScale <= 0)
            return;
        mFrameTime = wall_clock();
        mRendering = true;
        ++mRenderCalls;
        mRenderBatches = 0;
        // Compiled ink retains font-atlas UVs. A changed atlas must invalidate
        // retained pages as well as the unprojected text runs.
        auto *engine = Rml::GetFontEngineInterface();
        bool fontsChanged = mRetryText;
        for (const auto &[face, version] : mFontVersions)
            if (engine->GetVersion(face) != version) {
                fontsChanged = true;
                break;
            }
        const float dpi = GetContext()->GetDensityIndependentPixelRatio();
        if (mText.size() > 2048) {
            clear_text_cache();
            fontsChanged = true;
        }
        if (size != mLastSize || dpi != mLastDpi || fontsChanged) {
            ++mLayoutRevision;
            mLastSize = size;
            mLastDpi = dpi;
            mWrapped.clear();
            if (fontsChanged)
                clear_text_cache();
        }
        if (std::abs(nextScale - mScale) > .0001f) {
            clear_text_cache();
            mScale = nextScale;
        }
        mAxisScale = {size.x / width, size.y / height};
        mOffset = GetAbsoluteOffset(Rml::BoxArea::Content);
        mLayerOffset = {0, 0};
        const auto fit = storybook_geometry::fit_book(size.x, size.y);
        mBookOffset = {fit.offset.x, fit.offset.y};
        mBookTravel = (size.y - fit.offset.y) / mScale + 16;
        mSkyReveal = reveal(0, .16);
        mBookReveal = reveal(.08, .22);
        mLeftReveal = host_reveal(.29, .26);
        mRightReveal = host_reveal(.40, .26);
        mSkyTravel = width * .55f * (1 - out(mSkyReveal));
        mBookTranslation = mBookTravel * (1 - out(mBookReveal));
        const auto scene = aurora::rmlui::scene_dimensions();
        if (scene != mLastScene) {
            mLastScene = scene;
            ++mPhotoRevision;
        }
        // Explicit painter order. No depth testing, coplanar layers or cached row
        // ink behind the moving copy. Borders are part of the paper below print.
        recycle_frame_geometry();
        mGeometry.clear();
        mSide = -1;
        mMoving = false;
        const float t = mState.motion ? float(clock()) : 0;
        const auto oldScissor = GetRenderManager()->GetScissorRegion();
        GetRenderManager()->SetScissorRegion(Rml::Rectanglei::FromPositionSize(
            {int(mOffset.x), int(mOffset.y)}, {int(size.x), int(size.y)}));
        retained(Cache::Sky, !mState.motion, mPresentation.opening, [&] { scenery(t); });
        // Keep the sky full-window, then contain the complete book composition
        // with one uniform scale. Its hosts, controls and paper move together.
        mAxisScale = {mScale, mScale};
        mLayerOffset = {fit.offset.x, fit.offset.y};
        mLayer = Layer::Book;
        const bool bookStill = mBookReveal == 1;
        const bool tabsStill =
            !mState.motion ||
            (clock() - mTabStart >= .48 &&
             clock() - mChaptersStart >= .48 + .1 * std::max(0, int(mState.chapters.size()) - 1));
        retained(Cache::Tabs, bookStill && tabsStill, mStateRevision, [&] { tabs(t); });
        retained(Cache::Binding, bookStill, 0, [&] {
            binding();
            for (int side = 0; side < 2; ++side) {
                mSide = side;
                art(side ? "book-right" : "book-left", 20 + side * half, bookY, half, bookH, 0,
                    false, false);
                // Same face at rest and in flight: no mirrored ornament or silhouette
                // swap on the last frame. Only the hardcover remains underneath.
                art(side ? "paper-right" : "paper-left", 20 + side * half, bookY, half, bookH, 0,
                    false, false);
            }
            mSide = -1;
            crease();
        });
        mSide = -1;
        const bool turn = turning();
        const float progress = float(std::clamp((clock() - mTurnStart) / turnDuration, 0., 1.));
        if (turn) {
            mSide = 0;
            const auto &left = mForward ? mPrevious : mState;
            retained(Cache::TurnLeft, bookStill, mStateRevision,
                     [&] { page(left, t, false, Print::Ink); });
            page(left, t, false, Print::Cursor);
            mSide = 1;
            auto &underlay = mRetained[size_t(Cache::TurnRight)];
            if (mTurnPhotoRevision != mPhotoRevision) {
                mTurnPhotoRevision = mPhotoRevision;
                underlay.valid = false;
            }
            retained(Cache::TurnRight, bookStill, mStateRevision,
                     [&] { page(mForward ? mState : mPrevious, t, false, Print::Ink); });
            mTurn = mForward ? ease(progress) : 1 - ease(progress);
            prepare_mesh();
            mMoving = true;
            mSide = mTurn < .5f ? 1 : 0;
            // A single opaque sheet and its print use exactly the same mesh.
            const auto &face =
                mSide == 1 ? (mForward ? mPrevious : mState) : (mForward ? mState : mPrevious);
            moving_print(face, t);
            if (mSide == 0)
                page(face, t, false, Print::Cursor);
            mMoving = false;
        } else {
            mSide = 0;
            const bool inkStill = !mState.motion || (clock() - mSelectionStart >= .19 &&
                                                     clock() - mValueStart >= .24);
            retained(Cache::LeftPage, bookStill && inkStill, mStateRevision,
                     [&] { page(mState, t, true, Print::Ink); });
            retained(Cache::Cursor, bookStill && !mState.motion, mStateRevision,
                     [&] { page(mState, t, true, Print::Cursor); });
            mSide = 1;
            if (!mState.detail) {
                // The borrowed texture handle is stable; Aurora resolves its
                // current GPU view on every draw, including after AA/resize.
                auto &right = mRetained[size_t(Cache::RightPage)];
                if (mPhotoStateRevision != mStateRevision) {
                    mPhotoStateRevision = mStateRevision;
                    right.valid = false;
                }
                const size_t photoRevision = mPhotoRevision;
                retained(Cache::RightPage,
                         bookStill && (!mState.motion || clock() - mDescriptionStart >= .20),
                         photoRevision, [&] { page(mState, t, true, Print::Ink); });
            }
        }
        mSide = -1;
        retained(Cache::Hosts, !mState.motion, mPresentation.opening, [&] { foreground(t); });
        mLayer = Layer::Book;
        // Lift the prompt strip clear of the system's bottom gesture zone
        // (iOS swipe-home / Safari's toolbar) when the safe area says so; the
        // rest of the book keeps its place.
        {
            const float inset = safe_area_insets(GetContext()).bottom;
            const float spare = size.y - (fit.offset.y + footerBottom * fit.scale);
            mFooterLift = inset > 0 ? std::round(std::max(0.f, (inset - spare) / fit.scale + 6.f)) : 0.f;
        }
        retained(Cache::Footer, bookStill, size_t(mState.launcher) + size_t(mFooterLift) * 2, [&] { footer(); });
        flush_mask_batch();
        flush_batch(); // Submit before restoring the element's scissor.
        GetRenderManager()->SetScissorRegion(oldScissor);
        mRendering = false;
    }

private:
    struct TextRun {
        Rml::TexturedMeshList meshes;
        float width = 0;
        int version = 0;
        Rml::FontFaceHandle face = 0;
    };
    struct Lift {
        float x, z;
    };
    enum class Print { All, Ink, Cursor };
    enum class Cache {
        Sky,
        DayPanel,
        NightPanel,
        Tabs,
        Binding,
        LeftPage,
        Cursor,
        RightPage,
        TurnLeft,
        TurnRight,
        Hosts,
        Footer,
        Count
    };
    struct Draw {
        Rml::Geometry geometry;
        Rml::Texture texture;
        const Rml::CompiledShader *shader = nullptr;
    };
    struct Retained {
        std::vector<Draw> draws;
        size_t layout = 0, revision = 0;
        bool valid = false;
    };
    struct PrintDraw {
        Rml::Mesh mesh;
        Rml::Texture texture;
        bool print, proportional;
        Rml::Vector2f origin;
        std::vector<int> boundary;
    };
    struct PrintedFace {
        std::vector<PrintDraw> draws;
        size_t layout = 0, state = 0, photo = 0;
        bool valid = false;
    };
    enum class Layer { Day, Night, Book, Brighton, Twila };
    Layer mLayer = Layer::Book;
    StorybookState mState, mPrevious;
    bool mInitialized = false, mForward = true, mMoving = false, mProportional = false,
         mPendingHandoff = false, mHandoff = false;
    int mSide = -1;
    float mScale = 1, mTurn = 0, mSelectionFrom = 0;
    float mFooterLift = 0; // design units the prompt strip moves up (see render)
    double mTurnStart = -100, mSelectionStart = -100, mValueStart = -100, mDescriptionStart = -100,
           mTabStart = -100;
    double mChaptersStart = -100;
    storybook_geometry::BookTransition mPresentation;
    float mSkyReveal = 1, mBookReveal = 1, mLeftReveal = 1, mRightReveal = 1,
          mBookTravel = height + 16;
    float mSkyTravel = 0, mBookTranslation = 0;
    Rml::String mOldDescription;
    Rml::Vector2f mOffset, mAxisScale{1, 1}, mShapeOrigin, mLayerOffset;
    std::array<Lift, 65> mLift{};
    std::map<Rml::String, Rml::Texture> mTextures;
    std::map<Rml::String, TextRun> mText;
    std::map<Rml::FontFaceHandle, int> mFontVersions;
    bool mRetryText = false;
    std::map<Rml::String, std::vector<Rml::String>> mWrapped;
    std::vector<storybook_geometry::MaskTile> mMaskTiles;
    size_t mMaskLayout = size_t(-1), mPhotoRevision = 0, mPhotoStateRevision = 0;
    size_t mTurnPhotoRevision = 0;
    std::array<PrintedFace, 2> mPrintedFaces;
    std::vector<PrintDraw> *mRecordingPrint = nullptr;
    Rml::Vector2i mLastScene;
    Rml::Vector2f mBookOffset;
    std::vector<Draw> mGeometry;
    std::array<Retained, size_t(Cache::Count)> mRetained;
    std::vector<Draw> *mRecording = nullptr;
    size_t mStateRevision = 0, mLayoutRevision = 0, mRenderBatches = 0, mGeometryBuilds = 0,
           mGeometryReuses = 0, mTextGenerations = 0, mRenderCalls = 0;
    Rml::Vector2f mLastSize;
    float mLastDpi = 0;
    double mFrameTime = 0;
    bool mRendering = false;
    Rml::Mesh mBatch;
    Rml::Texture mBatchTexture;
    Rml::Mesh mMaskBatch;
    Rml::Texture mMaskBatchTexture;
    const Rml::CompiledShader *mMaskBatchShader = nullptr;
    // ReleaseGeometry returns the source mesh with its vector capacities
    // intact when ClearMesh is requested. Keep only a few of the largest
    // dynamic meshes so animated frames can reuse that storage without
    // retaining any live GPU geometry or the much longer-lived retained
    // page/print caches. Four entries cover the normal, mask, and two
    // occasional boundary batches while bounding CPU memory.
    static constexpr size_t scratchMeshLimit = 4;
    std::array<Rml::Mesh, scratchMeshLimit> mScratchMeshes;
    size_t mScratchMeshCount = 0;
    size_t mBatchVertexHint = 0, mBatchIndexHint = 0;
    size_t mMaskBatchVertexHint = 0, mMaskBatchIndexHint = 0;
    std::array<Rml::CompiledShader, 2> mMaskShaders;
    struct SkyMeshBatch {
        Rml::Mesh mesh;
        Rml::Geometry geometry;
        std::array<aurora::rmlui::GeometryTransform, aurora::rmlui::GeometryTransformPaletteSize>
            transforms{};
        size_t count = 0;
    };
    struct SkyMeshGroup {
        std::vector<SkyMeshBatch> batches;
        size_t layout = size_t(-1), instances = 0;
        bool building = false;
    };
    std::array<SkyMeshGroup, 1> mSkyMeshes;
    Rml::CompiledShader mSkyTransformShader;
    static double wall_clock() {
        using C = std::chrono::steady_clock;
        static const auto start = C::now();
        return std::chrono::duration<double>(C::now() - start).count();
    }
    double clock() const { return mRendering ? mFrameTime : wall_clock(); }
    void render_draw(const Draw &draw) {
        if (draw.shader)
            draw.geometry.Render(mOffset, draw.texture, *draw.shader);
        else
            draw.geometry.Render(mOffset, draw.texture);
        ++mRenderBatches;
    }
    static size_t scratch_capacity(const Rml::Mesh &mesh) {
        return mesh.vertices.capacity() + mesh.indices.capacity();
    }
    void retain_scratch_mesh(Rml::Mesh mesh) {
        const size_t capacity = scratch_capacity(mesh);
        if (!capacity)
            return;
        if (mScratchMeshCount < scratchMeshLimit) {
            mScratchMeshes[mScratchMeshCount++] = std::move(mesh);
            return;
        }
        size_t smallest = 0;
        for (size_t i = 1; i < mScratchMeshCount; ++i)
            if (scratch_capacity(mScratchMeshes[i]) < scratch_capacity(mScratchMeshes[smallest]))
                smallest = i;
        if (capacity > scratch_capacity(mScratchMeshes[smallest]))
            mScratchMeshes[smallest] = std::move(mesh);
    }
    void recycle_frame_geometry() {
        // Every entry was submitted by compile_draw on the previous frame.
        // ReleaseGeometry invalidates its GPU handle first; ClearMesh then
        // clears the returned vectors while retaining their allocations.
        // Nothing in mRetained or mPrintedFaces passes through this path.
        for (auto &draw : mGeometry)
            retain_scratch_mesh(draw.geometry.Release(Rml::Geometry::ReleaseMode::ClearMesh));
    }
    Rml::Mesh acquire_scratch_mesh(size_t vertexNeed, size_t indexNeed) {
        if (!mScratchMeshCount)
            return {};
        size_t best = 0, bestCapacity = 0;
        bool foundFit = false;
        for (size_t i = 0; i < mScratchMeshCount; ++i) {
            const auto &candidate = mScratchMeshes[i];
            const size_t capacity = scratch_capacity(candidate);
            const bool fits = candidate.vertices.capacity() >= vertexNeed &&
                              candidate.indices.capacity() >= indexNeed;
            if (fits) {
                if (!foundFit || capacity < bestCapacity) {
                    best = i;
                    bestCapacity = capacity;
                    foundFit = true;
                }
            } else if (!foundFit && capacity > bestCapacity) {
                best = i;
                bestCapacity = capacity;
            }
        }
        Rml::Mesh result = std::move(mScratchMeshes[best]);
        --mScratchMeshCount;
        if (best != mScratchMeshCount)
            mScratchMeshes[best] = std::move(mScratchMeshes[mScratchMeshCount]);
        mScratchMeshes[mScratchMeshCount] = Rml::Mesh{};
        return result;
    }
    void ensure_scratch_batch(Rml::Mesh &batch, size_t vertexNeed, size_t indexNeed) {
        // Retained builds become cache-owned geometry, not per-frame geometry;
        // do not consume the per-frame pool while constructing those caches.
        if (mRecording)
            return;
        if (!batch.vertices.empty() || !batch.indices.empty() || batch.vertices.capacity() ||
            batch.indices.capacity())
            return;
        batch = acquire_scratch_mesh(vertexNeed, indexNeed);
    }
    void compile_draw(Rml::Mesh mesh, Rml::Texture texture,
                      const Rml::CompiledShader *shader = nullptr) {
        auto &draws = mRecording ? *mRecording : mGeometry;
        draws.push_back({GetRenderManager()->MakeGeometry(std::move(mesh)), texture, shader});
        ++mGeometryBuilds;
        render_draw(draws.back());
    }
    template <class Build> void retained(Cache id, bool reusable, size_t revision, Build build) {
        if (mRecording) {
            build();
            return;
        } // The no-motion sky owns its nested panels.
        flush_mask_batch();
        flush_batch();
        auto &cache = mRetained[size_t(id)];
        if (reusable && cache.valid && cache.layout == mLayoutRevision &&
            cache.revision == revision) {
            for (const auto &draw : cache.draws) {
                render_draw(draw);
                ++mGeometryReuses;
            }
            return;
        }
        cache.valid = false;
        if (!reusable) {
            build();
            return;
        }
        cache.draws.clear();
        mRecording = &cache.draws;
        build();
        flush_batch();
        flush_mask_batch();
        mRecording = nullptr;
        cache.layout = mLayoutRevision;
        cache.revision = revision;
        cache.valid = true;
    }
    static float ease(float v) { return v * v * (3 - 2 * v); }
    static float out(float v) { return 1 - std::pow(1 - v, 3.f); }
    float reveal(float delay, float duration) const {
        if (!mState.motion)
            return mPresentation.opening ? 1.f : 0.f;
        return std::clamp(
            float((presentation_progress() * entranceDuration / introOutroTimeScale - delay) /
                  duration),
            0.f, 1.f);
    }
    float host_reveal(float delay, float duration) const {
        return storybook_geometry::host_spring(reveal(delay, duration));
    }
    float row_position(double now) const {
        const float p = std::clamp(float((now - mSelectionStart) / .19), 0.f, 1.f);
        return mSelectionFrom + (mState.selected % 6 - mSelectionFrom) * out(p);
    }
    static Rml::ColourbPremultiplied color(unsigned rgb, float alpha = 1) {
        return Rml::Colourb((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255,
                            int(std::clamp(alpha, 0.f, 1.f) * 255))
            .ToPremultiplied();
    }
    static storybook_art::Band paper_band(int side, float x) {
        float f = std::clamp((x - 20 - side * half) / half, 0.f, 1.f);
        if (side)
            f = 1 - f;
        f *= 255;
        int lo = int(f), hi = std::min(255, lo + 1);
        const float p = f - lo;
        const auto a = storybook_art::profile[lo], b = storybook_art::profile[hi];
        return {a.top + (b.top - a.top) * p, a.bottom + (b.bottom - a.bottom) * p};
    }
    static float paper_y(int side, float x, float y, bool print = true) {
        if (print) {
            const auto band = paper_band(side, x);
            y = bookY + band.top + (y - bookY) * (band.bottom - band.top) / bookH;
        }
        const float u = std::clamp(side ? (x - spine) / half : (spine - x) / half, 0.f, 1.f);
        const float drop = 27 * std::pow(1 - u, 10.f);
        return y + drop * (1 - (y - bookY) / bookH);
    }
    void prepare_mesh() {
        const float angle = mTurn * 3.14159265f, bend = .5f * std::sin(2 * angle);
        mLift[0] = {0, 0};
        for (int i = 1; i <= 64; ++i) {
            const float u = (i - .5f) / 64, a = angle - bend * std::sin(u * 3.14159265f);
            mLift[i] = {mLift[i - 1].x + std::cos(a) * half / 64,
                        mLift[i - 1].z + std::sin(a) * half / 64};
        }
    }
    Rml::Vector2f project(Rml::Vector2f p, bool print) const {
        if (mProportional)
            p = {mShapeOrigin.x + (p.x - mShapeOrigin.x) * mScale / mAxisScale.x,
                 mShapeOrigin.y + (p.y - mShapeOrigin.y) * mScale / mAxisScale.y};
        if (mSide >= 0) {
            p.y = paper_y(mSide, p.x, p.y, print);
            const float u =
                std::clamp(mSide ? (p.x - spine) / half : (spine - p.x) / half, 0.f, 1.f);
            if (mMoving) {
                const float f = u * 64;
                const int a = int(f), b = std::min(64, a + 1);
                const float k = f - a;
                const float x = mLift[a].x + (mLift[b].x - mLift[a].x) * k,
                            z = mLift[a].z + (mLift[b].z - mLift[a].z) * k;
                const float perspective = 4800 / (4800 - z);
                p = {spine + x * perspective, 470 + (p.y - 470) * perspective};
            }
        }
        // Sky positions fill the window; the book and every attached component
        // use the same centered, uniform projection, including during animation.
        if (mLayer == Layer::Day)
            p.x -= mSkyTravel;
        else if (mLayer == Layer::Night)
            p.x += mSkyTravel;
        else if (mLayer == Layer::Book)
            p.y += mBookTranslation;
        else {
            const float spring = mLayer == Layer::Brighton ? mLeftReveal : mRightReveal;
            const Rml::Vector2f anchor{mLayer == Layer::Brighton ? 100.f : 1572.f, 860};
            // Translation and scale rebound together; a linear translation
            // would cancel the spring and stop abruptly at the resting pose.
            p = anchor + (p - anchor) * spring;
            p.y += 70 * (1 - spring);
        }
        return {mLayerOffset.x + p.x * mAxisScale.x, mLayerOffset.y + p.y * mAxisScale.y};
    }
    void flush_batch() {
        if (mBatch.indices.empty())
            return;
        mBatchVertexHint = std::max(mBatchVertexHint, mBatch.vertices.size());
        mBatchIndexHint = std::max(mBatchIndexHint, mBatch.indices.size());
        compile_draw(std::move(mBatch), mBatchTexture);
        mBatch = {};
        mBatchTexture = {};
    }
    void flush_mask_batch() {
        if (mMaskBatch.indices.empty()) {
            mMaskBatch = {};
            mMaskBatchTexture = {};
            mMaskBatchShader = nullptr;
            return;
        }
        mMaskBatchVertexHint = std::max(mMaskBatchVertexHint, mMaskBatch.vertices.size());
        mMaskBatchIndexHint = std::max(mMaskBatchIndexHint, mMaskBatch.indices.size());
        compile_draw(std::move(mMaskBatch), mMaskBatchTexture, mMaskBatchShader);
        mMaskBatch = {};
        mMaskBatchTexture = {};
        mMaskBatchShader = nullptr;
    }
    void begin_untextured_batch() {
        // Direct primitives have the same painter-order boundary as submit:
        // mask work is submitted first, then an untextured batch may merge
        // only with the immediately adjacent untextured batch.
        flush_mask_batch();
        const Rml::Texture texture{};
        if (!(texture == mBatchTexture))
            flush_batch();
        mBatchTexture = texture;
        ensure_scratch_batch(mBatch, mBatchVertexHint, mBatchIndexHint);
    }
    void begin_mask_batch(Rml::Texture texture, const Rml::CompiledShader &shader) {
        const auto *shaderPointer = &shader;
        if (!mMaskBatch.indices.empty() &&
            (!(texture == mMaskBatchTexture) || shaderPointer != mMaskBatchShader))
            flush_mask_batch();
        if (mMaskBatch.indices.empty()) {
            if (!mBatch.indices.empty())
                flush_batch();
            ensure_scratch_batch(mMaskBatch, mMaskBatchVertexHint, mMaskBatchIndexHint);
            mMaskBatchTexture = texture;
            mMaskBatchShader = shaderPointer;
        }
    }
    template <class T> static void reserve_append(std::vector<T> &values, size_t count) {
        const size_t required = values.size() + count;
        if (required > values.capacity()) {
            const size_t grown = values.capacity() ? values.capacity() * 2 : size_t(64);
            values.reserve(std::max(required, grown));
        }
    }
    void submit(Rml::Mesh mesh, Rml::Texture texture = {}, bool print = false,
                const std::vector<int> &boundary = {}, const Rml::CompiledShader &shader = {}) {
        if (!shader)
            flush_mask_batch();
        if (mRecordingPrint)
            mRecordingPrint->push_back(
                {mesh, texture, print, mProportional, mShapeOrigin, boundary});
        // Move the composed layers together, never fade each layer separately:
        // per-mesh alpha would show rays, old ink and page stacks through paper.
        for (auto &v : mesh.vertices)
            v.position = project(v.position, print);
        // Coverage for the outer silhouette is computed after all page warping,
        // perspective and window scaling: it stays exactly one screen pixel.
        const int count = int(boundary.size()), base = int(mesh.vertices.size());
        mesh.vertices.reserve(base + count);
        mesh.indices.reserve(mesh.indices.size() + count * 6);
        for (int i = 0; i < count; ++i) {
            auto v = mesh.vertices[boundary[i]];
            const auto p = v.position,
                       prev = mesh.vertices[boundary[(i + count - 1) % count]].position,
                       next = mesh.vertices[boundary[(i + 1) % count]].position;
            auto normal = [](Rml::Vector2f d) {
                const float length = std::max(.0001f, std::hypot(d.x, d.y));
                return Rml::Vector2f{d.y / length, -d.x / length};
            };
            const auto a = normal(p - prev), b = normal(next - p);
            auto n = a + b;
            n /= std::max(.0001f, std::hypot(n.x, n.y));
            v.position += n * std::min(2.f, 1 / std::max(.5f, n.x * b.x + n.y * b.y));
            v.colour = color(0xffffff, 0);
            mesh.vertices.push_back(v);
        }
        for (int i = 0; i < count; ++i) {
            const int j = (i + 1) % count, a = boundary[i], b = boundary[j];
            mesh.indices.insert(mesh.indices.end(), {a, base + i, base + j, a, base + j, b});
        }
        if (mesh.indices.empty())
            return;
        // Only adjacent draws may merge: never sort by texture, since the page,
        // ink, coverage fringes and moving sheet all rely on painter order.
        // Projection and AA are already baked, so layer changes need no draw.
        // Custom shader draws keep their original coverage/draw boundaries.
        if (shader) {
            flush_batch();
            flush_mask_batch();
            compile_draw(std::move(mesh), texture, &shader);
            return;
        }
        if (!(texture == mBatchTexture))
            flush_batch();
        mBatchTexture = texture;
        if (mBatch.indices.empty()) {
            ensure_scratch_batch(mBatch, mBatchVertexHint, mBatchIndexHint);
            if (mBatch.vertices.empty() && mBatch.indices.empty() &&
                (mBatch.vertices.capacity() || mBatch.indices.capacity())) {
                reserve_append(mBatch.vertices, mesh.vertices.size());
                reserve_append(mBatch.indices, mesh.indices.size());
                mBatch.vertices.insert(mBatch.vertices.end(), mesh.vertices.begin(),
                                       mesh.vertices.end());
                mBatch.indices.insert(mBatch.indices.end(), mesh.indices.begin(),
                                      mesh.indices.end());
            } else
                mBatch = std::move(mesh);
            return;
        }
        const int offset = int(mBatch.vertices.size());
        mBatch.vertices.insert(mBatch.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (int index : mesh.indices)
            mBatch.indices.push_back(offset + index);
    }
    void moving_print(const StorybookState &state, float t) {
        auto &cache = mPrintedFaces[mSide];
        if (cache.valid && cache.layout == mLayoutRevision && cache.state == mStateRevision &&
            cache.photo == mPhotoRevision) {
            const bool proportional = mProportional;
            const auto origin = mShapeOrigin;
            for (const auto &draw : cache.draws) {
                mProportional = draw.proportional;
                mShapeOrigin = draw.origin;
                submit(draw.mesh, draw.texture, draw.print, draw.boundary);
            }
            mProportional = proportional;
            mShapeOrigin = origin;
            return;
        }
        // Retain print-space vertices, not the turning pose or its AA fringe.
        // Curl projection and one-pixel silhouette coverage still run each frame.
        cache.draws.clear();
        mRecordingPrint = &cache.draws;
        art(mSide ? "paper-right" : "paper-left", 20 + mSide * half, bookY, half, bookH, 0, false,
            false);
        page(state, t, false, Print::Ink);
        mRecordingPrint = nullptr;
        cache.layout = mLayoutRevision;
        cache.state = mStateRevision;
        cache.photo = mPhotoRevision;
        cache.valid = true;
    }
    Rml::Texture texture(const Rml::String &name) {
        auto it = mTextures.find(name);
        if (it != mTextures.end())
            return it->second;
        const auto source = name == "mp6scene://live" || name == "mp6tex://wordmark"
                                ? name
                                : resource_path("menu/storybook/" + name + ".png").generic_string();
        return mTextures.emplace(name, GetRenderManager()->LoadTexture(source)).first->second;
    }
    template <class Draw> void proportional(float x, float y, Draw draw) {
        const bool previous = mProportional;
        const auto origin = mShapeOrigin;
        mProportional = true;
        mShapeOrigin = {x, y};
        draw();
        mProportional = previous;
        mShapeOrigin = origin;
    }
    SkyMeshGroup &begin_sky_mesh(size_t index) {
        // Keep rigid scenery geometry alive across rendered frames. Only its
        // small transform/color palette changes; paper deformation still uses
        // the ordinary CPU path. Preserve the existing painter boundaries.
        flush_mask_batch();
        flush_batch();
        if (!mSkyTransformShader)
            mSkyTransformShader = GetRenderManager()->CompileShader("transform-palette", {});
        auto &group = mSkyMeshes[index];
        group.building = group.layout != mLayoutRevision;
        if (group.building) {
            group.batches.clear();
            group.layout = mLayoutRevision;
        }
        group.instances = 0;
        return group;
    }
    template <class Build>
    void sky_instance(SkyMeshGroup &group, float x, float y, float angle, unsigned rgb, float alpha,
                      Build build) {
        constexpr size_t capacity = aurora::rmlui::GeometryTransformPaletteSize;
        const size_t instance = group.instances++, batchIndex = instance / capacity,
                     index = instance % capacity;
        if (group.building) {
            if (batchIndex == group.batches.size())
                group.batches.emplace_back();
            auto &batch = group.batches[batchIndex];
            build(batch.mesh, unsigned(index));
            batch.count = index + 1;
        }
        auto &transform = group.batches[batchIndex].transforms[index];
        const auto ink = color(rgb, alpha);
        if (!ink.alpha) {
            transform = {};
            return;
        }
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const auto position = project({x, y}, false);
        transform.basis = {cosine, sine, -sine, cosine};
        transform.offset = {position.x, position.y, 0, 0};
        transform.color = {ink.red / 255.f, ink.green / 255.f, ink.blue / 255.f, ink.alpha / 255.f};
    }
    void end_sky_mesh(SkyMeshGroup &group) {
        for (auto &batch : group.batches) {
            if (group.building) {
                batch.geometry = GetRenderManager()->MakeGeometry(std::move(batch.mesh));
                ++mGeometryBuilds;
            } else
                ++mGeometryReuses;
            // The backend copies these values into this frame's uniform arena
            // synchronously; neither the worker nor a retained draw borrows it.
            aurora::rmlui::set_geometry_transform_palette({batch.transforms.data(), batch.count});
            batch.geometry.Render(mOffset, {}, mSkyTransformShader);
            ++mRenderBatches;
        }
    }
    int sky_ellipse_count(float rx, float ry) const {
        return std::clamp(
            int(std::ceil(3.14159265f * std::sqrt(2 * std::max(rx * mScale, ry * mScale)))), 8, 48);
    }
    void sky_ellipse(SkyMeshGroup &group, float x, float y, float localX, float localY, float rx,
                     float ry, unsigned rgb, float alpha) {
        sky_instance(group, x, y, 0, rgb, alpha, [&](Rml::Mesh &mesh, unsigned index) {
            storybook_sky_mesh::append_ellipse(mesh, localX * mScale, localY * mScale, rx * mScale,
                                               ry * mScale, sky_ellipse_count(rx, ry), index);
        });
    }
    void sky_star(SkyMeshGroup &group, float x, float y, float size, unsigned rgb, float alpha,
                  float angle) {
        sky_instance(group, x, y, angle, rgb, alpha, [&](Rml::Mesh &mesh, unsigned index) {
            storybook_sky_mesh::append_star(mesh, size * mScale, index);
        });
    }
    void sky_glow(SkyMeshGroup &group, float x, float y, float rx, float ry, unsigned rgb,
                  float alpha) {
        sky_instance(group, x, y, 0, rgb, alpha, [&](Rml::Mesh &mesh, unsigned index) {
            storybook_sky_mesh::append_glow(mesh, rx * mScale, ry * mScale, index);
        });
    }
    template <size_t Count>
    void sky_polygon(SkyMeshGroup &group, float x, float y,
                     const std::array<Rml::Vector2f, Count> &points, unsigned rgb, float alpha) {
        sky_instance(group, x, y, 0, rgb, alpha, [&](Rml::Mesh &mesh, unsigned index) {
            auto physical = points;
            for (auto &p : physical)
                p *= mScale;
            storybook_sky_mesh::append_polygon(mesh, physical.data(), int(Count), index);
        });
    }
    void smooth_polygon(const Rml::Vector2f *source, int count, unsigned rgb, float alpha,
                        bool print = false) {
        if (count < 3)
            return;
        // A one-physical-pixel coverage fringe, independent of render resolution,
        // keeps vector silhouettes smooth even when the game uses no AA.
        if (mRecordingPrint) {
            float area = 0;
            for (int i = 0; i < count; ++i) {
                const auto a = source[i], b = source[(i + 1) % count];
                area += a.x * b.y - b.x * a.y;
            }
            const auto point = [&](int i) { return source[area < 0 ? count - 1 - i : i]; };
            Rml::Mesh mesh;
            Rml::Vector2f center{0, 0};
            for (int i = 0; i < count; ++i)
                center += point(i);
            center /= float(count);
            const auto metric = mProportional ? Rml::Vector2f{mScale, mScale} : mAxisScale;
            const auto solid = color(rgb, alpha), clear = color(rgb, 0);
            mesh.vertices.reserve(1 + 2 * count);
            mesh.indices.reserve(9 * count);
            mesh.vertices.push_back({center, solid, {}});
            for (int i = 0; i < count; ++i) {
                const auto p = point(i), prev = point((i + count - 1) % count),
                           next = point((i + 1) % count);
                auto normal = [&](Rml::Vector2f d) {
                    d = {d.x * metric.x, d.y * metric.y};
                    const float length = std::max(.0001f, std::hypot(d.x, d.y));
                    return Rml::Vector2f{d.y / length, -d.x / length};
                };
                const auto a = normal(p - prev), b = normal(next - p);
                auto n = a + b;
                const float length = std::max(.0001f, std::hypot(n.x, n.y));
                n /= length;
                const float extent = std::min(2.f, 1 / std::max(.5f, n.x * b.x + n.y * b.y));
                mesh.vertices.push_back({p, solid, {}});
                mesh.vertices.push_back(
                    {{p.x + n.x * extent / metric.x, p.y + n.y * extent / metric.y}, clear, {}});
            }
            for (int i = 0; i < count; ++i) {
                const int a = 1 + 2 * i, b = 1 + 2 * ((i + 1) % count);
                mesh.indices.insert(mesh.indices.end(), {0, a, b, a, a + 1, b + 1, a, b + 1, b});
            }
            submit(std::move(mesh), {}, print);
            return;
        }
        float area = 0;
        for (int i = 0; i < count; ++i) {
            const auto a = source[i], b = source[(i + 1) % count];
            area += a.x * b.y - b.x * a.y;
        }
        const auto point = [&](int i) { return source[area < 0 ? count - 1 - i : i]; };
        begin_untextured_batch();
        const int base = int(mBatch.vertices.size());
        Rml::Vector2f center{0, 0};
        for (int i = 0; i < count; ++i)
            center += point(i);
        center /= float(count);
        const auto metric = mProportional ? Rml::Vector2f{mScale, mScale} : mAxisScale;
        const auto solid = color(rgb, alpha), clear = color(rgb, 0);
        reserve_append(mBatch.vertices, 1 + 2 * count);
        reserve_append(mBatch.indices, 9 * count);
        mBatch.vertices.push_back({project(center, print), solid, {}});
        auto normal = [&](Rml::Vector2f d) {
            d = {d.x * metric.x, d.y * metric.y};
            const float length = std::max(.0001f, std::hypot(d.x, d.y));
            return Rml::Vector2f{d.y / length, -d.x / length};
        };
        // Adjacent corners share the same directed edge, including its exact
        // floating-point inputs. Normalize it once, retaining the original
        // per-corner AA math and polygon winding.
        const auto closingNormal = normal(point(0) - point(count - 1));
        auto a = closingNormal;
        for (int i = 0; i < count; ++i) {
            const auto p = point(i), next = point((i + 1) % count);
            const auto b = i + 1 == count ? closingNormal : normal(next - p);
            auto n = a + b;
            const float length = std::max(.0001f, std::hypot(n.x, n.y));
            n /= length;
            const float extent = std::min(2.f, 1 / std::max(.5f, n.x * b.x + n.y * b.y));
            mBatch.vertices.push_back({project(p, print), solid, {}});
            mBatch.vertices.push_back(
                {project({p.x + n.x * extent / metric.x, p.y + n.y * extent / metric.y}, print),
                 clear,
                 {}});
            a = b;
        }
        for (int i = 0; i < count; ++i) {
            const int a = 1 + 2 * i, b = 1 + 2 * ((i + 1) % count);
            mBatch.indices.insert(mBatch.indices.end(),
                                  {base, base + a, base + b, base + a, base + a + 1, base + b + 1,
                                   base + a, base + b + 1, base + b});
        }
    }
    void smooth_polygon(const std::vector<Rml::Vector2f> &points, unsigned rgb, float alpha,
                        bool print = false) {
        smooth_polygon(points.data(), int(points.size()), rgb, alpha, print);
    }
    void quad(float x, float y, float w, float h, unsigned rgb, float alpha = 1,
              bool print = false) {
        if (mProportional) {
            const Rml::Vector2f points[] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
            smooth_polygon(points, 4, rgb, alpha, print);
            return;
        }
        Rml::Mesh mesh;
        const auto c = color(rgb, alpha);
        mesh.vertices = {
            {{x, y}, c, {}}, {{x + w, y}, c, {}}, {{x + w, y + h}, c, {}}, {{x, y + h}, c, {}}};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        submit(std::move(mesh), {}, print);
    }
    void strip(float x, float y, float w, float h, Rml::Texture tex, float u0, float v0, float u1,
               float v1, float angle = 0, bool print = true, unsigned tint = 0xffffff,
               float alpha = 1) {
        const int slices = mSide < 0 ? 1 : std::max(2, int(w / 12));
        const auto original = [&] {
            Rml::Mesh mesh;
            const auto c = color(tint, alpha);
            mesh.vertices.reserve(2 * (slices + 1));
            mesh.indices.reserve(6 * slices);
            const float cosine = std::cos(angle), sine = std::sin(angle);
            for (int i = 0; i <= slices; ++i) {
                const float u = float(i) / slices;
                for (int j = 0; j < 2; ++j) {
                    float px = x + w * u, py = y + h * j;
                    if (angle != 0) {
                        const float dx = (px - x - w / 2) * mAxisScale.x,
                                    dy = (py - y - h / 2) * mAxisScale.y;
                        px = x + w / 2 + (cosine * dx - sine * dy) / mAxisScale.x;
                        py = y + h / 2 + (sine * dx + cosine * dy) / mAxisScale.y;
                    }
                    mesh.vertices.push_back({{px, py}, c, {u0 + (u1 - u0) * u, j ? v1 : v0}});
                }
                if (i) {
                    const int k = i * 2;
                    mesh.indices.insert(mesh.indices.end(), {k - 2, k, k + 1, k - 2, k + 1, k - 1});
                }
            }
            std::vector<int> boundary;
            boundary.reserve(2 * (slices + 1));
            for (int i = 0; i <= slices; ++i)
                boundary.push_back(i * 2);
            for (int i = slices; i >= 0; --i)
                boundary.push_back(i * 2 + 1);
            submit(std::move(mesh), tex, print, boundary);
        };
        // Retained print faces need the original local mesh and boundary list;
        // their replay stores unprojected vertices plus projection metadata.
        if (mRecordingPrint)
            return original();

        // Animated strips normally have <=68 slices. Keep all source and
        // projected boundary state on the stack, with the heap-backed builder
        // above as a bounded-work fallback for unusual widths.
        constexpr int maxDirectSlices = 128;
        if (slices > maxDirectSlices)
            return original();
        constexpr size_t maxDirectVertices = size_t(2 * (maxDirectSlices + 1));
        auto append_direct = [&]<size_t ScratchVertices>() {
            std::array<Rml::Vector2f, ScratchVertices> projected{};
            std::array<Rml::Vector2f, ScratchVertices> texCoords{};
            std::array<int, ScratchVertices> boundary{};
            const int vertexCount = 2 * (slices + 1), boundaryCount = vertexCount;
            const auto c = color(tint, alpha);
            flush_mask_batch();
            if (!(tex == mBatchTexture))
                flush_batch();
            mBatchTexture = tex;
            if (mBatch.indices.empty())
                ensure_scratch_batch(mBatch, mBatchVertexHint, mBatchIndexHint);
            const int base = int(mBatch.vertices.size());
            const float cosine = std::cos(angle), sine = std::sin(angle);
            for (int i = 0; i <= slices; ++i) {
                const float u = float(i) / slices;
                for (int j = 0; j < 2; ++j) {
                    float px = x + w * u, py = y + h * j;
                    if (angle != 0) {
                        const float dx = (px - x - w / 2) * mAxisScale.x,
                                    dy = (py - y - h / 2) * mAxisScale.y;
                        px = x + w / 2 + (cosine * dx - sine * dy) / mAxisScale.x;
                        py = y + h / 2 + (sine * dx + cosine * dy) / mAxisScale.y;
                    }
                    const int index = 2 * i + j;
                    projected[index] = project({px, py}, print);
                    texCoords[index] = {u0 + (u1 - u0) * u, j ? v1 : v0};
                }
            }
            int boundaryIndex = 0;
            for (int i = 0; i <= slices; ++i)
                boundary[boundaryIndex++] = i * 2;
            for (int i = slices; i >= 0; --i)
                boundary[boundaryIndex++] = i * 2 + 1;
            reserve_append(mBatch.vertices, size_t(vertexCount + boundaryCount));
            reserve_append(mBatch.indices, size_t(6 * slices + boundaryCount * 6));
            for (int i = 0; i < vertexCount; ++i)
                mBatch.vertices.push_back({projected[i], c, texCoords[i]});
            for (int i = 1; i <= slices; ++i) {
                const int k = 2 * i;
                mBatch.indices.insert(mBatch.indices.end(),
                                      {base + k - 2, base + k, base + k + 1, base + k - 2,
                                       base + k + 1, base + k - 1});
            }
            auto normal = [](Rml::Vector2f d) {
                const float length = std::max(.0001f, std::hypot(d.x, d.y));
                return Rml::Vector2f{d.y / length, -d.x / length};
            };
            for (int i = 0; i < boundaryCount; ++i) {
                const int local = boundary[i],
                          prev = boundary[(i + boundaryCount - 1) % boundaryCount],
                          next = boundary[(i + 1) % boundaryCount];
                const auto p = projected[local], a = normal(p - projected[prev]),
                           b = normal(projected[next] - p);
                auto n = a + b;
                n /= std::max(.0001f, std::hypot(n.x, n.y));
                const float extent = std::min(2.f, 1 / std::max(.5f, n.x * b.x + n.y * b.y));
                mBatch.vertices.push_back({p + n * extent, color(0xffffff, 0), texCoords[local]});
            }
            for (int i = 0; i < boundaryCount; ++i) {
                const int j = (i + 1) % boundaryCount, a = boundary[i], b = boundary[j];
                mBatch.indices.insert(mBatch.indices.end(),
                                      {base + a, base + vertexCount + i, base + vertexCount + j,
                                       base + a, base + vertexCount + j, base + b});
            }
        };
        if (slices == 1)
            append_direct.template operator()<4>();
        else
            append_direct.template operator()<maxDirectVertices>();
    }
    void art(const char *id, float x, float y, float w, float h, float angle = 0, bool flip = false,
             bool print = true, unsigned tint = 0xffffff, float alpha = 1) {
        const Rml::String name = id;
        if (name.find("book-") != 0 && name.find("paper-") != 0 && name != "selection-ribbon" &&
            name != "game-preview") {
            const float nw = w * mScale / mAxisScale.x, nh = h * mScale / mAxisScale.y;
            x += (w - nw) / 2;
            y += (h - nh) / 2;
            w = nw;
            h = nh;
        }
        for (const auto &a : storybook_art::sprites)
            if (Rml::String(id) == a.name) {
                const auto uv = storybook_geometry::sprite_uv;
                strip(x, y, w, h, texture(id), uv(a.x + (flip ? a.w : 0), a.width),
                      uv(a.y, a.height), uv(a.x + (flip ? 0 : a.w), a.width),
                      uv(a.y + a.h, a.height), angle, print, tint, alpha);
                return;
            }
    }
    void gradient(float x, float y, float w, float h, unsigned top, unsigned bottom) {
        Rml::Mesh mesh;
        const auto a = color(top), b = color(bottom);
        mesh.vertices = {
            {{x, y}, a, {}}, {{x + w, y}, a, {}}, {{x + w, y + h}, b, {}}, {{x, y + h}, b, {}}};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        submit(std::move(mesh));
    }
    void rounded(float x, float y, float w, float h, float radius, unsigned top, unsigned bottom,
                 float alpha = 1) {
        Rml::Mesh mesh;
        const auto a = color(top, alpha), b = color(bottom, alpha);
        mesh.vertices.reserve(37);
        mesh.indices.reserve(108);
        auto shade = [&](float py) {
            const float f = std::clamp((py - y) / h, 0.f, 1.f);
            return Rml::ColourbPremultiplied((unsigned char)(a.red + (b.red - a.red) * f),
                                             (unsigned char)(a.green + (b.green - a.green) * f),
                                             (unsigned char)(a.blue + (b.blue - a.blue) * f),
                                             a.alpha);
        };
        mesh.vertices.push_back({{x + w / 2, y + h / 2}, shade(y + h / 2), {}});
        for (int corner = 0; corner < 4; ++corner)
            for (int j = 0; j <= 8; ++j) {
                const float angle = (-1 + corner) * 1.5707963f + j * 1.5707963f / 8;
                const float cx = corner < 2 ? x + w - radius : x + radius,
                            cy = corner == 0 || corner == 3 ? y + radius : y + h - radius;
                const float px = cx + std::cos(angle) * radius, py = cy + std::sin(angle) * radius;
                mesh.vertices.push_back({{px, py}, shade(py), {}});
            }
        for (int i = 1; i <= 36; ++i)
            mesh.indices.insert(mesh.indices.end(), {0, i, i == 36 ? 1 : i + 1});
        submit(std::move(mesh));
    }
    void glow(float x, float y, float rx, float ry, unsigned rgb, float alpha, bool print = false) {
        if (mRecordingPrint) {
            Rml::Mesh mesh;
            mesh.vertices.reserve(50);
            mesh.indices.reserve(144);
            mesh.vertices.push_back({{x, y}, color(rgb, alpha), {}});
            const auto &ring = storybook_geometry::ellipse_ring(48);
            for (int i = 0; i <= 48; ++i) {
                const auto p = storybook_geometry::ellipse_point(ring, 48, i, x, y, rx, ry);
                mesh.vertices.push_back({{p.x, p.y}, color(rgb, 0), {}});
                if (i)
                    mesh.indices.insert(mesh.indices.end(), {0, i, i + 1});
            }
            submit(std::move(mesh), {}, print);
            return;
        }
        begin_untextured_batch();
        const int base = int(mBatch.vertices.size());
        reserve_append(mBatch.vertices, 50);
        reserve_append(mBatch.indices, 144);
        mBatch.vertices.push_back({project({x, y}, print), color(rgb, alpha), {}});
        const auto &ring = storybook_geometry::ellipse_ring(48);
        for (int i = 0; i <= 48; ++i) {
            const auto p = storybook_geometry::ellipse_point(ring, 48, i, x, y, rx, ry);
            mBatch.vertices.push_back({project({p.x, p.y}, print), color(rgb, 0), {}});
            if (i)
                mBatch.indices.insert(mBatch.indices.end(), {base, base + i, base + i + 1});
        }
    }
    void line(float x, float y, float ex, float ey, float thickness, unsigned rgb, float alpha = 1,
              bool print = true) {
        const float len = std::max(.001f, std::hypot(ex - x, ey - y)),
                    dx = -(ey - y) * thickness / len / 2, dy = (ex - x) * thickness / len / 2;
        const Rml::Vector2f points[] = {
            {x - dx, y - dy}, {ex - dx, ey - dy}, {ex + dx, ey + dy}, {x + dx, y + dy}};
        smooth_polygon(points, 4, rgb, alpha, print);
    }
    void ellipse(float x, float y, float rx, float ry, unsigned rgb, float alpha = 1,
                 bool print = false) {
        // Sub-pixel stars need a small coverage disk, not balloon-sized meshes.
        // Bound screen-space chord error without exhausting Aurora's frame arena.
        const auto metric = mProportional ? Rml::Vector2f{mScale, mScale} : mAxisScale;
        const int count = std::clamp(
            int(std::ceil(3.14159265f * std::sqrt(2 * std::max(rx * metric.x, ry * metric.y)))), 8,
            48);
        std::array<Rml::Vector2f, 48> points;
        const auto &ring = storybook_geometry::ellipse_ring(count);
        for (int i = 0; i < count; ++i) {
            const auto p = storybook_geometry::ellipse_point(ring, count, i, x, y, rx, ry);
            points[size_t(i)] = {p.x, p.y};
        }
        smooth_polygon(points.data(), count, rgb, alpha, print);
    }
    void star(float x, float y, float size, unsigned rgb, float alpha = 1, float angle = 0) {
        std::array<Rml::Vector2f, 10> points;
        for (int i = 0; i < 10; ++i) {
            float a = angle - 1.5707963f + i * .62831853f, r = size * (i % 2 ? .22f : .5f);
            points[size_t(i)] = {x + std::cos(a) * r, y + std::sin(a) * r};
        }
        smooth_polygon(points.data(), 10, rgb, alpha);
    }
    void clear_text_cache() {
        mText.clear();
        mFontVersions.clear();
        mRetryText = false;
    }
    TextRun &run(const Rml::String &value, float size) {
        const int px = std::max(8, int(std::round(size * mScale)));
        const auto key = std::to_string(px) + ":" + value;
        auto &r = mText[key];
        auto *engine = Rml::GetFontEngineInterface();
        // FontEngineInterface accepts the normalized family, unlike RCSS's
        // case-insensitive parser. A missing face must never reach GetVersion.
        if (!r.face)
            r.face = engine->GetFontFaceHandle("titan one", Rml::Style::FontStyle::Normal,
                                               Rml::Style::FontWeight::Normal, px);
        if (!r.face) {
            mRetryText = true;
            return r;
        }
        const int version = engine->GetVersion(r.face);
        if (r.meshes.empty() || r.version != version) {
            r.meshes.clear();
            static const Rml::String language = "en";
            ++mTextGenerations;
            r.width = float(engine->GenerateString(*GetRenderManager(), r.face, 0, value, {0, 0},
                                                   color(0xffffff), 1, {language}, r.meshes)) /
                      mScale;
            r.version = engine->GetVersion(r.face);
        }
        // Check each face once per frame, not once per cached string. Keep a
        // mixed-version cache dirty even if an engine later rolls its version
        // back: retained ink may still reference either atlas generation.
        const auto [faceVersion, inserted] = mFontVersions.try_emplace(r.face, r.version);
        if (!inserted && faceVersion->second != r.version)
            mRetryText = true;
        return r;
    }
    struct PlacedText {
        TextRun *run;
        float left;
        Rml::Vector2f scale;
    };
    PlacedText place_text(const Rml::String &value, float x, float size, int center, float maxWidth,
                          float pop = 1) {
        auto &r = run(value, size);
        const float sx = mScale / mAxisScale.x;
        const float fit = std::min(1.f, maxWidth / std::max(1.f, r.width * sx)) * pop;
        return {&r,
                x - (center ? r.width * fit * sx / 2 : 0),
                {fit / mAxisScale.x, fit / mAxisScale.y}};
    }
    void text(const Rml::String &value, float x, float y, float size, unsigned rgb = 0,
              int center = 0, float maxWidth = 10000, float alpha = 1, float pop = 1) {
        if (value.empty())
            return;
        const auto placed = place_text(value, x, size, center, maxWidth, pop);
        for (const auto &item : placed.run->meshes) {
            auto mesh = item.mesh;
            for (auto &v : mesh.vertices) {
                v.position = {placed.left + v.position.x * placed.scale.x,
                              y + v.position.y * placed.scale.y};
                v.colour = color(rgb, alpha);
            }
            submit(std::move(mesh), item.texture, mSide >= 0);
        }
    }
    void binding() {
        // Solid ivory fold behind both leaves, including the transparent trim at
        // the hinge. The lower red saddle joins the hardcover rather than leaving
        // the day/night zigzag visible through the middle of the book.
        quad(spine - 12, bookY + 27, 24, bookH - 27, 0xe8dcb9);
        Rml::Mesh mesh;
        for (int i = 0; i <= 32; ++i) {
            const float u = float(i) / 32, x = spine - 58 + 116 * u,
                        bump = std::pow(std::sin(u * 3.14159265f), 3.f);
            mesh.vertices.push_back({{x, bookY + bookH - 10 + 7 * bump}, color(0xf52b0c), {}});
            mesh.vertices.push_back({{x, bookY + bookH - 2 + 11 * bump}, color(0x821005), {}});
            if (i) {
                int k = i * 2;
                mesh.indices.insert(mesh.indices.end(), {k - 2, k, k + 1, k - 2, k + 1, k - 1});
            }
        }
        submit(std::move(mesh));
    }
    void crease() {
        const float bottom = bookY + bookH * (975.f - 4) / 1016;
        // Center crease belongs to the stationary binding, never to a turning face.
        for (int i = 0; i < 6; ++i) {
            const float shade = std::abs(i - 2.5f) / 2.5f;
            const unsigned r = unsigned(232 + 6 * shade), g = unsigned(220 + 10 * shade),
                           b = unsigned(185 + 22 * shade);
            quad(spine - 3 + i, 119, 1, bottom - 119, (r << 16) | (g << 8) | b);
        }
    }
    void arrow(float x, float y, int dir, float alpha = 1, unsigned tint = 0xffbc16) {
        // One joined chevron, not two butt-ended strokes with a notch at the tip.
        proportional(x, y, [&] {
            Rml::Mesh mesh;
            const auto c = color(tint, alpha);
            for (const auto p : std::array<Rml::Vector2f, 6>{
                     {{-8, -10}, {-3, -14}, {9, 0}, {-3, 14}, {-8, 10}, {1, 0}}})
                mesh.vertices.push_back({{x + p.x * dir, y + p.y}, c, {}});
            mesh.indices = {0, 1, 5, 1, 2, 5, 2, 3, 5, 3, 4, 5};
            std::vector<int> boundary{0, 1, 2, 3, 4, 5};
            if (dir < 0) {
                std::reverse(boundary.begin(), boundary.end());
                for (size_t i = 0; i < mesh.indices.size(); i += 3)
                    std::swap(mesh.indices[i], mesh.indices[i + 1]);
            }
            submit(std::move(mesh), {}, true, boundary);
        });
    }
    void scenery(float t) {
        const auto mask = texture("zigzag-mask");
        if (!mMaskShaders[0])
            for (int i = 0; i < 2; ++i) {
                Rml::Dictionary parameters;
                parameters["inverted"] = i == 0;
                mMaskShaders[i] = GetRenderManager()->CompileShader("luminance-mask", parameters);
            }
        if (mMaskLayout != mLayoutRevision) {
            mMaskTiles = storybook_geometry::sky_mask_tiles(width * mAxisScale.x,
                                                            height * mAxisScale.y, mScale);
            mMaskLayout = mLayoutRevision;
        }
        const auto maskDimensions = mask.GetDimensions();
        // Split at repeat boundaries before assigning UVs. The ray mask and the
        // sky panels share these tiles, so both retain identical teeth in motion.
        auto masked = [&](const storybook_geometry::Point *polygon, int polygonCount,
                          Rml::Texture mask, auto shade) {
            const auto &shader = mMaskShaders[mLayer == Layer::Day ? 0 : 1];
            const auto append = [&](const storybook_geometry::Point *localPoints, int localCount,
                                    const storybook_geometry::MaskTile &tile) {
                // A clipped polygon contributes no batch state until it can
                // produce a real triangle. This preserves the old draw
                // boundary while avoiding a temporary mesh for each ray.
                if (localCount < 3)
                    return;
                begin_mask_batch(mask, shader);
                if (mMaskBatch.indices.empty()) {
                    mMaskBatch.vertices.reserve(mMaskTiles.size() * 8);
                    mMaskBatch.indices.reserve(mMaskTiles.size() * 18);
                }
                const int base = int(mMaskBatch.vertices.size());
                for (int i = 0; i < localCount; ++i) {
                    auto p = localPoints[i];
                    p.x += tile.panel.x;
                    p.y += tile.panel.y;
                    const auto colour = shade(p.y);
                    const auto uv = storybook_geometry::tile_mask_uv(
                        p, tile, float(maskDimensions.x), float(maskDimensions.y));
                    mMaskBatch.vertices.push_back(
                        {project({p.x, p.y}, false), colour, {uv.x, uv.y}});
                }
                for (int i = 1; i + 1 < localCount; ++i)
                    mMaskBatch.indices.insert(mMaskBatch.indices.end(),
                                              {base, base + i, base + i + 1});
            };
            for (const auto &tile : mMaskTiles) {
                // The current zigzag asset has solid side-margin columns. A
                // zero-width UV at the opposite luminance is guaranteed to
                // produce zero mask coverage (texel-center clamp is retained),
                // so avoid clipping/uploading those invisible triangles.
                const bool oppositeSolidMargin =
                    tile.uv.w == 0.f && ((mLayer == Layer::Day && tile.uv.x == 1.f) ||
                                         (mLayer == Layer::Night && tile.uv.x == 0.f));
                if (oppositeSolidMargin)
                    continue;
                int localCount = 0;
                std::array<storybook_geometry::Point, 12> clipped{};
                const storybook_geometry::Point *localPoints = nullptr;
                if (polygonCount <= 8) {
                    std::array<storybook_geometry::Point, 8> source{};
                    for (int i = 0; i < polygonCount; ++i) {
                        source[size_t(i)].x = polygon[i].x - tile.panel.x;
                        source[size_t(i)].y = polygon[i].y - tile.panel.y;
                    }
                    const auto fixed = storybook_geometry::clip_to_panel_fixed(
                        source.data(), polygonCount, tile.panel.w, tile.panel.h);
                    localCount = fixed.size;
                    for (int i = 0; i < localCount; ++i)
                        clipped[size_t(i)] = fixed.points[size_t(i)];
                    localPoints = clipped.data();
                } else {
                    std::vector<storybook_geometry::Point> local(polygon, polygon + polygonCount);
                    for (auto &p : local) {
                        p.x -= tile.panel.x;
                        p.y -= tile.panel.y;
                    }
                    local = storybook_geometry::clip_to_panel(std::move(local), tile.panel.w,
                                                              tile.panel.h);
                    localCount = int(local.size());
                    localPoints = local.data();
                    append(localPoints, localCount, tile);
                    continue; // Consume the fallback while its vector storage is alive.
                }
                append(localPoints, localCount, tile);
            }
        };
        auto skyPanel = [&](Rml::Texture mask, unsigned top, unsigned middle, unsigned bottom) {
            const auto shade = [&](float y) {
                float v = y / height;
                auto a = color(v < .48f ? top : middle), b = color(v < .48f ? middle : bottom);
                float f = v < .48f ? v / .48f : (v - .48f) / .52f;
                return Rml::ColourbPremultiplied((unsigned char)(a.red + (b.red - a.red) * f),
                                                 (unsigned char)(a.green + (b.green - a.green) * f),
                                                 (unsigned char)(a.blue + (b.blue - a.blue) * f),
                                                 255);
            };
            const storybook_geometry::Point topHalf[] = {
                {0, 0}, {width, 0}, {width, height * .48f}, {0, height * .48f}};
            const storybook_geometry::Point bottomHalf[] = {
                {0, height * .48f}, {width, height * .48f}, {width, height}, {0, height}};
            masked(topHalf, 4, mask, shade);
            masked(bottomHalf, 4, mask, shade);
        };
        mLayer = Layer::Day;
        retained(Cache::DayPanel, mSkyReveal == 1, 0,
                 [&] { skyPanel(mask, 0xfff48a, 0xf8cc35, 0xd79016); });
        for (int i = 0; i < 14; ++i) {
            const float a = i * 3.14159265f / 7 + t * .11f;
            const auto ray =
                storybook_geometry::sky_ray_fixed(a, width, height, mAxisScale.x, mAxisScale.y);
            masked(ray.points.data(), ray.size, mask, [&](float) { return color(0xfff98d); });
        }
        mLayer = Layer::Night;
        retained(Cache::NightPanel, mSkyReveal == 1, 0,
                 [&] { skyPanel(mask, 0x343ab2, 0x1c175f, 0x0b062d); });
        flush_mask_batch();
        struct SkyDot {
            float x, y, r, phase;
        };
        static const auto dots = [] {
            std::array<SkyDot, 240> result{};
            unsigned seed = 606;
            auto random = [&]() {
                seed = seed * 1664525u + 1013904223u;
                return float(seed) / 4294967296.f;
            };
            for (auto &dot : result)
                dot = {width * .5f + random() * width * .5f, random() * height,
                       .55f + random() * 1.1f, random() * 6.28f};
            return result;
        }();
        const bool rigidSky = mState.motion;
        SkyMeshGroup *dotMesh = rigidSky ? &begin_sky_mesh(0) : nullptr;
        for (const auto &dot : dots) {
            float x = dot.x, y = dot.y, r = dot.r, phase = dot.phase;
            if (x < 860)
                continue;
            const float glow = .5f + .5f * std::sin(t * (1.3f + r * .3f) + phase);
            x += std::sin(t * .24f + phase) * 2;
            y += std::cos(t * .19f + phase) * 1.7f;
            const bool hidden = sky_hidden(x, y, r * 3.4f);
            if (dotMesh) {
                const float visible = hidden ? 0.f : 1.f;
                if (r > 1.1f) {
                    sky_ellipse(*dotMesh, x, y, 0, 0, r * 3, r * 3, 0xadcbff,
                                .07f * glow * visible);
                    sky_star(*dotMesh, x, y, r * 3.3f, 0xe3efff, (.14f + .8f * glow) * visible,
                             std::sin(t * .22f + phase) * .25f);
                } else
                    sky_ellipse(*dotMesh, x, y, 0, 0, r, r, 0xe3efff,
                                (.14f + .8f * glow) * visible);
                continue;
            }
            if (hidden)
                continue;
            proportional(x, y, [&] {
                if (r > 1.1f) {
                    ellipse(x, y, r * 3, r * 3, 0xadcbff, .07f * glow);
                    star(x, y, r * 3.3f, 0xe3efff, .14f + .8f * glow,
                         std::sin(t * .22f + phase) * .25f);
                } else
                    ellipse(x, y, r, r, 0xe3efff, .14f + .8f * glow);
            });
        }
        // A complete night sky, not a decorative frame around the closed book.
        // Interior stars stay behind the paper and are revealed as it slides away.
        const float sky[][3] = {
            {1267, 15, 22},  {1454, 29, 42},  {1504, 64, 29},  {1649, 114, 25}, {1654, 176, 45},
            {1660, 252, 19}, {1653, 319, 34}, {1665, 475, 17}, {1380, 897, 36}, {1540, 920, 35},
            {1647, 914, 20}, {940, 155, 28},  {1130, 230, 36}, {1375, 180, 24}, {1550, 340, 31},
            {995, 380, 22},  {1245, 420, 40}, {1440, 520, 26}, {910, 595, 34},  {1135, 645, 25},
            {1570, 720, 37}, {1340, 755, 30}, {975, 830, 23},
        };
        // These adjacent untextured groups share a shader. Keep their original
        // painter order in one palette stream instead of flushing partial
        // palettes between dots, stars and balloons. Each instance already
        // snapshots the correct day/night position before mLayer changes.
        SkyMeshGroup *starMesh = dotMesh;
        for (size_t i = 0; i < std::size(sky); ++i) {
            const float phase = float(i) * 1.73f, x = sky[i][0] + std::sin(t * .27f + phase) * 3.5f,
                        y = sky[i][1] + std::cos(t * .21f + phase) * 3;
            const float brightness = .5f + .5f * std::sin(t * (1.4f + i * .035f) + i),
                        angle = std::sin(t * .24f + phase) * .18f;
            const bool hidden = sky_hidden(x, y, sky[i][2] * .81f);
            if (starMesh) {
                const float visible = hidden ? 0.f : 1.f;
                sky_glow(*starMesh, x, y, sky[i][2] * .8f, sky[i][2] * .8f, 0xdbe7ff,
                         .36f * brightness * visible);
                sky_star(*starMesh, x, y, sky[i][2], i % 3 ? 0xfff83b : 0xf6fcff,
                         (.48f + .52f * brightness) * visible, angle);
                continue;
            }
            if (hidden)
                continue;
            proportional(x, y, [&] {
                glow(x, y, sky[i][2] * .8f, sky[i][2] * .8f, 0xdbe7ff, .36f * brightness);
                star(x, y, sky[i][2], i % 3 ? 0xfff83b : 0xf6fcff, .48f + .52f * brightness, angle);
            });
        }
        mLayer = Layer::Day;
        // Scatter balloons across the entire day half, including the normally
        // occluded interior. Keep each one's proportional silhouette and motion.
        const float balloons[][5] = {
            {39, 260, 44, .3f, 13},     {34, 480, 30, 2, 9},        {375, 898, 29, 4, 11},
            {278, 38, 28, 1, 8},        {165, 210, 36, 5, 10},      {410, 220, 46, 1.7f, 12},
            {685, 195, 32, 3.1f, 8.5f}, {265, 430, 29, 4.4f, 9.5f}, {575, 410, 40, 2.8f, 11},
            {145, 665, 35, .9f, 12.5f}, {420, 685, 30, 5.5f, 8.5f}, {715, 740, 44, 3.8f, 10.5f},
        };
        const unsigned balloonColors[] = {0xf35e7d, 0x28cbbd, 0xf07629, 0x76acde};
        SkyMeshGroup *balloonMesh = dotMesh;
        for (size_t i = 0; i < std::size(balloons); ++i) {
            const auto &b = balloons[i];
            float x = b[0] + std::sin(t * .28f + b[3]) * 7,
                  y = height + 80 - std::fmod(height + 80 - b[1] + t * b[4], height + 160),
                  w = b[2], h = w * 1.3f;
            const bool hidden = sky_hidden(x, y, h);
            if (balloonMesh) {
                const float visible = hidden ? 0.f : 1.f;
                sky_ellipse(*balloonMesh, x, y, 0, -h * .14f, w * .48f, h * .40f, 0xfff3c8,
                            visible);
                sky_ellipse(*balloonMesh, x, y, 0, -h * .15f, w * .44f, h * .38f,
                            balloonColors[i % std::size(balloonColors)], visible);
                sky_ellipse(*balloonMesh, x, y, 0, -h * .16f, w * .14f, h * .37f, 0xfff3b6,
                            visible);
                // The two ropes and basket are part of the same rigid balloon.
                for (const float side : {-1.f, 1.f}) {
                    const float sx = side * w * .14f, sy = h * .2f, ex = side * w * .1f,
                                ey = h * .49f;
                    const float length = std::max(.001f, std::hypot(ex - sx, ey - sy)),
                                dx = -(ey - sy) * 1.2f / length / 2,
                                dy = (ex - sx) * 1.2f / length / 2;
                    sky_polygon(*balloonMesh, x, y,
                                std::array<Rml::Vector2f, 4>{{{sx - dx, sy - dy},
                                                              {ex - dx, ey - dy},
                                                              {ex + dx, ey + dy},
                                                              {sx + dx, sy + dy}}},
                                0x9b6729, visible);
                }
                sky_polygon(*balloonMesh, x, y,
                            std::array<Rml::Vector2f, 4>{{{-w * .13f, h * .46f},
                                                          {w * .13f, h * .46f},
                                                          {w * .13f, h * .61f},
                                                          {-w * .13f, h * .61f}}},
                            0xb47738, visible);
                continue;
            }
            if (hidden)
                continue;
            proportional(x, y, [&] {
                ellipse(x, y - h * .14f, w * .48f, h * .40f, 0xfff3c8);
                ellipse(x, y - h * .15f, w * .44f, h * .38f,
                        balloonColors[i % std::size(balloonColors)]);
                ellipse(x, y - h * .16f, w * .14f, h * .37f, 0xfff3b6);
                line(x - w * .14f, y + h * .2f, x - w * .1f, y + h * .49f, 1.2f, 0x9b6729);
                line(x + w * .14f, y + h * .2f, x + w * .1f, y + h * .49f, 1.2f, 0x9b6729);
                quad(x - w * .13f, y + h * .46f, w * .26f, h * .15f, 0xb47738);
            });
        }
        if (balloonMesh)
            end_sky_mesh(*balloonMesh);
        // Deterministic particle births: the initial scattered set expires; later
        // pieces enter above and leave below, never teleporting across the scene.
        const unsigned confettiColors[] = {0x00c8ed, 0xf33bce, 0x18d442, 0x7734ef};
        for (int id = storybook_geometry::first_confetti_birth(t); id <= int(t * 2.2f); ++id) {
            unsigned s = unsigned(id + 57) * 2654435761u;
            auto rnd = [&]() {
                s = s * 1664525u + 1013904223u;
                return float(s) / 4294967296.f;
            };
            float size = 20 + rnd() * 17, x = rnd() < .42f ? 15 + rnd() * 80 : 100 + rnd() * 670,
                  y = id < 0 ? rnd() * height : -size - 25, born = id < 0 ? 0 : id / 2.2f,
                  speed = 70 + rnd() * 45, phase = rnd() * 6.28f;
            float age = t - born;
            y += speed * age;
            if (age < 0 || y - size > height)
                continue;
            float fold = .25f + .75f * std::abs(std::cos(age * 2 + phase));
            if (sky_hidden(x, y, size))
                continue;
            std::array<Rml::Vector2f, 4> points;
            const float a = phase + age * .4f;
            const float cosine = std::cos(a), sine = std::sin(a);
            for (int j = 0; j < 4; ++j) {
                float dx = ((j == 1 || j == 2) ? .38f : -.38f) * size * fold,
                      dy = (j < 2 ? -.5f : .5f) * size;
                const auto p = storybook_geometry::rotate_offset({x, y}, {dx, dy}, cosine, sine);
                points[size_t(j)] = {p.x, p.y};
            }
            proportional(x, y, [&] {
                smooth_polygon(points.data(), 4, confettiColors[unsigned(id + 24) % 4], 1);
            });
        }
        art("sky-sun", 96, 35, 158, 158, t * .16f);
        mLayer = Layer::Night;
        art("sky-moon", 1538, 9 + std::sin(t * .9f) * 6, 100, 108, std::sin(t * .65f) * .09f);
        flush_mask_batch();
    }
    bool sky_hidden(float x, float y, float radius) const {
        // Only interiors of the two opaque, stationary paper leaves qualify.
        // Never cull during a book entrance/exit: the fully populated sky must
        // be revealed behind it. Bounds include the entire glow and AA fringe.
        if (mBookReveal != 1)
            return false;
        const auto p = project({x, y}, false);
        const float r = radius * mScale + 3;
        const float top = mBookOffset.y + 160 * mScale, bottom = mBookOffset.y + 780 * mScale;
        if (p.y - r <= top || p.y + r >= bottom)
            return false;
        for (const auto range : std::array<Rml::Vector2f, 2>{{{200, 760}, {910, 1490}}})
            if (p.x - r > mBookOffset.x + range.x * mScale &&
                p.x + r < mBookOffset.x + range.y * mScale)
                return true;
        return false;
    }
    void tabs(float t) {
        const int count = int(mState.chapters.size());
        const float tw = count > 4 ? 218.f : 245.f,
                    start = (width - count * tw - (count - 1) * 16) / 2;
        for (int i = 0; i < count; ++i) {
            const float x = start + i * (tw + 16);
            const bool active = i == mState.chapterIndex;
            const float drop = chapter_offset(i);
            const float lift =
                active ? 11 * std::sin(std::clamp(float((clock() - mTabStart) / .48), 0.f, 1.f) *
                                       3.14159265f)
                       : 0;
            if (active) {
                for (const auto &a : storybook_art::sprites)
                    if (Rml::String(a.name) == "tab-selected") {
                        // Shared integer-pixel vertices: no independently alpha-covered
                        // nine-slice boundaries (the updated preview's seam fix).
                        const float edge = 36, sx[] = {a.x, a.x + 40, a.x + a.w - 40, a.x + a.w},
                                    sy[] = {a.y, a.y + 40, a.y + a.h - 40, a.y + a.h};
                        const float dx[] = {x, x + edge, x + tw - edge, x + tw},
                                    dy[] = {17 + drop - lift, 17 + drop - lift + edge,
                                            121 + drop - lift - edge, 121 + drop - lift};
                        Rml::Mesh mesh;
                        const auto c = color(0xffffff);
                        for (int r = 0; r < 4; ++r)
                            for (int j = 0; j < 4; ++j)
                                mesh.vertices.push_back({{std::round(dx[j] * mScale) / mScale,
                                                          std::round(dy[r] * mScale) / mScale},
                                                         c,
                                                         {sx[j] / a.width, sy[r] / a.height}});
                        for (int r = 0; r < 3; ++r)
                            for (int j = 0; j < 3; ++j) {
                                int k = r * 4 + j;
                                mesh.indices.insert(mesh.indices.end(),
                                                    {k, k + 1, k + 5, k, k + 5, k + 4});
                            }
                        submit(std::move(mesh), texture(a.name));
                        break;
                    }
            } else {
                rounded(x - 2, 27 + drop, tw + 4, 110, 13, 0xdfca96, 0xd1ba79);
                rounded(x, 29 + drop, tw, 106, 11, 0xfffef6, 0xf1e7c9);
            }
            text(mState.chapters[i], x + tw / 2, 71 + drop - lift, 34, active ? 0xffffff : 0, 1,
                 tw - 30);
        }
    }
    void value(const Rml::String &value, float y, unsigned rgb, float alpha, float pop) {
        const auto split = value.find(" (");
        if (split != Rml::String::npos) {
            text(value.substr(0, split), 655.5f, y - 7, 32, rgb, 1, 169, alpha, pop);
            text(value.substr(split + 1), 655.5f, y + 13, 15, rgb, 1, 176, alpha);
        } else
            text(value, 655.5f, y, 32, rgb, 1, 169, alpha, pop);
    }
    void media_frame(storybook_geometry::Rect p) {
        // A paper photo slipped into an album, not a rigid picture frame.
        // The same thin stock and soft shadow follow every printed image's warp.
        strip(p.x - 10, p.y - 6, p.w + 20, p.h + 20, {}, 0, 0, 1, 1, 0, true, 0x806b40, .12f);
        strip(p.x - 9, p.y - 9, p.w + 18, p.h + 18, {}, 0, 0, 1, 1, 0, true, 0xfffdf4);
    }
    void media_corners(storybook_geometry::Rect p) {
        // Small folded paper pockets sit over the photo corners. Sample each
        // edge so the pockets stay attached during the existing page curl.
        const float size = std::min(34.f, std::min(p.w, p.h) * .13f);
        for (int corner = 0; corner < 4; ++corner) {
            const float sx = corner % 2 ? -1.f : 1.f, sy = corner / 2 ? -1.f : 1.f;
            const float x = corner % 2 ? p.x + p.w + 11 : p.x - 11,
                        y = corner / 2 ? p.y + p.h + 11 : p.y - 11;
            const Rml::Vector2f points[] = {{x, y}, {x + sx * size, y}, {x, y + sy * size}};
            std::array<Rml::Vector2f, 18> outline;
            int count = 0;
            for (int edge = 0; edge < 3; ++edge)
                for (int i = 0; i < 6; ++i)
                    outline[size_t(count++)] =
                        points[edge] + (points[(edge + 1) % 3] - points[edge]) * (i / 6.f);
            auto shadow = outline;
            for (auto &point : shadow)
                point.y += 1.5f;
            smooth_polygon(shadow.data(), count, 0x9f8041, .18f, true);
            smooth_polygon(outline.data(), count, 0xffe777, 1, true);
            line(x + sx * (size - 2), y + sy * 2, x + sx * 2, y + sy * (size - 2), 1, 0xfff7c5,
                 .8f);
        }
    }
    void page(const StorybookState &state, float t, bool live, Print print = Print::All) {
        if (state.launcher) {
            launcher_page(state, t, live, print);
            return;
        }
        if (mSide == 0) {
            const int offset = state.selected / 6 * 6;
            float position = live ? row_position(clock()) : float(state.selected % 6);
            if (print != Print::Cursor) {
                art("selection-ribbon", 145, 147 + position * 73, 666, 72);
                for (int i = offset; i < std::min(offset + 6, int(state.rows.size())); ++i) {
                    const auto &row = state.rows[i];
                    const float y = 195 + (i - offset) * 73;
                    const bool active = i == state.selected;
                    const unsigned coverage = unsigned(
                        255 * std::clamp(1 - std::abs(float(i - offset) - position), 0.f, 1.f));
                    const unsigned ink = coverage * 0x010101;
                    const float a = row.disabled ? .42f : 1;
                    const float vp = live ? std::clamp(float((clock() - mValueStart) / .24), 0.f,
                                                       1.f)
                                          : 1,
                                pop = active ? 1 + .07f * std::sin(vp * 3.14159265f) : 1;
                    text(row.label, 183, y, 35, ink, 0, row.adjustable ? 350 : 580, a);
                    value(row.value, y, ink, a, pop);
                    if (row.adjustable) {
                        const float kick = active ? 7 * std::sin(vp * 3.14159265f) : 0;
                        arrow(555 - (row.canLeft ? kick : 0), y - 14, -1, 1,
                              row.canLeft && !row.disabled ? 0xffbc16 : 0xa4a39b);
                        arrow(756 + (row.canRight ? kick : 0), y - 14, 1, 1,
                              row.canRight && !row.disabled ? 0xffbc16 : 0xa4a39b);
                    }
                }
                if (state.rows.size() > 6) {
                    text(std::to_string(offset / 6 + 1) + " / " +
                             std::to_string((state.rows.size() + 5) / 6),
                         470, 662, 23, 0, 1);
                    arrow(400, 652, -1);
                    arrow(540, 652, 1);
                }
            }
            if (print == Print::Ink)
                return;
            const float pulse = 1 + .035f * std::sin(t * 3),
                        hop = live ? std::sin(std::clamp(float((clock() - mSelectionStart) / .19),
                                                         0.f, 1.f) *
                                              3.14159265f)
                                   : 0;
            art("cursor-star", 119 - (pulse - 1) * 28.5f - 7 * hop,
                156 + position * 73 - (pulse - 1) * 29.5f, 57 * pulse, 59 * pulse,
                std::sin(t * 2) * .065f);
            return;
        }
        if (state.chapter == "Picture" && state.inGame) {
            auto tex = texture("mp6scene://live");
            // Aurora's borrowed scene texture advertises a nominal 720p size;
            // the actual frame follows the viewport, including non-16:9 windows.
            const auto scene = aurora::rmlui::scene_dimensions();
            const auto photo = storybook_geometry::preview_photo(mAxisScale.x, mAxisScale.y,
                                                                 float(scene.x), float(scene.y));
            const float x = photo.x, y = photo.y, w = photo.w, h = photo.h;
            media_frame(photo);
            strip(x, y, w, h, tex, 0, 0, 1, 1);
            media_corners(photo);
        } else {
            const auto photo =
                storybook_geometry::preview_photo(mAxisScale.x, mAxisScale.y, 320, 274);
            media_frame(photo);
            if (state.chapter == "Picture" || state.chapter == "Sound" || state.chapter == "Game") {
                const char *id = state.chapter == "Picture" ? "chapter-picture"
                                 : state.chapter == "Sound" ? "chapter-sound"
                                                            : "chapter-game";
                for (const auto &a : storybook_art::sprites)
                    if (Rml::String(a.name) == id) {
                        const float s = 252 / std::max(a.w, a.h);
                        art(id, 1216 - a.w * s / 2, 289 - a.h * s / 2, a.w * s, a.h * s);
                        break;
                    }
            } else {
                art("bubble-star", 1155, 194, 122, 122, -.08f);
                art("bubble-star", 1095, 320, 68, 68, -.23f);
                art("bubble-star", 1292, 305, 62, 62, .22f);
            }
            media_corners(photo);
        }
        help(state, live, 552);
        line(909, 622, 1180, 622, 2, 0xf6d000);
        line(1227, 622, 1422, 622, 2, 0xf6d000);
        art("bubble-star", 1189, 606, 32, 32);
        text("Changes are saved automatically.", 1210, 669, 25, 0, 1, 570);
    }
    void help(const StorybookState &state, bool live, float baseline) {
        const float dp =
            live ? std::clamp(float((clock() - mDescriptionStart) / .20), 0.f, 1.f) : 1;
        const bool leaving = dp < .35f;
        const auto &description = leaving ? mOldDescription : state.description;
        const float fade = leaving ? 1 - dp / .35f : std::clamp((dp - .35f) / .65f, 0.f, 1.f),
                    slide = leaving ? -10 * (1 - fade) : 10 * (1 - fade);
        auto found = mWrapped.find(description);
        if (found == mWrapped.end()) {
            if (mWrapped.size() >= 32)
                mWrapped.clear();
            std::vector<Rml::String> lines;
            Rml::String current, word;
            auto flush = [&]() {
                if (word.empty())
                    return;
                const auto next = current.empty() ? word : current + " " + word;
                if (!current.empty() && run(next, 26).width > 600) {
                    lines.push_back(current);
                    current = word;
                } else
                    current = next;
                word.clear();
            };
            for (char ch : description) {
                if (ch == ' ' || ch == '\n')
                    flush();
                else
                    word += ch;
            }
            flush();
            if (!current.empty())
                lines.push_back(current);
            found = mWrapped.emplace(description, std::move(lines)).first;
        }
        const auto &lines = found->second;
        for (size_t i = 0; i < lines.size() && i < 4; ++i)
            text(lines[i], 1216, baseline - float(lines.size() - 1) * 16 + float(i) * 32 + slide,
                 26, 0x0900ed, 1, 600, fade);
    }
    // Opening spread: the same paper, ink, star and motion as Settings. Semantic
    // launcher buttons still own all actions; this only paints their state.
    void launcher_page(const StorybookState &state, float t, bool live, Print print = Print::All) {
        if (mSide == 0) {
            const float position = live ? row_position(clock()) : float(state.selected),
                        top = launcherRowTop + position * launcherRowStride;
            if (print != Print::Cursor) {
                text("Ready to party?", 470, 224, 35, 0, 1, 570);
                art("selection-ribbon", 145, top, 666, launcherRowHeight);
                for (size_t i = 0; i < state.rows.size(); ++i) {
                    const unsigned coverage =
                        unsigned(255 * std::clamp(1 - std::abs(float(i) - position), 0.f, 1.f));
                    text(state.rows[i].label, 204, launcherRowTop + 58 + i * launcherRowStride, 46,
                         coverage * 0x010101, 0, 555);
                }
            }
            if (print != Print::Ink) {
                const float pulse = 1 + .035f * std::sin(t * 3);
                art("cursor-star", 118 - (pulse - 1) * 30, top + 14 - (pulse - 1) * 30, 60 * pulse,
                    62 * pulse, std::sin(t * 2) * .065f);
            }
            if (print == Print::Cursor)
                return;
            text("Native port", 470, 691, 23, 0, 1, 490);
            text("Version " + state.version, 470, 726, 20, 0, 1, 490);
            return;
        }
        bool drawn = false;
        if (state.wordmark) {
            const auto logo = texture("mp6tex://wordmark");
            const auto d = logo.GetDimensions();
            if (d.x > 0 && d.y > 0) {
                const auto mount = storybook_geometry::launcher_logo(float(d.x), float(d.y));
                const auto &image = mount.artwork;
                media_frame(mount.paper);
                strip(image.x, image.y, image.w, image.h, logo, 0, 0, 1, 1);
                media_corners(mount.paper);
                drawn = true;
            }
        }
        if (!drawn) {
            art("bubble-star", 1179, 206, 74, 74);
            text("Starlit Party", 1216, 363, 46, 0, 1, 590);
        }
        help(state, live, 553);
        line(965, 622, 1180, 622, 2, 0xf6d000);
        line(1227, 622, 1467, 622, 2, 0xf6d000);
        art("bubble-star", 1189, 606, 32, 32);
        text(state.linked ? "Game linked. Ready to play!" : "Link your game to begin.", 1216, 676,
             25, 0, 1, 570);
        text(state.linked ? "GameCube / USA" : "Your own Mario Party 6 (USA) disc", 1216, 714, 20,
             0, 1, 570);
    }
    void foreground(float t) {
        const float drift = std::sin(t * .5f) * 3;
        // Geometry::Render rounds draw translations to whole pixels. Bake this
        // subpixel drift into the direct strip instead, preserving smooth motion.
        auto cloud = [&](const char *id, float x, float y, bool flip = false) {
            const float h = Rml::String(id) == "cloud-round"  ? 130
                            : Rml::String(id) == "cloud-bank" ? 95
                                                              : 94;
            art(id, x - 6 + drift, y - 6, 264, h, 0, flip);
        };
        if (mLeftReveal > 0) {
            mLayer = Layer::Brighton;
            cloud("cloud-round", -167, 593);
            cloud("cloud-bank", -84, 738);
            cloud("cloud", 24, 787);
            art("brighton", -9, 607 + std::sin(t * 1.2f) * 2, 234, 250, std::sin(t * .9f) * .025f);
            cloud("cloud-round", -99, 792);
        }
        if (mRightReveal > 0) {
            mLayer = Layer::Twila;
            cloud("cloud-round", 1564, 610, true);
            cloud("cloud-bank", 1392, 773, true);
            art("twila", 1468, 628 + std::sin(t * .9f) * 3, 172, 199, std::sin(t * .8f) * .028f);
            art("cursor-star", 1405, 785, 53, 54, -.13f);
            cloud("cloud-bank", 1453, 802, true);
        }
    }
    void footer() {
        // Capsule footer; A/B are one grayscale sprite with per-instance tint.
        quad(466, 877 - mFooterLift, 740, 56, 0x0c115d);
        ellipse(466, 905 - mFooterLift, 28, 28, 0x0c115d);
        ellipse(1206, 905 - mFooterLift, 28, 28, 0x0c115d);
        quad(466, 879 - mFooterLift, 740, 52, 0xfaf8f0);
        ellipse(466, 905 - mFooterLift, 26, 26, 0xfaf8f0);
        ellipse(1206, 905 - mFooterLift, 26, 26, 0xfaf8f0);
        if (mState.launcher) {
            art("button-round", 555, 881 - mFooterLift, 49, 49, 0, false, true, 0x11f64e);
            text("A", 579.5f, 915 - mFooterLift, 30, 0xffffff, 1);
            text("Choose", 620, 914 - mFooterLift, 26);
            line(797, 891 - mFooterLift, 797, 919 - mFooterLift, 2, 0xd6b75c, 1, false);
            text("Up / Down: Move", 835, 914 - mFooterLift, 23, 0, 0, 335);
            return;
        }
        art("button-round", 524, 881 - mFooterLift, 49, 49, 0, false, true, 0x11f64e);
        art("button-round", 727, 881 - mFooterLift, 49, 49, 0, false, true, 0xff1818);
        art("button-shoulder", 904, 885 - mFooterLift, 54, 42);
        art("button-shoulder", 989, 885 - mFooterLift, 54, 42, 0, true);
        text("A", 548.5f, 915 - mFooterLift, 30, 0xffffff, 1);
        text("B", 751.5f, 915 - mFooterLift, 30, 0xffffff, 1);
        text("L", 932, 919 - mFooterLift, 30, 0xffffff, 1);
        text("R", 1017, 919 - mFooterLift, 30, 0xffffff, 1);
        text("Choose", 584, 914 - mFooterLift, 26);
        text("Back", 786, 914 - mFooterLift, 26);
        text("/", 975, 919 - mFooterLift, 35, 0, 1);
        text("Chapters", 1059, 914 - mFooterLift, 26);
    }
};
} // namespace mp6::ui
