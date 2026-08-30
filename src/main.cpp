#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>

#include <GLES3/gl3.h>
#include <emscripten/html5.h>

namespace {

constexpr auto canvas = "#background";

void draw() {
  glClearColor(0.035F, 0.039F, 0.043F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
}

void resize() {
  double css_width{};
  double css_height{};
  emscripten_get_element_css_size(canvas, &css_width, &css_height);

  const auto pixel_ratio = std::min(emscripten_get_device_pixel_ratio(), 2.0);
  const auto width = static_cast<int>(std::lround(css_width * pixel_ratio));
  const auto height = static_cast<int>(std::lround(css_height * pixel_ratio));

  emscripten_set_canvas_element_size(canvas, width, height);
  glViewport(0, 0, width, height);
  draw();
}

EM_BOOL on_resize(int, const EmscriptenUiEvent *, void *) {
  resize();
  return EM_TRUE;
}

} // namespace

int main() {
  EmscriptenWebGLContextAttributes attributes;
  emscripten_webgl_init_context_attributes(&attributes);
  attributes.alpha = EM_FALSE;
  attributes.antialias = EM_FALSE;
  attributes.majorVersion = 2;

  const auto context = emscripten_webgl_create_context(canvas, &attributes);
  if (context <= 0) {
    return 1;
  }

  emscripten_webgl_make_context_current(context);
  emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_resize);
  resize();
}

#else

int main() {}

#endif
