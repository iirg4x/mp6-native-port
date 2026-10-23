#include "../../src/gx/ui/storybook_geometry.hpp"
#include <array>
#include <cassert>
#include <cstdio>

using namespace mp6::ui::storybook_geometry;
constexpr float width=1672, height=941, pi=3.14159265f;

static bool contains(const std::vector<Point>& polygon, Point p) {
    bool positive=false,negative=false;
    for(size_t i=0;i<polygon.size();++i) {
        const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        const float cross=(b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
        positive|=cross>0.001f;negative|=cross<-0.001f;
    }
    return polygon.size()>=3&&!(positive&&negative);
}

int main() {
    for(const float extent:std::array<float,4>{{64,128,264,2048}}) {
        assert(sprite_uv(0,extent)==.5f/extent);
        assert(sprite_uv(extent,extent)==1-.5f/extent);
        assert(sprite_uv(4,extent)==4/extent);
        assert(sprite_uv(extent-4,extent)==1-4/extent);
    }
    puts("Storybook sprites: no repeat-sampler bleed at full-texture edges");
    // The renderer's repeated ellipses/glows use the same circle resolutions.
    // The shared table must preserve the old angle/float rounding after each
    // caller applies its live center and radii, while returning one stable
    // template per resolution rather than allocating a point vector.
    for(const Point sample:std::array<Point,3>{{{37.25f,-12.5f},{0.125f,941.75f},{1672.5f,3.75f}}})
        for(const Point radius:std::array<Point,3>{{{2.75f,4.5f},{31.125f,18.875f},{.03125f,.0625f}}})
            for(int count=8;count<=48;++count) {
        const auto& ring=ellipse_ring(count);
        assert(&ring==&ellipse_ring(count));
        for(int i=0;i<=count;++i) {
            const float angle=i*6.2831853f/count;
            const auto actual=ellipse_point(ring,count,i,sample.x,sample.y,radius.x,radius.y);
            const float expectedX=sample.x+std::cos(angle)*radius.x,expectedY=sample.y+std::sin(angle)*radius.y;
            assert(actual.x==expectedX&&actual.y==expectedY);
        }
    }
    assert(&ellipse_ring(8)==&ellipse_ring(7));
    assert(&ellipse_ring(48)==&ellipse_ring(49));
    for(const Point center:std::array<Point,3>{{{37.25f,-12.5f},{0.125f,941.75f},{1672.5f,3.75f}}})
        for(const Point offset:std::array<Point,3>{{{2.75f,4.5f},{31.125f,-18.875f},{.03125f,-.0625f}}})
            for(const float angle:std::array<float,4>{{-.7f,0,.125f,6.1f}}) {
        const float cosine=std::cos(angle),sine=std::sin(angle);
        const auto actual=rotate_offset(center,offset,cosine,sine);
        const Point expected{center.x+std::cos(angle)*offset.x-std::sin(angle)*offset.y,
            center.y+std::sin(angle)*offset.x+std::cos(angle)*offset.y};
        assert(actual.x==expected.x&&actual.y==expected.y);
    }
    puts("Storybook primitive reuse: ring, glow closure and confetti transforms preserve legacy float samples");
    for(const auto viewport:std::array<Point,8>{{{390,720},{640,480},{1280,960},{1280,720},
            {1920,820},{2560,1440},{3840,2160},{900,420}}}) {
        const auto fit=fit_book(viewport.x,viewport.y),again=fit_book(viewport.x*2,viewport.y*2);
        const auto first=fit.project({0,0}),last=fit.project({width,height});
        assert(first.x>=-.001f&&first.y>=-.001f&&last.x<=viewport.x+.001f&&last.y<=viewport.y+.001f);
        assert(std::abs((last.x-first.x)/(last.y-first.y)-width/height)<.0001f);
        assert(std::abs(first.x+last.x-viewport.x)<.001f&&std::abs(first.y+last.y-viewport.y)<.001f);
        assert(std::abs(again.scale-fit.scale*2)<.0001f);
        for(const auto point:std::array<Point,4>{{{0,0},{width,height},{555,181},{548,905}}}) {
            const auto mapped=fit.unproject(fit.project(point));
            assert(std::abs(mapped.x-point.x)<.001f&&std::abs(mapped.y-point.y)<.001f);
        }
    }
    puts("Storybook uniform fit: centered, uncropped, fixed aspect and round-trip input passed at eight sizes");
    for(const auto viewport:std::array<Point,6>{{{390,720},{1280,960},{1280,720},{1920,820},{2560,1440},{900,420}}}) {
        const float scale=fit_book(viewport.x,viewport.y).scale;
        const auto tiles=sky_mask_tiles(viewport.x,viewport.y,scale);
        float area=0;
        for(const auto& tile:tiles) {
            assert(tile.panel.x>=0&&tile.panel.y>=0);
            assert(tile.panel.x+tile.panel.w<=width+.001f&&tile.panel.y+tile.panel.h<=height+.001f);
            assert(tile.uv.y==0&&tile.uv.h<=1.0001f);
            if(tile.uv.w>0){
                const float sx=tile.panel.w*viewport.x/width/(tile.uv.w*64);
                const float sy=tile.panel.h*viewport.y/height/(tile.uv.h*126);
                assert(std::abs(sx-sy)<.0001f&&std::abs(sx-scale*width/2048)<.0001f);
            }
            const auto uv=tile_mask_uv({tile.panel.x,tile.panel.y},tile,64,126);
            assert(uv.x>=.5f/64&&uv.x<=1-.5f/64&&uv.y==.5f/126);
            area+=tile.panel.w*tile.panel.h;
        }
        assert(std::abs(area-width*height)<4);
    }
    puts("Storybook mask: repeated source tooth, uniform pixel scale and complete coverage passed at six sizes");
    // No viewport/book scaling on ray directions: the same angular wedge is
    // visible at every aspect and DPI, anchored to the screen-space sun.
    for(const auto viewport:std::array<Point,8>{{{390,720},{640,480},{1280,960},{1280,720},
            {1920,820},{2560,1440},{3840,2160},{900,420}}}) {
        const float sx=viewport.x/width,sy=viewport.y/height;
        const Point sun{175*sx,114*sy};
        const float radius=.5f*std::min(sun.x,sun.y);
        for(int rotation=0;rotation<360;rotation+=5) {
            const float angle=rotation*pi/180;
            auto ray=sky_ray(angle,width,height,sx,sy);
            for(auto& p:ray){p.x*=sx;p.y*=sy;
                assert(p.x>=0&&p.x<=viewport.x+.001f&&p.y>=0&&p.y<=viewport.y+.001f);}
            for(const float offset:std::array<float,5>{{-.09f,-.06f,0,.06f,.09f}}) {
                const Point sample{sun.x+std::cos(angle+offset)*radius,sun.y+std::sin(angle+offset)*radius};
                assert(contains(ray,sample)==(std::abs(offset)<.075f));
            }
        }
    }
    puts("Storybook rays: unscaled screen-space angles passed at eight viewport/DPI sizes");
    // The foreground pop is an actual rebound, not scale overshoot cancelled
    // by a linear slide. Both directions meet their endpoints at zero velocity.
    const auto exitSpring=[](float p){return host_spring((1-p)*(1-p));};
    assert(host_spring(-1)==0&&host_spring(0)==0&&host_spring(1)==1&&host_spring(2)==1);
    assert(exitSpring(0)==1&&exitSpring(1)==0);
    constexpr float epsilon=.0001f;
    assert(std::abs(host_spring(epsilon)/epsilon)<.03f);
    assert(std::abs((1-host_spring(1-epsilon))/epsilon)<.03f);
    assert(std::abs((exitSpring(epsilon)-1)/epsilon)<.03f);
    assert(std::abs(exitSpring(1-epsilon)/epsilon)<.03f);
    float entrancePeak=1,exitPeak=1;
    for(int i=0;i<=4000;++i){const float p=i/4000.f,in=host_spring(p),out=exitSpring(p);
        assert(std::isfinite(in)&&std::isfinite(out));
        assert(in>=0&&in<1.04f&&out>=0&&out<1.04f);
        entrancePeak=std::max(entrancePeak,in);exitPeak=std::max(exitPeak,out);
    }
    assert(entrancePeak>1.03f&&exitPeak>1.03f);
    assert(host_spring(.615f)<1); // a smaller counter-settle after the overshoot
    // Fixed group scaling stays proportional at all menu aspects, and the
    // rebound moves the top of the host above its idle position, then back.
    for(const auto window:std::array<Point,3>{{{1280,720},{1280,960},{1920,820}}}) {
        const float scale=std::min(window.x/width,window.y/height);
        const float spring=host_spring(.31f);
        const float headOffset=(-253*spring+70*(1-spring)+253)*scale;
        assert(headOffset<0&&headOffset>-13*scale);
        assert(std::abs((234*scale*spring)/(250*scale*spring)-234.f/250)<.0001f);
    }
    puts("Storybook foreground spring: bounded rebound, zero-velocity endpoints and proportional groups passed");
    for(int tab=0;tab<5;++tab){
        assert(chapter_tab_offset(-1,tab)==112&&chapter_tab_offset(.90f,tab)==0);
        assert(std::abs(chapter_tab_offset(tab*.10f,tab)-112)<.001f);
        float minimum=0;
        for(int i=0;i<=9000;++i){const float offset=chapter_tab_offset(i/10000.f,tab);
            assert(std::isfinite(offset)&&offset>=-17&&offset<=112.001f);
            minimum=std::min(minimum,offset);
        }
        assert(minimum<-10); // A visible rebound, not a plain easing slide.
    }
    assert(chapter_tab_offset(.02f,0)<chapter_tab_offset(.02f,1));
    for(int tab=1;tab<5;++tab){
        assert(chapter_tab_offset(tab*.10f-.005f,tab)==112);
        assert(chapter_tab_offset(tab*.10f,tab-1)<0);
    }
    puts("Storybook chapter tabs: 100ms cascade, bounded rebound and 880ms final settle passed");
    for(int i=0;i<=100;++i){
        BookTransition transition;transition.target(true,true,0,1.36,.8);
        const double now=i*.0136,before=transition.progress(now,1.36,.8);
        transition.target(false,true,now,1.36,.8);
        assert(std::abs(transition.progress(now,1.36,.8)-before)<1e-10);
        const double reverseAt=now+.015,reversePose=transition.progress(reverseAt,1.36,.8);
        transition.target(true,true,reverseAt,1.36,.8);
        assert(std::abs(transition.progress(reverseAt,1.36,.8)-reversePose)<1e-10);
        assert(transition.progress(reverseAt+1.36,1.36,.8)==1);
        transition.target(false,false,reverseAt+2,1.36,.8);
        assert(transition.progress(reverseAt+2,1.36,.8)==0);
    }
    puts("Storybook reversible presentation: no pose jumps at every intro phase passed");
    // Full-scene fit stays inside the padded paper slot at narrow/wide windows,
    // including supersampled scene textures and the 4:3 game in a wide window.
    for(const auto window:std::array<Point,8>{{{640,480},{1280,960},{1280,720},{1920,1080},
            {1840,790},{2560,1440},{3840,2160},{900,420}}})
        for(const auto scene:std::array<Point,5>{{{640,480},{1280,720},{1840,790},{3840,2160},{1016,435}}}) {
            const float sx=fit_book(window.x,window.y).scale,sy=sx;
            const auto photo=preview_photo(sx,sy,scene.x,scene.y);
            assert(photo.x>=926-.001f&&photo.y>=152-.001f);
            assert(photo.x+photo.w<=1506+.001f&&photo.y+photo.h<=426+.001f);
            assert(std::abs(photo.w*sx/(photo.h*sy)-scene.x/scene.y)<.0001f);
        }
    // Logo artwork and its complete paper mount have independent bounds. The
    // widest old logo made the outer corners cross the printed page border.
    for(const auto viewport:std::array<Point,6>{{{640,480},{1280,720},{1280,960},{1840,790},{2560,1440},{900,420}}})
        for(const auto source:std::array<Point,4>{{{512,164},{1024,328},{256,256},{64,512}}}) {
            const auto mount=launcher_logo(source.x,source.y);const auto p=mount.paper,a=mount.artwork;
            assert(p.x-14>=926&&p.x+p.w+14<=1506);
            assert(p.y-14>=152&&p.y+p.h+18<=452);
            assert(a.x>=p.x+24-.001f&&a.y>=p.y+24-.001f);
            assert(a.x+a.w<=p.x+p.w-24+.001f&&a.y+a.h<=p.y+p.h-24+.001f);
            const auto fit=fit_book(viewport.x,viewport.y);
            assert(std::abs((a.w*fit.scale)/(a.h*fit.scale)-source.x/source.y)<.0001f);
            assert(std::abs(a.x+a.w/2-(p.x+p.w/2))<.001f);
            assert(std::abs(a.y+a.h/2-(p.y+p.h/2))<.001f);
        }
    puts("Launcher logo: proportional artwork inset inside a bounded paper mount at six viewport sizes passed");
    // This local point enters the visible opening during early intro/late outro.
    // Its old UV (1800/1672) repeats onto the opaque left side of the day mask.
    const std::vector<Point> oldRay{{175,114},
        {175+std::cos(-.075f)*2000,114+std::sin(-.075f)*2000},
        {175+std::cos(.075f)*2000,114+std::sin(.075f)*2000}};
    assert(contains(oldRay,{1800,114}));
    assert(!contains(sky_ray(0,width,height),{1800,114}));
    assert(contains(sky_ray(0,width,height),{1600,114})); // Still reaches the edge.
    assert(clip_to_panel({{-3,-3},{-1,-3},{-1,-1}},width,height).empty());
    assert(clip_to_panel({},width,height).empty());
    const auto full=clip_to_panel({{-20,-20},{1800,-20},{1800,1000},{-20,1000}},width,height);
    assert(full.size()==4&&contains(full,{0,0})&&contains(full,{width,height}));
    // The fixed-capacity mask path must preserve every legacy clipped vertex
    // bit-for-bit, including edge intersections and the fan's coverage area.
    const auto compareFixedClip=[](const std::vector<Point>& input,float w,float h) {
        const auto expected=clip_to_panel(input,w,h);
        const auto actual=clip_to_panel_fixed(input.data(),int(input.size()),w,h);
        assert(actual.size==int(expected.size()));
        for(int i=0;i<actual.size;++i)assert(actual.points[size_t(i)].x==expected[size_t(i)].x&&
            actual.points[size_t(i)].y==expected[size_t(i)].y);
        for(int i=1;i+1<actual.size;++i){
            const auto a=actual.points[0],b=actual.points[size_t(i)],c=actual.points[size_t(i+1)];
            const Point sample{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3};
            assert(sample.x>=0&&sample.x<=w&&sample.y>=0&&sample.y<=h);
        }
    };
    compareFixedClip({},width,height);
    compareFixedClip({{-3,-3},{-1,-3},{-1,-1}},width,height);
    compareFixedClip({{-20,-20},{1800,-20},{1800,1000},{-20,1000}},width,height);
    // Trivial reject/accept must preserve exact output and must not reject a
    // polygon merely touching a tile edge (including zero-area fan vertices).
    for(const auto& polygon:std::array<std::vector<Point>,9>{{
        {{10,10},{20,10},{20,20},{10,20}},
        {{0,0},{100,0},{100,100},{0,100}},
        {{-10,10},{0,10},{0,20},{-10,20}},
        {{100,10},{110,10},{110,20},{100,20}},
        {{10,-10},{20,-10},{20,0},{10,0}},
        {{10,100},{20,100},{20,110},{10,110}},
        {{101,10},{110,10},{110,20}},
        {{10,-10},{20,-10},{20,-1}},
        {{10,101},{20,101},{20,110}},
    }})compareFixedClip(polygon,100,100);
    for(int rotation=0;rotation<360;rotation+=3) {
        const float angle=rotation*pi/180;
        compareFixedClip({{175,114},{175+std::cos(angle-.075f)*4000,114+std::sin(angle-.075f)*4000},
            {175+std::cos(angle+.075f)*4000,114+std::sin(angle+.075f)*4000}},width,height);
    }
    for(const Point viewport:std::array<Point,8>{{{320,800},{390,720},{640,480},{1280,720},
            {1920,820},{2560,1440},{3840,2160},{900,420}}}) {
        const float sx=viewport.x/width,sy=viewport.y/height;
        for(int rotation=0;rotation<360;rotation++)for(int ray=0;ray<14;ray++) {
            const float angle=rotation*pi/180+ray*pi/7;
            const auto expected=sky_ray(angle,width,height,sx,sy);
            const auto actual=sky_ray_fixed(angle,width,height,sx,sy);
            assert(actual.size==int(expected.size()));
            for(int i=0;i<actual.size;++i)assert(actual.points[size_t(i)].x==expected[size_t(i)].x&&
                actual.points[size_t(i)].y==expected[size_t(i)].y);
        }
    }
    puts("Storybook fixed mask clipping: exact legacy vertices, fan coverage and edge intersections passed");
    const auto firstUV=mask_uv({0,0},width,height,2048,1024);
    const auto lastUV=mask_uv({width,height},width,height,2048,1024);
    assert(firstUV.x==.5f/2048&&firstUV.y==.5f/1024);
    assert(lastUV.x==1-.5f/2048&&lastUV.y==1-.5f/1024);

    // Sweep a full revolution of all fourteen rays. Every fan triangle and mask
    // UV stays in bounds, including corners, under every reveal and window size.
    const std::array<Point,4> sizes{{{1280,720},{1280,960},{1920,820},{2560,1440}}};
    for(int rotation=0;rotation<360;++rotation)for(int ray=0;ray<14;++ray) {
        const auto polygon=sky_ray(rotation*pi/180+ray*pi/7,width,height);
        assert(polygon.size()>=3&&polygon.size()<=7);
        for(const Point p:polygon) {
            assert(std::isfinite(p.x)&&std::isfinite(p.y));
            assert(p.x>=0&&p.x<=width&&p.y>=0&&p.y<=height);
            assert(p.x/width>=0&&p.x/width<=1&&p.y/height>=0&&p.y/height<=1);
            const auto uv=mask_uv(p,width,height,2048,1024);
            assert(uv.x>=firstUV.x&&uv.x<=lastUV.x&&uv.y>=firstUV.y&&uv.y<=lastUV.y);
            for(int step=0;step<=100;++step)for(const auto size:sizes) {
                const float reveal=step/100.f,offset=width*.55f*std::pow(1-reveal,3.f);
                const float screenX=(p.x-offset)*size.x/width;
                const float panelRight=(width-offset)*size.x/width;
                assert(screenX<=panelRight+.001f);
                assert(p.y*size.y/height<=size.y+.001f);
            }
        }
        // A few barycentric samples per fan ensure triangulation preserves the
        // convex clipped silhouette, rather than introducing an outside wedge.
        for(size_t i=1;i+1<polygon.size();++i) {
            const auto a=polygon[0],b=polygon[i],c=polygon[i+1];
            const Point sample{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3};
            assert(sample.x>=0&&sample.x<=width&&sample.y>=0&&sample.y<=height);
        }
    }
    puts("Storybook rays: rotation, mask UV and intro/outro bounds passed at four window sizes");
    for(float seconds:{0.f,1.f,15.9f,16.f,16.1f,60.f,3600.f,86400.f}) {
        const int first=first_confetti_birth(seconds),last=int(seconds*2.2f);
        assert(last-first+1<=60);
        if(seconds<=16.f)assert(first==-24);
        else {
            assert(first>=0&&first==int((seconds-16.f)*2.2f));
            // Even the slowest initial particle, starting at the very top,
            // is outside the viewport. No visible particle is being culled.
            assert(seconds*70-37>design_height);
        }
    }
    puts("Storybook confetti: bounded live births through 24 hours passed");
}
