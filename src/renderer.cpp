#include "renderer.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

// Dragging turns the sculpture: anywhere right of the text column on wide
// screens, anywhere but a link on narrow ones. On touch screens the page
// still scrolls vertically, so a sideways swipe turns it. The listeners add
// the drag, in CSS pixels, to the first two floats at input, which the
// renderer consumes, and keep the scroll in the third.
EM_JS(void, listen_to_page_script, (float *input), {
  var base = input >> 2;
  var frame = document.querySelector(".frame");
  if (!frame) {
    return;
  }
  var dragging = null;
  var last_x = 0;
  var last_y = 0;
  var turnable = function(event) {
    if (event.target.closest && event.target.closest("a")) {
      return false;
    }
    var column = document.querySelector(".terminal");
    if (!column || column.getBoundingClientRect().right > 0.75 * innerWidth) {
      return true;
    }
    return event.clientX > column.getBoundingClientRect().right;
  };
  frame.addEventListener(
      "pointerdown", function(event) {
        if (event.button != 0 || !turnable(event)) {
          return;
        }
        dragging = event.pointerId;
        last_x = event.clientX;
        last_y = event.clientY;
        frame.setPointerCapture(dragging);
        frame.style.cursor = "grabbing";
        frame.style.userSelect = "none";
      });
  frame.addEventListener(
      "pointermove", function(event) {
        if (dragging == event.pointerId) {
          HEAPF32[base] += event.clientX - last_x;
          HEAPF32[base + 1] += event.clientY - last_y;
          last_x = event.clientX;
          last_y = event.clientY;
        } else if (event.pointerType == "mouse") {
          frame.style.cursor = turnable(event) ? "grab" : "";
        }
      });
  var release = function(event) {
    if (dragging == event.pointerId) {
      dragging = null;
      frame.style.cursor = "";
      frame.style.userSelect = "";
    }
  };
  frame.addEventListener("pointerup", release);
  frame.addEventListener("pointercancel", release);
  frame.addEventListener(
      "scroll",
      function() { HEAPF32[base + 2] = frame.scrollTop / Math.max(frame.clientHeight, 1); },
      {passive : true});
});

namespace {

constexpr auto canvas_selector = "#background";
constexpr double maximum_device_pixel_ratio = 2.0;
constexpr double maximum_canvas_dimension = 8192.0;
// Enough for a 5K display at its full device pixel ratio.
constexpr double maximum_pixel_count = 16'000'000.0;
constexpr double maximum_frame_step = 0.1;
constexpr int resize_settle_delay_milliseconds = 160;
constexpr float default_text_edge = 0.40F;
// Each visit starts somewhere within this many seconds of the scene's motion.
constexpr double maximum_start_offset_seconds = 900.0;

// The right edge of the left-aligned text column as a fraction of the canvas width.
[[nodiscard]] float measure_text_edge() {
  const auto edge = EM_ASM_DOUBLE({
    var canvas = document.querySelector("#background");
    var column = document.querySelector(".terminal");
    if (!canvas || !column) {
      return NaN;
    }
    var canvas_rect = canvas.getBoundingClientRect();
    var column_rect = column.getBoundingClientRect();
    return (column_rect.right - canvas_rect.left) / canvas_rect.width;
  });
  if (!std::isfinite(edge)) {
    emscripten_log(EM_LOG_WARN, "Unable to measure the text column");
    return default_text_edge;
  }
  return std::clamp(static_cast<float>(edge), 0.0F, 1.0F);
}

[[nodiscard]] double random_start_offset() {
  const auto bits = static_cast<std::uint32_t>(EM_ASM_INT({
    var value = new Uint32Array(1);
    if (window.crypto && window.crypto.getRandomValues) {
      window.crypto.getRandomValues(value);
      return value[0] | 0;
    }
    return (Math.random() * 4294967296) | 0;
  }));
  return static_cast<double>(bits) / 4294967296.0 * maximum_start_offset_seconds;
}

// The scene's time. It advances by real elapsed time, but a long pause, such as
// a hidden tab, moves it by at most one frame step.
class SceneClock final {
public:
  explicit SceneClock(double start_seconds) : seconds_{start_seconds} {}

  void advance(double elapsed_seconds) {
    seconds_ += std::clamp(elapsed_seconds, 0.0, maximum_frame_step);
  }

  [[nodiscard]] GLfloat seconds() const { return static_cast<GLfloat>(seconds_); }

private:
  double seconds_;
};

// What the page tells the renderer. Page script writes it in place: the drag
// since the renderer last looked, in CSS pixels, and how far the page has
// scrolled, in viewport heights.
struct PageInput final {
  float drag_x;
  float drag_y;
  float scroll;
};

// The page script addresses the fields as three consecutive floats.
static_assert(std::is_standard_layout_v<PageInput>);
static_assert(offsetof(PageInput, drag_y) == sizeof(float) &&
              offsetof(PageInput, scroll) == 2 * sizeof(float));

PageInput page_input{};

void listen_to_page(PageInput &input) { listen_to_page_script(&input.drag_x); }

// A unit quaternion, w + xi + yj + zk, for the sculpture's orientation. Turns
// compose by multiplication, so any orientation is reachable and none is
// special.
struct Quaternion final {
  float w{1.0F};
  float x{};
  float y{};
  float z{};

  // The turn by |angle| radians about the axis along angle.
  [[nodiscard]] static Quaternion turn(float angle_x, float angle_y, float angle_z) {
    const auto angle = std::sqrt(angle_x * angle_x + angle_y * angle_y + angle_z * angle_z);
    if (angle < 0.000001F) {
      return {};
    }
    const auto scale = std::sin(0.5F * angle) / angle;
    return {.w = std::cos(0.5F * angle),
            .x = angle_x * scale,
            .y = angle_y * scale,
            .z = angle_z * scale};
  }

  friend Quaternion operator*(Quaternion a, Quaternion b) {
    return {.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
  }

  [[nodiscard]] float dot(Quaternion other) const {
    return w * other.w + x * other.x + y * other.y + z * other.z;
  }

  [[nodiscard]] Quaternion normalized() const {
    const auto length = std::sqrt(dot(*this));
    return {.w = w / length, .x = x / length, .y = y / length, .z = z / length};
  }

  // Part of the way toward another orientation, along the shorter arc.
  [[nodiscard]] Quaternion toward(Quaternion target, float fraction) const {
    const auto sign = dot(target) < 0.0F ? -1.0F : 1.0F;
    return Quaternion{.w = w + (sign * target.w - w) * fraction,
                      .x = x + (sign * target.x - x) * fraction,
                      .y = y + (sign * target.y - y) * fraction,
                      .z = z + (sign * target.z - z) * fraction}
        .normalized();
  }

  // The rotation matrix, column by column, as GLSL expects it.
  [[nodiscard]] std::array<GLfloat, 9> matrix() const {
    return {1.0F - 2.0F * (y * y + z * z), 2.0F * (x * y + w * z),
            2.0F * (x * z - w * y),        2.0F * (x * y - w * z),
            1.0F - 2.0F * (x * x + z * z), 2.0F * (y * z + w * x),
            2.0F * (x * z + w * y),        2.0F * (y * z - w * x),
            1.0F - 2.0F * (x * x + y * y)};
  }
};

// Radians the sculpture turns per CSS pixel of drag.
constexpr float turn_per_pixel = 0.007F;

// The view the renderer draws. A drag turns the target orientation about the
// view's axes: sideways about the vertical, up and down about the horizontal.
// The drawn orientation eases toward it, so a turn glides to a stop.
struct View final {
  Quaternion orientation =
      Quaternion::turn(0.38F, 0.0F, 0.0F) * Quaternion::turn(0.0F, -0.6F, 0.0F);
  Quaternion target = orientation;
  GLfloat scroll{};

  // Takes the page's drag and scroll and reports whether anything visibly changed.
  bool follow(PageInput &input, double elapsed_seconds) {
    target = (Quaternion::turn(input.drag_y * turn_per_pixel, input.drag_x * turn_per_pixel, 0.0F) *
              target)
                 .normalized();
    input.drag_x = 0.0F;
    input.drag_y = 0.0F;

    const auto blend = static_cast<float>(
        1.0 - std::exp(-12.0 * std::clamp(elapsed_seconds, 0.0, maximum_frame_step)));
    const auto before = orientation;
    orientation = orientation.toward(target, blend);
    const auto turned = 1.0F - std::abs(orientation.dot(before)) > 0.0000001F;
    const auto scrolled = std::abs(input.scroll - scroll) > 0.0001F;
    scroll = input.scroll;
    return turned || scrolled;
  }
};

class CanvasExtent final {
public:
  [[nodiscard]] static std::optional<CanvasExtent> measure() {
    double css_width{};
    double css_height{};
    if (emscripten_get_element_css_size(canvas_selector, &css_width, &css_height) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        !std::isfinite(css_width) || !std::isfinite(css_height) || css_width <= 0.0 ||
        css_height <= 0.0) {
      emscripten_log(EM_LOG_ERROR, "Unable to measure the background canvas");
      return std::nullopt;
    }

    const auto reported_ratio = emscripten_get_device_pixel_ratio();
    const auto device_pixel_ratio =
        std::isfinite(reported_ratio) ? std::clamp(reported_ratio, 1.0, maximum_device_pixel_ratio)
                                      : 1.0;
    const auto maximum_css_dimension = maximum_canvas_dimension / device_pixel_ratio;
    auto pixel_width = std::min(css_width, maximum_css_dimension) * device_pixel_ratio;
    auto pixel_height = std::min(css_height, maximum_css_dimension) * device_pixel_ratio;
    const auto pixel_count = pixel_width * pixel_height;
    if (pixel_count > maximum_pixel_count) {
      const auto render_scale = std::sqrt(maximum_pixel_count / pixel_count);
      pixel_width *= render_scale;
      pixel_height *= render_scale;
    }

    const auto width = std::max(1, static_cast<int>(std::lround(pixel_width)));
    const auto height = std::max(1, static_cast<int>(std::lround(pixel_height)));
    return CanvasExtent{Dimensions{.width = width, .height = height}};
  }

  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] GLfloat width_as_float() const { return static_cast<GLfloat>(width_); }
  [[nodiscard]] GLfloat height_as_float() const { return static_cast<GLfloat>(height_); }

  friend bool operator==(const CanvasExtent &, const CanvasExtent &) = default;

private:
  struct Dimensions final {
    int width;
    int height;
  };

  explicit CanvasExtent(Dimensions dimensions)
      : width_{dimensions.width}, height_{dimensions.height} {}

  int width_;
  int height_;
};

class WebGlContext final {
public:
  [[nodiscard]] static std::optional<WebGlContext> create() {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.alpha = false;
    attributes.depth = false;
    attributes.stencil = false;
    attributes.antialias = false;
    attributes.premultipliedAlpha = false;
    attributes.preserveDrawingBuffer = false;
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.enableExtensionsByDefault = false;
    attributes.desynchronized = true;

    const auto handle = emscripten_webgl_create_context(canvas_selector, &attributes);
    if (handle <= 0) {
      emscripten_log(EM_LOG_ERROR, "Unable to create a WebGL2 context");
      return std::nullopt;
    }
    if (emscripten_webgl_make_context_current(handle) != EMSCRIPTEN_RESULT_SUCCESS) {
      emscripten_log(EM_LOG_ERROR, "Unable to activate the WebGL2 context");
      emscripten_webgl_destroy_context(handle);
      return std::nullopt;
    }
    return WebGlContext{handle};
  }

  WebGlContext(const WebGlContext &) = delete;
  WebGlContext &operator=(const WebGlContext &) = delete;
  WebGlContext(WebGlContext &&other) noexcept : handle_{std::exchange(other.handle_, 0)} {}
  WebGlContext &operator=(WebGlContext &&) = delete;

  ~WebGlContext() {
    if (handle_ > 0) {
      emscripten_webgl_destroy_context(handle_);
    }
  }

private:
  explicit WebGlContext(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle) : handle_{handle} {}

  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle_;
};

enum class ShaderStage : std::uint8_t { vertex, fragment };

class Shader final {
public:
  [[nodiscard]] static std::optional<Shader> compile(ShaderStage stage, const char *source) {
    const GLenum type = stage == ShaderStage::vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    const auto handle = glCreateShader(type);
    if (handle == 0U) {
      emscripten_log(EM_LOG_ERROR, "Unable to allocate a shader");
      return std::nullopt;
    }

    glShaderSource(handle, 1, &source, nullptr);
    glCompileShader(handle);
    GLint compiled{};
    glGetShaderiv(handle, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
      std::array<GLchar, 2048> message{};
      glGetShaderInfoLog(handle, static_cast<GLsizei>(message.size()), nullptr, message.data());
      emscripten_log(EM_LOG_ERROR, "Shader compilation failed: %s", message.data());
      glDeleteShader(handle);
      return std::nullopt;
    }
    return Shader{handle};
  }

  Shader(const Shader &) = delete;
  Shader &operator=(const Shader &) = delete;
  Shader(Shader &&other) noexcept : handle_{std::exchange(other.handle_, 0U)} {}
  Shader &operator=(Shader &&) = delete;

  ~Shader() {
    if (handle_ != 0U) {
      glDeleteShader(handle_);
    }
  }

  [[nodiscard]] GLuint handle() const { return handle_; }

private:
  explicit Shader(GLuint handle) : handle_{handle} {}

  GLuint handle_;
};

class Pipeline final {
public:
  [[nodiscard]] static std::optional<Pipeline> create() {
    auto vertex_shader = Shader::compile(ShaderStage::vertex, portfolio::shaders::vertex);
    if (!vertex_shader) {
      return std::nullopt;
    }
    auto fragment_shader = Shader::compile(ShaderStage::fragment, portfolio::shaders::fragment);
    if (!fragment_shader) {
      return std::nullopt;
    }

    const auto program = glCreateProgram();
    if (program == 0U) {
      emscripten_log(EM_LOG_ERROR, "Unable to allocate the shader program");
      return std::nullopt;
    }
    glAttachShader(program, vertex_shader->handle());
    glAttachShader(program, fragment_shader->handle());
    glLinkProgram(program);

    GLint linked{};
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      std::array<GLchar, 2048> message{};
      glGetProgramInfoLog(program, static_cast<GLsizei>(message.size()), nullptr, message.data());
      emscripten_log(EM_LOG_ERROR, "Shader link failed: %s", message.data());
      glDeleteProgram(program);
      return std::nullopt;
    }

    const Uniforms uniforms{
        .resolution = glGetUniformLocation(program, "u_resolution"),
        .time = glGetUniformLocation(program, "u_time"),
        .text_edge = glGetUniformLocation(program, "u_text_edge"),
        .orientation = glGetUniformLocation(program, "u_orientation"),
        .scroll = glGetUniformLocation(program, "u_scroll"),
    };
    if (!uniforms.valid()) {
      emscripten_log(EM_LOG_ERROR, "The shader program is missing required uniforms");
      glDeleteProgram(program);
      return std::nullopt;
    }

    GLuint vertex_array{};
    glGenVertexArrays(1, &vertex_array);
    if (vertex_array == 0U) {
      emscripten_log(EM_LOG_ERROR, "Unable to allocate the fullscreen vertex array");
      glDeleteProgram(program);
      return std::nullopt;
    }

    // This is the renderer's only pipeline, so its program and vertex array stay
    // bound. Blending, culling, depth, and stencil tests start disabled. The
    // resolution and layout uniforms are set before the first draw.
    glUseProgram(program);
    glBindVertexArray(vertex_array);
    return Pipeline{Handles{.program = program, .vertex_array = vertex_array}, uniforms};
  }

  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;
  Pipeline(Pipeline &&other) noexcept
      : program_{std::exchange(other.program_, 0U)},
        vertex_array_{std::exchange(other.vertex_array_, 0U)}, uniforms_{other.uniforms_} {}
  Pipeline &operator=(Pipeline &&) = delete;

  ~Pipeline() {
    if (vertex_array_ != 0U) {
      glDeleteVertexArrays(1, &vertex_array_);
    }
    if (program_ != 0U) {
      glDeleteProgram(program_);
    }
  }

  void set_layout(const CanvasExtent &extent, float text_edge) const {
    glUniform2f(uniforms_.resolution, extent.width_as_float(), extent.height_as_float());
    glUniform1f(uniforms_.text_edge, text_edge);
  }

  void draw(const SceneClock &clock, const View &view) const {
    glUniform1f(uniforms_.time, clock.seconds());
    const auto orientation = view.orientation.matrix();
    glUniformMatrix3fv(uniforms_.orientation, 1, GL_FALSE, orientation.data());
    glUniform1f(uniforms_.scroll, view.scroll);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

private:
  struct Handles final {
    GLuint program;
    GLuint vertex_array;
  };

  struct Uniforms final {
    GLint resolution;
    GLint time;
    GLint text_edge;
    GLint orientation;
    GLint scroll;

    [[nodiscard]] bool valid() const {
      return resolution >= 0 && time >= 0 && text_edge >= 0 && orientation >= 0 && scroll >= 0;
    }
  };

  Pipeline(Handles handles, Uniforms uniforms)
      : program_{handles.program}, vertex_array_{handles.vertex_array}, uniforms_{uniforms} {}

  GLuint program_;
  GLuint vertex_array_;
  Uniforms uniforms_;
};

enum class MotionMode : std::uint8_t { animated, reduced };
enum class ResizeResult : std::uint8_t { unchanged, applied, failed };

[[nodiscard]] MotionMode preferred_motion_mode() {
  const auto reduced = EM_ASM_INT({
    return window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  });
  return reduced != 0 ? MotionMode::reduced : MotionMode::animated;
}

class Renderer final {
public:
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  Renderer(Renderer &&) noexcept = default;
  Renderer &operator=(Renderer &&) = delete;
  ~Renderer() = default;

  [[nodiscard]] static bool launch() {
    static std::optional<Renderer> active_renderer;
    if (active_renderer) {
      emscripten_log(EM_LOG_ERROR, "The renderer is already running");
      return false;
    }

    auto renderer = create();
    if (!renderer) {
      return false;
    }
    active_renderer.emplace(std::move(*renderer));
    if (!active_renderer->start()) {
      active_renderer.reset();
      return false;
    }
    return true;
  }

private:
  [[nodiscard]] static std::optional<Renderer> create() {
    auto context = WebGlContext::create();
    if (!context) {
      return std::nullopt;
    }
    const auto extent = CanvasExtent::measure();
    if (!extent) {
      return std::nullopt;
    }
    auto pipeline = Pipeline::create();
    if (!pipeline) {
      return std::nullopt;
    }
    return Renderer{std::move(*context), std::move(*pipeline), SceneClock{random_start_offset()},
                    *extent, preferred_motion_mode()};
  }

  [[nodiscard]] bool start() {
    if (!apply_extent(extent_)) {
      return false;
    }
    if (emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, on_resize) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
      emscripten_log(EM_LOG_ERROR, "Unable to register the canvas resize callback");
      return false;
    }

    listen_to_page(page_input);
    pipeline_.draw(clock_, view_);
    // The loop runs even when motion is reduced, so the visitor can still turn
    // the sculpture, but then it only draws when the view changes.
    last_animation_timestamp_ = emscripten_get_now();
    emscripten_request_animation_frame_loop(on_animation_frame, this);
    return true;
  }

  Renderer(WebGlContext context, Pipeline pipeline, SceneClock clock, CanvasExtent extent,
           MotionMode motion_mode)
      : context_{std::move(context)}, pipeline_{std::move(pipeline)}, clock_{clock},
        extent_{extent}, motion_mode_{motion_mode} {}

  [[nodiscard]] bool apply_extent(const CanvasExtent &extent) {
    if (emscripten_set_canvas_element_size(canvas_selector, extent.width(), extent.height()) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
      emscripten_log(EM_LOG_ERROR, "Unable to resize the background canvas");
      return false;
    }
    glViewport(0, 0, extent.width(), extent.height());
    // The text column only moves when the viewport, and so the canvas, resizes.
    pipeline_.set_layout(extent, measure_text_edge());
    extent_ = extent;
    return true;
  }

  [[nodiscard]] ResizeResult resize() {
    const auto next_extent = CanvasExtent::measure();
    if (!next_extent) {
      return ResizeResult::failed;
    }
    if (*next_extent == extent_) {
      return ResizeResult::unchanged;
    }
    return apply_extent(*next_extent) ? ResizeResult::applied : ResizeResult::failed;
  }

  static EM_BOOL on_animation_frame(double timestamp, void *user_data) {
    auto &renderer = *static_cast<Renderer *>(user_data);
    const auto elapsed_seconds = (timestamp - renderer.last_animation_timestamp_) / 1000.0;
    renderer.last_animation_timestamp_ = timestamp;
    const auto turned = renderer.view_.follow(page_input, elapsed_seconds);
    if (renderer.motion_mode_ == MotionMode::animated) {
      renderer.clock_.advance(elapsed_seconds);
    } else if (!turned) {
      return EM_TRUE;
    }
    renderer.pipeline_.draw(renderer.clock_, renderer.view_);
    return EM_TRUE;
  }

  static EM_BOOL on_resize(int, const EmscriptenUiEvent *, void *user_data) {
    auto &renderer = *static_cast<Renderer *>(user_data);
    renderer.resize_after_timestamp_ =
        emscripten_get_now() + static_cast<double>(resize_settle_delay_milliseconds);
    if (!renderer.resize_scheduled_) {
      renderer.resize_scheduled_ = true;
      emscripten_async_call(on_deferred_resize, &renderer, resize_settle_delay_milliseconds);
    }
    return EM_TRUE;
  }

  static void on_deferred_resize(void *user_data) {
    auto &renderer = *static_cast<Renderer *>(user_data);
    const auto remaining = renderer.resize_after_timestamp_ - emscripten_get_now();
    if (remaining > 0.0) {
      const auto delay = std::max(1, static_cast<int>(std::ceil(remaining)));
      emscripten_async_call(on_deferred_resize, &renderer, delay);
      return;
    }

    renderer.resize_scheduled_ = false;
    if (renderer.resize() == ResizeResult::applied &&
        renderer.motion_mode_ == MotionMode::reduced) {
      renderer.pipeline_.draw(renderer.clock_, renderer.view_);
    }
  }

  WebGlContext context_;
  Pipeline pipeline_;
  SceneClock clock_;
  View view_;
  CanvasExtent extent_;
  MotionMode motion_mode_;
  double last_animation_timestamp_{};
  double resize_after_timestamp_{};
  bool resize_scheduled_{};
};

} // namespace

namespace portfolio {

int run() { return Renderer::launch() ? 0 : 1; }

} // namespace portfolio
