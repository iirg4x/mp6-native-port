// Native fixture for the replayed RmlUi ElementText::OnRender implementation.
// The Python test injects the actual patched function body between the markers;
// this file supplies only small deterministic stand-ins for its dependencies.

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#define RMLUI_ZoneScoped ((void)0)

using FontFaceHandle = int;

struct Vector2f {
  float x = 0.0f;
  float y = 0.0f;
  Vector2f operator+(const Vector2f& rhs) const { return {x + rhs.x, y + rhs.y}; }
};

struct Vector2i {
  int x = 0;
  int y = 0;
  Vector2i() = default;
  Vector2i(int x, int y) : x(x), y(y) {}
  explicit Vector2i(const Vector2f& value) : x(static_cast<int>(value.x)), y(static_cast<int>(value.y)) {}
  Vector2i operator-(const Vector2i& rhs) const { return {x - rhs.x, y - rhs.y}; }
  Vector2i operator+(const Vector2i& rhs) const { return {x + rhs.x, y + rhs.y}; }
};

struct Rectanglei {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool valid = false;

  bool Valid() const { return valid; }
  static Rectanglei FromSize(const Vector2i& size) { return {0, 0, size.x, size.y, true}; }
  static Rectanglei FromCorners(const Vector2i& first, const Vector2i& second) {
    return {first.x, first.y, second.x, second.y, true};
  }
  bool Intersects(const Rectanglei& other) const {
    return right > other.left && left < other.right && bottom > other.top && top < other.bottom;
  }
};

struct FontMetrics {
  float ascent = 8.0f;
  float descent = 2.0f;
};

struct FontEngine {
  int GetVersion(FontFaceHandle) const { return 1; }
  FontMetrics GetFontMetrics(FontFaceHandle) const { return {}; }
};

struct Mesh {};

struct Geometry {
  enum class ReleaseMode { ClearMesh };

  Mesh Release(ReleaseMode) { return {}; }
  void Render(const Vector2f&, int) {}
  void Render(const Vector2f&) {}
};

template <typename T, typename... Args>
std::unique_ptr<T> MakeUnique(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

struct RenderManager {
  Rectanglei GetScissorRegion() const { return {}; }
  Vector2i GetViewport() const { return {100, 100}; }
  Geometry MakeGeometry(Mesh&&) { return {}; }
};

struct Context {
  RenderManager render_manager;
  RenderManager& GetRenderManager() { return render_manager; }
};

struct TransformState {
  bool GetTransform() const { return true; }
};

namespace Math {
int RoundUpToInteger(float value) { return static_cast<int>(std::ceil(value)); }
} // namespace Math

namespace Style {
enum class TextDecoration { None, Underline };
} // namespace Style

struct Line {
  Vector2f position{};
  int width = 0;
};

struct GeometryEntry {
  Geometry geometry;
  int texture = 0;
};

struct Colour {
  unsigned alpha_value = 255;
  unsigned alpha() const { return alpha_value; }
};

struct ElementText {
  float opacity = 0.0f;
  Colour colour{};
  int font_handle_version = 0;
  bool geometry_dirty = true;
  bool font_effects_dirty = true;
  int font_effects_handle = 0;
  Style::TextDecoration generated_decoration = Style::TextDecoration::None;
  Style::TextDecoration decoration_property = Style::TextDecoration::None;
  std::unique_ptr<Geometry> decoration;
  std::vector<Line> lines{{Vector2f{0.0f, 0.0f}, 10}};
  std::vector<GeometryEntry> geometry{{}};
  Context context;
  RenderManager render_manager;
  FontEngine font_engine;
  TransformState transform_state;
  FontFaceHandle font_face_handle = 1;
  int font_face_queries = 0;
  int geometry_generations = 0;

  Context* GetContext() { return &context; }
  RenderManager* GetRenderManager() { return &render_manager; }
  FontFaceHandle GetFontFaceHandle() {
    ++font_face_queries;
    return font_face_handle;
  }
  FontEngine* GetFontEngineInterface() { return &font_engine; }
  bool UpdateFontEffects() { return false; }
  void GenerateGeometry(RenderManager&, FontFaceHandle) {
    ++geometry_generations;
    geometry_dirty = false;
  }
  void GenerateDecoration(Mesh&, FontFaceHandle) {}
  Vector2f GetAbsoluteOffset() const { return {}; }
  TransformState* GetTransformState() { return &transform_state; }

  void OnRender() {
    // BEGIN ACTUAL_ELEMENT_TEXT_ON_RENDER
    // END ACTUAL_ELEMENT_TEXT_ON_RENDER
  }
};

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void hidden_text_preserves_dirty_state() {
  ElementText text;
  text.opacity = 0.0f;
  text.geometry_dirty = true;
  text.colour.alpha_value = 255;
  text.OnRender();
  require(text.geometry_dirty, "zero-opacity render cleared geometry_dirty");
  require(text.geometry_generations == 0, "zero-opacity render generated geometry");
  require(text.font_face_queries == 0, "zero-opacity render reached font work");

  text.opacity = 1.0f;
  text.OnRender();
  require(!text.geometry_dirty, "visible render did not consume dirty geometry");
  require(text.geometry_generations == 1, "visible render did not generate geometry");
  require(text.font_face_queries == 1, "visible render queried the font more than once");
}

void transparent_colour_does_not_skip_visible_text() {
  ElementText text;
  text.opacity = 1.0f;
  text.colour.alpha_value = 0;
  text.geometry_dirty = true;
  text.OnRender();
  require(text.geometry_generations == 1, "transparent colour incorrectly skipped visible text");
  require(!text.geometry_dirty, "transparent colour left visible geometry dirty");
}

int main() {
  hidden_text_preserves_dirty_state();
  transparent_colour_does_not_skip_visible_text();
  std::cout << "PASS RmlUi transparent text actual OnRender\n";
  return 0;
}
