#pragma once

#include <RmlUi/Core/Mesh.h>
#include "storybook_geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace mp6::ui::storybook_sky_mesh {

namespace detail {

template <class Container>
inline void reserve_append(Container& values, std::size_t count) {
    const std::size_t required = values.size() + count;
    if (required > values.capacity()) {
        const std::size_t capacity = values.capacity();
        const std::size_t grown = capacity ? capacity * 2 : std::size_t(64);
        values.reserve(std::max(required, grown));
    }
}

inline Rml::ColourbPremultiplied opaque_white() {
    return Rml::ColourbPremultiplied{255, 255, 255, 255};
}

inline Rml::ColourbPremultiplied transparent_white() {
    // Match Colourb(255,255,255,0).ToPremultiplied(): RGB is zero in the
    // premultiplied representation, while retaining the transparent fringe's
    // white source colour conceptually.
    return Rml::Colourb(255, 255, 255, 0).ToPremultiplied();
}

} // namespace detail

// Append a smooth, one-pixel-fringed polygon in local physical-pixel space.
// The palette index is carried in tex_coord.x; the sky palette shader supplies
// the visible colour, so geometry can use opaque/transparent white vertices.
inline void append_polygon(Rml::Mesh& mesh, const Rml::Vector2f* points, int count, unsigned paletteIndex) {
    if (!points || count < 3)
        return;

    float area = 0;
    for (int i = 0; i < count; ++i) {
        const auto a = points[i], b = points[(i + 1) % count];
        area += a.x * b.y - b.x * a.y;
    }
    const auto point = [&](int i) {
        return points[area < 0 ? count - 1 - i : i];
    };

    const int base = int(mesh.vertices.size());
    detail::reserve_append(mesh.vertices, std::size_t(1 + 2 * count));
    detail::reserve_append(mesh.indices, std::size_t(9 * count));

    const Rml::Vector2f uv{float(paletteIndex), 0};
    const auto solid = detail::opaque_white();
    const auto clear = detail::transparent_white();
    Rml::Vector2f center{0, 0};
    for (int i = 0; i < count; ++i)
        center += point(i);
    center /= float(count);
    mesh.vertices.push_back({center, solid, uv});

    // Sky coordinates are already physical pixels, so the normal metric is
    // exactly one in both axes. Keep the same normalization and miter extent
    // expressions as smooth_polygon's one-pixel coverage fringe.
    const Rml::Vector2f metric{1, 1};
    const auto normal = [&](Rml::Vector2f d) {
        d = {d.x * metric.x, d.y * metric.y};
        const float length = std::max(.0001f, std::hypot(d.x, d.y));
        return Rml::Vector2f{d.y / length, -d.x / length};
    };
    for (int i = 0; i < count; ++i) {
        const auto p = point(i), prev = point((i + count - 1) % count), next = point((i + 1) % count);
        const auto a = normal(p - prev), b = normal(next - p);
        auto n = a + b;
        const float length = std::max(.0001f, std::hypot(n.x, n.y));
        n /= length;
        const float extent = std::min(2.f, 1 / std::max(.5f, n.x * b.x + n.y * b.y));
        mesh.vertices.push_back({p, solid, uv});
        mesh.vertices.push_back({{p.x + n.x * extent / metric.x, p.y + n.y * extent / metric.y}, clear, uv});
    }
    for (int i = 0; i < count; ++i) {
        const int a = 1 + 2 * i, b = 1 + 2 * ((i + 1) % count);
        mesh.indices.insert(mesh.indices.end(), {base, base + a, base + b,
                                                  base + a, base + a + 1, base + b + 1,
                                                  base + a, base + b + 1, base + b});
    }
}

inline void append_ellipse(Rml::Mesh& mesh, float x, float y, float rx, float ry, int count, unsigned paletteIndex) {
    std::array<Rml::Vector2f, 48> points{};
    const auto& ring = storybook_geometry::ellipse_ring(count);
    for (int i = 0; i < count; ++i) {
        const auto p = storybook_geometry::ellipse_point(ring, count, i, x, y, rx, ry);
        points[std::size_t(i)] = {p.x, p.y};
    }
    append_polygon(mesh, points.data(), count, paletteIndex);
}

inline void append_star(Rml::Mesh& mesh, float size, unsigned paletteIndex) {
    std::array<Rml::Vector2f, 10> points{};
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + i * .62831853f;
        const float r = size * (i % 2 ? .22f : .5f);
        points[std::size_t(i)] = {std::cos(a) * r, std::sin(a) * r};
    }
    append_polygon(mesh, points.data(), int(points.size()), paletteIndex);
}

inline void append_glow(Rml::Mesh& mesh, float rx, float ry, unsigned paletteIndex) {
    const int base = int(mesh.vertices.size());
    detail::reserve_append(mesh.vertices, 50);
    detail::reserve_append(mesh.indices, 144);

    const Rml::Vector2f uv{float(paletteIndex), 0};
    mesh.vertices.push_back({{0, 0}, detail::opaque_white(), uv});
    const auto clear = detail::transparent_white();
    const auto& ring = storybook_geometry::ellipse_ring(48);
    for (int i = 0; i <= 48; ++i) {
        const auto p = storybook_geometry::ellipse_point(ring, 48, i, 0, 0, rx, ry);
        mesh.vertices.push_back({{p.x, p.y}, clear, uv});
        if (i)
            mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
    }
}

} // namespace mp6::ui::storybook_sky_mesh
