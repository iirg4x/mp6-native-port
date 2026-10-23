#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

namespace mp6::ui::storybook_geometry {
struct Point { float x, y; };
struct Rect { float x,y,w,h; };
constexpr float design_width=1672, design_height=941;

// Procedural decorations use the same small set of circle resolutions every
// frame. Keep the trigonometry in a shared, immutable table; entries remain
// float so applying the live radius and center has the same rounding as the
// original float cos/sin expression, while avoiding a heap-backed point list
// and repeated trig calls.
struct RingPoint { float x, y; };
struct PrimitiveRing { std::array<RingPoint,48> points{}; };
struct FixedPanelPolygon { std::array<Point,12> points{}; int size=0; };
inline const PrimitiveRing& ellipse_ring(int count) {
    static const auto rings=[] {
        std::array<PrimitiveRing,41> result{};
        for(int n=8;n<=48;++n) {
            auto& ring=result[size_t(n-8)];
            for(int i=0;i<n;++i) {
                const float angle=i*6.2831853f/n;
                ring.points[size_t(i)]={std::cos(angle),std::sin(angle)};
            }
        }
        return result;
    }();
    return rings[size_t(std::clamp(count,8,48)-8)];
}
inline Point ellipse_point(const PrimitiveRing& ring,int count,int index,float x,float y,float rx,float ry) {
    // Glow closes with the original final angle rather than assuming the
    // first sample is bit-identical after the float division by count.
    if(index==count) {
        const float angle=index*6.2831853f/count;
        return {x+std::cos(angle)*rx,y+std::sin(angle)*ry};
    }
    const auto& point=ring.points[size_t(index)];
    return {x+point.x*rx,y+point.y*ry};
}
inline Point rotate_offset(Point center,Point offset,float cosine,float sine) {
    return {center.x+cosine*offset.x-sine*offset.y,
            center.y+sine*offset.x+cosine*offset.y};
}

// Mask rays begin as triangles (often already viewport-clipped) and panel fills
// as quads. Their intersection with a tile has at most seven vertices; keep
// the exact edge/vertex order of clip_to_panel without allocating four vectors
// per tile. Requires a convex polygon of at most eight points (four clipping
// planes can add at most four vertices). Larger/concave callers use the vector
// implementation. The pointer form stays local to the mask renderer.
inline FixedPanelPolygon clip_to_panel_fixed(const Point* source,int count,float width,float height) {
    FixedPanelPolygon result;
    if(count<=0)return result;
    assert(count<=8);
    if(count>8)return result;
    // Most ray/tile pairs are disjoint or already contained. Avoid running
    // four clipping passes (and copying both scratch arrays) in those cases.
    // Strict outside comparisons preserve touching/degenerate boundary output.
    float left=source[0].x,right=left,top=source[0].y,bottom=top;
    for(int i=1;i<count;++i){
        left=std::min(left,source[i].x);right=std::max(right,source[i].x);
        top=std::min(top,source[i].y);bottom=std::max(bottom,source[i].y);
    }
    if(right<0||left>width||bottom<0||top>height)return result;
    if(left>=0&&right<=width&&top>=0&&bottom<=height){
        result.size=count;
        for(int i=0;i<count;++i)result.points[size_t(i)]=source[i];
        return result;
    }
    std::array<Point,12> polygon{},clipped{};
    for(int i=0;i<count;++i)polygon[size_t(i)]=source[i];
    for(int edge=0;edge<4&&count>0;++edge) {
        const bool horizontal=edge>=2,lower=(edge%2)==0;
        const float bound=lower?0.f:(horizontal?height:width);
        const auto coordinate=[&](Point p){return horizontal?p.y:p.x;};
        const auto inside=[&](Point p){return lower?coordinate(p)>=bound:coordinate(p)<=bound;};
        int clippedCount=0;
        Point previous=polygon[size_t(count-1)];bool previousInside=inside(previous);
        for(int i=0;i<count;++i) {
            const Point current=polygon[size_t(i)];
            const bool currentInside=inside(current);
            if(currentInside!=previousInside) {
                const float t=(bound-coordinate(previous))/(coordinate(current)-coordinate(previous));
                Point intersection{previous.x+(current.x-previous.x)*t,previous.y+(current.y-previous.y)*t};
                if(horizontal)intersection.y=bound;else intersection.x=bound;
                assert(clippedCount<int(clipped.size()));
                if(clippedCount<int(clipped.size()))clipped[size_t(clippedCount++)]=intersection;
            }
            if(currentInside&&clippedCount<int(clipped.size()))clipped[size_t(clippedCount++)]=current;
            previous=current;previousInside=currentInside;
        }
        polygon.swap(clipped);count=clippedCount;
    }
    result.size=count;
    for(int i=0;i<count;++i)result.points[size_t(i)]=polygon[size_t(i)];
    return result;
}

// Initial scattered particles are all below the viewport after 16 seconds.
// Visit only living births, not every expired particle since process startup.
inline int first_confetti_birth(float seconds) {
    return seconds<=16.f?-24:std::max(0,int((seconds-16.f)*2.2f));
}

// The shared UI sampler repeats. Full-width book/cloud sprites must stop at
// texel centers so their transparent outer edge cannot sample the opaque spine
// on the opposite side. Interior atlas crop coordinates stay unchanged.
inline float sprite_uv(float pixel,float extent) {
    return std::clamp(pixel,.5f,extent-.5f)/extent;
}

// The entire book is one rigid composition. Only the sky fills independently;
// never derive separate x/y scales for paper, print, tabs or hit testing.
struct BookFit {
    float scale;
    Point offset;
    Point project(Point p) const {return {offset.x+p.x*scale,offset.y+p.y*scale};}
    Point unproject(Point p) const {return {(p.x-offset.x)/scale,(p.y-offset.y)/scale};}
};
inline BookFit fit_book(float viewportWidth,float viewportHeight) {
    const float scale=std::max(.0001f,std::min(viewportWidth/design_width,viewportHeight/design_height));
    return {scale,{(viewportWidth-design_width*scale)/2,(viewportHeight-design_height*scale)/2}};
}

struct MaskTile { Rect panel,uv; };
inline std::vector<MaskTile> sky_mask_tiles(float viewportWidth,float viewportHeight,float scale) {
    // One opaque black/white 64x126 tooth, with matching top/bottom edges.
    // Source pixels scale equally on both axes. Repeat vertically, and extend
    // only the constant opaque/transparent margins to cover a wider viewport.
    const float unit=design_width/2048*scale;
    const float sx=viewportWidth/design_width,sy=viewportHeight/design_height;
    const float tileHeight=126*unit/sy;
    const float left=std::clamp(design_width/2-32*unit/sx,0.f,design_width);
    const float right=std::clamp(design_width/2+32*unit/sx,0.f,design_width);
    const float columns[]={0,left,right,design_width};
    std::vector<MaskTile> tiles;
    for(float y=0;y<design_height;y+=tileHeight) {
        const float h=std::min(tileHeight,design_height-y);
        for(int i=0;i<3;++i)if(columns[i+1]>columns[i])
            tiles.push_back({{columns[i],y,columns[i+1]-columns[i],h},
                {i==2?1.f:0.f,0,i==1?1.f:0.f,h*sy/(unit*126)}});
    }
    return tiles;
}
inline Point tile_mask_uv(Point p,const MaskTile& tile,float textureWidth,float textureHeight) {
    const float u=tile.uv.x+(p.x-tile.panel.x)/tile.panel.w*tile.uv.w;
    const float v=tile.uv.y+(p.y-tile.panel.y)/tile.panel.h*tile.uv.h;
    return {std::clamp(u,.5f/textureWidth,1-.5f/textureWidth),
            std::clamp(v,.5f/textureHeight,1-.5f/textureHeight)};
}

// A restrained, finite damped spring for each foreground host/cloud group.
// The squared envelope makes both endpoint velocities zero, so the last frame
// joins the idle drift without a hard stop. Peak overshoot is about 3.8%.
inline float host_spring(float phase) {
    if(phase<=0)return 0;
    if(phase>=1)return 1;
    const float remaining=1-phase;
    // Roundoff near phase zero must not invert a nearly collapsed sprite.
    return std::max(0.f,1-std::exp(-8*phase)*remaining*remaining*(std::cos(10*phase)+std::sin(10*phase)));
}

// Tabs rise in a relaxed left-to-right cascade, with each spring starting 100ms
// after the previous one. Position only: lettering and silhouettes never squash.
inline float chapter_tab_offset(float elapsed,int index) {
    const float phase=std::clamp((elapsed-index*.10f)/.48f,0.f,1.f);
    if(phase<=0)return 112;
    if(phase>=1)return 0;
    const float remaining=1-phase;
    return 112*std::exp(-6*phase)*remaining*remaining*
        (std::cos(13*phase)+(8.f/13)*std::sin(13*phase));
}

// One reversible playhead drives the whole entrance. Retargeting samples the
// current position before changing direction, including during delayed stages.
// The reverse trip uses its own speed, never a new fully-open starting pose.
struct BookTransition {
    double position=1, changedAt=0;
    bool opening=true, started=false;
    double progress(double now,double entrance,double exit) const {
        if(!started)return position;
        return std::clamp(position+(now-changedAt)*(opening?1/entrance:-1/exit),0.,1.);
    }
    void target(bool open,bool motion,double now,double entrance,double exit) {
        const double current=progress(now,entrance,exit);
        if(motion&&started&&opening==open)return;
        position=motion?(started?current:(open?0.:1.)):(open?1.:0.);
        changedAt=now;opening=open;started=true;
    }
};

// A photo mounted inside the printed right-page border, not at the paper's
// outer edge. Fit the complete scene in physical pixels before page curvature.
inline Rect preview_photo(float axisX,float axisY,float sceneWidth,float sceneHeight) {
    constexpr Rect slot{926,152,580,274};
    float w=slot.w*axisX,h=slot.h*axisY;
    if(sceneWidth>0&&sceneHeight>0) {
        const float fit=std::min(w/sceneWidth,h/sceneHeight);
        w=sceneWidth*fit;h=sceneHeight*fit;
    }
    w/=axisX;h/=axisY;
    return {slot.x+(slot.w-w)/2,slot.y+(slot.h-h)/2,w,h};
}

struct LogoMount { Rect paper,artwork; };
inline LogoMount launcher_logo(float sourceWidth,float sourceHeight) {
    // The whole album mount (including its 14-unit shadow/AA allowance) fits
    // inside the right-page ornament. The logo fits INSIDE the paper instead
    // of setting the paper's width and pushing its corners over the border.
    constexpr Rect paper{946,204,540,224};
    constexpr float inset=24;
    const Rect inner{paper.x+inset,paper.y+inset,paper.w-2*inset,paper.h-2*inset};
    if(sourceWidth<=0||sourceHeight<=0)return {paper,{inner.x,inner.y,0,0}};
    const float scale=std::min(inner.w/sourceWidth,inner.h/sourceHeight);
    const float w=sourceWidth*scale,h=sourceHeight*scale;
    return {paper,{inner.x+(inner.w-w)/2,inner.y+(inner.h-h)/2,w,h}};
}

inline Point mask_uv(Point p, float width, float height, float textureWidth, float textureHeight) {
    textureWidth=std::max(1.f,textureWidth);textureHeight=std::max(1.f,textureHeight);
    // Keep bilinear taps inside the first/last texel centers too. At UV 0/1 a
    // repeat sampler otherwise blends the opposite mask edge into a thin seam.
    return {(.5f+std::clamp(p.x/width,0.f,1.f)*(textureWidth-1))/textureWidth,
            (.5f+std::clamp(p.y/height,0.f,1.f)*(textureHeight-1))/textureHeight};
}

// Clip in panel-local coordinates, before its entrance/exit translation. The
// shared UI sampler repeats: off-panel mask UVs would otherwise wrap sunlight
// into the opening between the day and night panels.
inline std::vector<Point> clip_to_panel(std::vector<Point> polygon, float width, float height) {
    for (int edge=0; edge<4 && !polygon.empty(); ++edge) {
        const bool horizontal=edge>=2, lower=(edge%2)==0;
        const float bound=lower?0.f:(horizontal?height:width);
        const auto coordinate=[&](Point p){return horizontal?p.y:p.x;};
        const auto inside=[&](Point p){return lower?coordinate(p)>=bound:coordinate(p)<=bound;};
        std::vector<Point> clipped;
        clipped.reserve(polygon.size()+1);
        Point previous=polygon.back();bool previousInside=inside(previous);
        for (const Point current:polygon) {
            const bool currentInside=inside(current);
            if (currentInside!=previousInside) {
                const float t=(bound-coordinate(previous))/(coordinate(current)-coordinate(previous));
                Point intersection{previous.x+(current.x-previous.x)*t,previous.y+(current.y-previous.y)*t};
                // Pin the clipped axis exactly to the boundary (no UV overshoot).
                if(horizontal)intersection.y=bound;else intersection.x=bound;
                clipped.push_back(intersection);
            }
            if(currentInside)clipped.push_back(current);
            previous=current;previousInside=currentInside;
        }
        polygon.swap(clipped);
    }
    return polygon;
}

inline std::vector<Point> sky_ray(float angle, float width, float height, float axisX=1, float axisY=1) {
    // Construct the fan in screen space, not in the stretched sky coordinates.
    // Resizing may move its sun anchor, but must not widen or squash its angles.
    // Reach past every viewport corner; only clipping determines ray length.
    const float reach=2*std::hypot(width*axisX,height*axisY);
    return clip_to_panel({{175,114},
        {175+std::cos(angle-.075f)*reach/axisX,114+std::sin(angle-.075f)*reach/axisY},
        {175+std::cos(angle+.075f)*reach/axisX,114+std::sin(angle+.075f)*reach/axisY}},width,height);
}
inline FixedPanelPolygon sky_ray_fixed(float angle,float width,float height,float axisX=1,float axisY=1) {
    // Keep the source expressions identical to sky_ray; only the clipping
    // storage changes, so every float and vertex remains reference-equivalent.
    const float reach=2*std::hypot(width*axisX,height*axisY);
    const std::array<Point,3> source{{{175,114},
        {175+std::cos(angle-.075f)*reach/axisX,114+std::sin(angle-.075f)*reach/axisY},
        {175+std::cos(angle+.075f)*reach/axisX,114+std::sin(angle+.075f)*reach/axisY}}};
    return clip_to_panel_fixed(source.data(),int(source.size()),width,height);
}
} // namespace mp6::ui::storybook_geometry
