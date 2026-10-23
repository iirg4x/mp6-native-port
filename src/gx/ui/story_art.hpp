// Native, resolution-independent storybook surfaces. Each element is a separate
// animation target; no menu, text, or book spread is baked into a screenshot.
#pragma once
#include <RmlUi/Core.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/RenderManager.h>
#include <cmath>
#include <algorithm>

namespace mp6::ui {
class StoryShape final : public Rml::Element {
public:
    explicit StoryShape(const Rml::String &tag) : Rml::Element(tag) {}
protected:
    void OnResize() override { mDirty = true; }
    void OnAttributeChange(const Rml::ElementAttributes &attributes) override
    {
        Rml::Element::OnAttributeChange(attributes);
        mDirty = true;
    }
    void OnPropertyChange(const Rml::PropertyIdSet &properties) override
    {
        Rml::Element::OnPropertyChange(properties);
        if (properties.Contains(Rml::PropertyId::Opacity)) mDirty = true;
    }
    void OnRender() override
    {
        if (mDirty) rebuild();
        mGeometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
    }
private:
    Rml::Geometry mGeometry;
    bool mDirty = true;
    void rebuild()
    {
        const auto size = GetBox().GetSize(Rml::BoxArea::Content);
        if (size.x <= 0 || size.y <= 0 || !GetRenderManager()) return;
        auto mesh = mGeometry.Release(Rml::Geometry::ReleaseMode::ClearMesh);
        const auto kind = GetAttribute<Rml::String>("kind", "page");
        const float opacity = GetComputedValues().opacity();
        auto color = [opacity](int r, int g, int b, float alpha = 1.f) {
            return Rml::Colourb(r, g, b, int(255 * alpha)).ToPremultiplied(opacity);
        };
        auto quad = [&](float x0, float y0, float x1, float y1,
                        Rml::ColourbPremultiplied a, Rml::ColourbPremultiplied b) {
            const int start = int(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(), {
                {{x0, y0}, a, {}}, {{x1, y0}, b, {}},
                {{x1, y1}, b, {}}, {{x0, y1}, a, {}}});
            mesh.indices.insert(mesh.indices.end(), {start,start+1,start+2,start,start+2,start+3});
        };
        if (kind == "page") {
            const bool right = GetAttribute<int>("right", 0) != 0;
            // Curved top and bottom contours, independent left/right meshes.
            // One-pixel transparent fringe keeps the outline smooth without MSAA.
            constexpr int slices = 100;
            for (int i = 0; i <= slices; ++i) {
                const float u = float(i) / slices;
                const float t = right ? 1.f-u : u;
                const float bow = std::sin(t * 3.14159265f);
                const float top = size.y * (0.036f - .030f * bow);
                const float bottom = size.y * (.985f - .022f * bow);
                const float crease = std::pow(t, 14.f);
                const float edge = std::pow(1.f-t, 16.f);
                auto paper = color(int(255 - 32*crease - 4*edge),
                                   int(253 - 34*crease - 6*edge),
                                   int(242 - 38*crease - 10*edge));
                const float x = u * size.x;
                mesh.vertices.insert(mesh.vertices.end(), {
                    {{x,top-1.f}, color(255,253,242,0),{}},
                    {{x,top+1.f},paper,{}},
                    {{x,bottom-1.f},paper,{}},
                    {{x,bottom+1.f},color(255,253,242,0),{}}});
                if (i) {
                    const int k = i*4;
                    for (int j=0;j<3;++j)
                        mesh.indices.insert(mesh.indices.end(), {k-4+j,k+j,k+j+1,k-4+j,k+j+1,k-3+j});
                }
            }
        } else if (kind == "zigzag") {
            const auto navy = color(7,15,104);
            constexpr int steps = 24;
            for (int i=0;i<steps;++i) {
                const int k=int(mesh.vertices.size());
                const float y=float(i)*size.y/steps, next=float(i+1)*size.y/steps;
                const float x=(i%2 ? .48f : .52f)*size.x;
                const float xn=(i%2 ? .52f : .48f)*size.x;
                mesh.vertices.insert(mesh.vertices.end(), {
                    {{x,y},navy,{}},{{size.x,y},navy,{}},
                    {{size.x,next},navy,{}},{{xn,next},navy,{}}});
                mesh.indices.insert(mesh.indices.end(),{k,k+1,k+2,k,k+2,k+3});
            }
        } else if (kind == "rays") {
            const Rml::Vector2f center{size.x*.14f,size.y*.14f};
            const float radius=std::max(size.x,size.y)*1.5f;
            for(int i=0;i<18;++i) {
                const float a=i*6.2831853f/18, b=a+.09f;
                const int k=int(mesh.vertices.size());
                mesh.vertices.insert(mesh.vertices.end(),{
                    {center,color(255,253,160,.65f),{}},
                    {{center.x+std::cos(a)*radius,center.y+std::sin(a)*radius},color(255,253,160,0),{}},
                    {{center.x+std::cos(b)*radius,center.y+std::sin(b)*radius},color(255,253,160,0),{}}});
                mesh.indices.insert(mesh.indices.end(),{k,k+1,k+2});
            }
        } else if (kind == "sun") {
            const Rml::Vector2f c{size.x/2,size.y/2};
            const float r=std::min(size.x,size.y)*.48f;
            auto triangle=[&](Rml::Vector2f a,Rml::Vector2f b,Rml::Vector2f d,Rml::ColourbPremultiplied tint) {
                const int k=int(mesh.vertices.size());
                mesh.vertices.insert(mesh.vertices.end(),{{a,tint,{}},{b,tint,{}},{d,tint,{}}});
                mesh.indices.insert(mesh.indices.end(),{k,k+1,k+2});
            };
            for(int i=0;i<14;++i) {
                const float a=i*6.2831853f/14;
                triangle({c.x+std::cos(a-.13f)*r*.69f,c.y+std::sin(a-.13f)*r*.69f},
                         {c.x+std::cos(a)*r,c.y+std::sin(a)*r},
                         {c.x+std::cos(a+.13f)*r*.69f,c.y+std::sin(a+.13f)*r*.69f},color(248,179,9));
            }
            for(int i=0;i<96;++i) {
                const float a=i*6.2831853f/96,b=(i+1)*6.2831853f/96;
                triangle(c,{c.x+std::cos(a)*r*.63f,c.y+std::sin(a)*r*.63f},
                    {c.x+std::cos(b)*r*.63f,c.y+std::sin(b)*r*.63f},color(249,188,14));
            }
            for(int i=0;i<96;++i) {
                const float t=float(i)/96,u=float(i+1)/96;
                const float ra=r*(.03f+.45f*t),rb=r*(.03f+.45f*u);
                const float aa=t*10,bb=u*10,w=r*.045f;
                const Rml::Vector2f p{c.x+std::cos(aa)*ra,c.y+std::sin(aa)*ra};
                const Rml::Vector2f q{c.x+std::cos(bb)*rb,c.y+std::sin(bb)*rb};
                const Rml::Vector2f n{-std::sin(aa)*w,std::cos(aa)*w};
                triangle(p+n,p-n,q+n,color(255,253,242));
                triangle(p-n,q-n,q+n,color(255,253,242));
            }
        } else if (kind == "photo") {
            mesh.vertices = {{{0,0},color(255,239,126),{}},{{size.x,0},color(255,216,62),{}},
                {{0,size.y},color(240,186,34),{}}};
            mesh.indices = {0,1,2};
        } else if (kind == "ribbon") {
            const float tip=std::min(size.y*.3f,size.x*.08f);
            quad(0,0,size.x-tip,size.y,color(244,23,11),color(233,14,8));
            const int k=int(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(),{{{size.x-tip,0},color(233,14,8),{}},
                {{size.x,size.y/2},color(222,15,8),{}},{{size.x-tip,size.y},color(233,14,8),{}}});
            mesh.indices.insert(mesh.indices.end(),{k,k+1,k+2});
        } else if (kind == "star") {
            const Rml::Vector2f c{size.x/2,size.y/2};
            const float r=std::min(size.x,size.y)*.48f;
            for(int i=0;i<10;++i) {
                const float a=-1.5707963f+i*.62831853f,b=a+.62831853f;
                const float ra=(i%2 ? .46f:1.f)*r, rb=(i%2 ? 1.f:.46f)*r;
                const int k=int(mesh.vertices.size());
                mesh.vertices.insert(mesh.vertices.end(),{
                    {c,color(255,240,95),{}},
                    {{c.x+std::cos(a)*ra,c.y+std::sin(a)*ra},color(255,204,30),{}},
                    {{c.x+std::cos(b)*rb,c.y+std::sin(b)*rb},color(255,224,48),{}}});
                mesh.indices.insert(mesh.indices.end(),{k,k+1,k+2});
            }
        } else {
            quad(0,0,size.x,size.y,color(255,235,62),color(255,216,18));
        }
        mGeometry = GetRenderManager()->MakeGeometry(std::move(mesh));
        mDirty = false;
    }
};
} // namespace mp6::ui
