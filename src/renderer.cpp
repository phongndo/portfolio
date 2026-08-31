#include "renderer.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

namespace {

constexpr auto canvas_selector = "#background";
constexpr float reduced_motion_frame = 19.0F;
constexpr double maximum_device_pixel_ratio = 2.0;
constexpr double maximum_canvas_dimension = 8192.0;
constexpr double maximum_pixel_count = 5'000'000.0;
constexpr std::uint32_t fallback_random_state = 0x6D2B79F5U;
constexpr std::size_t maximum_center_count = 7;
constexpr std::size_t seed_value_count = 56;
constexpr float placement_left = 0.32F;
constexpr float placement_right = 0.92F;
constexpr float placement_top = 0.08F;
constexpr float placement_bottom = 0.92F;
constexpr float minimum_center_distance = 0.21F;
constexpr float minimum_center_distance_squared = minimum_center_distance * minimum_center_distance;

struct NormalizedPoint final {
  float x;
  float y;
};

constexpr std::array fallback_positions{
    NormalizedPoint{0.36F, 0.18F}, NormalizedPoint{0.65F, 0.15F}, NormalizedPoint{0.90F, 0.24F},
    NormalizedPoint{0.40F, 0.58F}, NormalizedPoint{0.69F, 0.52F}, NormalizedPoint{0.88F, 0.80F},
    NormalizedPoint{0.55F, 0.85F},
};

// A discretized normal distribution centered on five (sigma 1.25). About 80%
// of visits receive four to six centers, while a single center occurs on about
// one in every 500 visits.
constexpr std::array center_count_cumulative_probabilities{
    0.00194628F, 0.02022834F, 0.11077998F, 0.34727336F, 0.67295498F, 0.90944836F,
};

constexpr bool valid_fallback_positions() {
  for (std::size_t index = 0; index < fallback_positions.size(); ++index) {
    const auto point = fallback_positions[index];
    if (point.x < placement_left || point.x > placement_right || point.y < placement_top ||
        point.y > placement_bottom) {
      return false;
    }

    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto delta_x = point.x - fallback_positions[previous].x;
      const auto delta_y = point.y - fallback_positions[previous].y;
      if (delta_x * delta_x + delta_y * delta_y < minimum_center_distance_squared) {
        return false;
      }
    }
  }
  return true;
}

static_assert(seed_value_count % 4U == 0U);
static_assert(maximum_center_count * 2U <= seed_value_count);
static_assert(fallback_positions.size() == maximum_center_count);
static_assert(std::ranges::is_sorted(center_count_cumulative_probabilities));
static_assert(valid_fallback_positions());

class Random final {
public:
  explicit Random(std::uint32_t state) : state_{state == 0U ? fallback_random_state : state} {}

  [[nodiscard]] float unit() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    return static_cast<float>(state_ >> 8U) * (1.0F / 16777216.0F);
  }

private:
  std::uint32_t state_;
};

[[nodiscard]] std::uint32_t browser_random_state() {
  return static_cast<std::uint32_t>(EM_ASM_INT({
    var value = new Uint32Array(1);
    if (window.crypto && window.crypto.getRandomValues) {
      window.crypto.getRandomValues(value);
      return value[0] | 0;
    }
    return (Math.random() * 4294967296) | 0;
  }));
}

class FieldSeed final {
public:
  static constexpr GLsizei vector_count = static_cast<GLsizei>(seed_value_count / 4U);

  [[nodiscard]] static FieldSeed random() {
    Random random{browser_random_state()};
    const auto center_count = sample_center_count(random);
    Values values;
    for (auto &value : values) {
      value = random.unit();
    }

    std::array<NormalizedPoint, maximum_center_count> positions{};
    auto layout_is_complete = true;
    for (std::size_t index = 0; index < center_count; ++index) {
      const auto position = sample_position(random, positions, index);
      if (!position) {
        layout_is_complete = false;
        break;
      }
      positions[index] = *position;
    }

    if (!layout_is_complete) {
      positions = fallback_positions;
    }
    for (std::size_t index = 0; index < center_count; ++index) {
      values[index * 2U] = positions[index].x;
      values[index * 2U + 1U] = positions[index].y;
    }
    return FieldSeed{values, center_count};
  }

  [[nodiscard]] const GLfloat *data() const { return values_.data(); }
  [[nodiscard]] GLint center_count() const { return static_cast<GLint>(center_count_); }

private:
  using Values = std::array<GLfloat, seed_value_count>;
  static constexpr int maximum_placement_attempts = 96;

  FieldSeed(Values values, std::size_t center_count)
      : values_{values}, center_count_{center_count} {}

  [[nodiscard]] static std::size_t sample_center_count(Random &random) {
    const auto sample = random.unit();
    for (std::size_t index = 0; index < center_count_cumulative_probabilities.size(); ++index) {
      if (sample < center_count_cumulative_probabilities[index]) {
        return index + 1U;
      }
    }
    return maximum_center_count;
  }

  [[nodiscard]] static std::optional<NormalizedPoint>
  sample_position(Random &random,
                  const std::array<NormalizedPoint, maximum_center_count> &positions,
                  std::size_t populated_count) {
    for (auto attempt = 0; attempt < maximum_placement_attempts; ++attempt) {
      const NormalizedPoint candidate{
          .x = placement_left + (placement_right - placement_left) * random.unit(),
          .y = placement_top + (placement_bottom - placement_top) * random.unit(),
      };
      auto separated = true;
      for (std::size_t previous = 0; previous < populated_count; ++previous) {
        const auto delta_x = candidate.x - positions[previous].x;
        const auto delta_y = candidate.y - positions[previous].y;
        if (delta_x * delta_x + delta_y * delta_y < minimum_center_distance_squared) {
          separated = false;
          break;
        }
      }
      if (separated) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  Values values_;
  std::size_t center_count_;
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
    const auto type = stage == ShaderStage::vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
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
  [[nodiscard]] static std::optional<Pipeline> create(const FieldSeed &seed,
                                                      const CanvasExtent &extent) {
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
        .time = glGetUniformLocation(program, "u_time"),
        .resolution = glGetUniformLocation(program, "u_resolution"),
        .seed = glGetUniformLocation(program, "u_seed[0]"),
        .center_count = glGetUniformLocation(program, "u_center_count"),
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

    Pipeline pipeline{
        Handles{.program = program, .vertex_array = vertex_array},
        uniforms,
    };
    pipeline.bind();
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glUniform1f(uniforms.time, 0.0F);
    glUniform2f(uniforms.resolution, extent.width_as_float(), extent.height_as_float());
    glUniform4fv(uniforms.seed, FieldSeed::vector_count, seed.data());
    glUniform1i(uniforms.center_count, seed.center_count());
    return pipeline;
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

  void set_resolution(const CanvasExtent &extent) const {
    bind();
    glUniform2f(uniforms_.resolution, extent.width_as_float(), extent.height_as_float());
  }

  void draw(float time) const {
    bind();
    glUniform1f(uniforms_.time, time);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

private:
  struct Handles final {
    GLuint program;
    GLuint vertex_array;
  };

  struct Uniforms final {
    GLint time;
    GLint resolution;
    GLint seed;
    GLint center_count;

    [[nodiscard]] bool valid() const {
      return time >= 0 && resolution >= 0 && seed >= 0 && center_count >= 0;
    }
  };

  Pipeline(Handles handles, Uniforms uniforms)
      : program_{handles.program}, vertex_array_{handles.vertex_array}, uniforms_{uniforms} {}

  void bind() const {
    glUseProgram(program_);
    glBindVertexArray(vertex_array_);
  }

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
    const auto seed = FieldSeed::random();
    auto pipeline = Pipeline::create(seed, *extent);
    if (!pipeline) {
      return std::nullopt;
    }
    return Renderer{std::move(*context), std::move(*pipeline), *extent, preferred_motion_mode()};
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

    if (motion_mode_ == MotionMode::reduced) {
      pipeline_.draw(reduced_motion_frame);
      return true;
    }

    animation_start_ = emscripten_get_now();
    pipeline_.draw(0.0F);
    emscripten_request_animation_frame_loop(on_animation_frame, this);
    return true;
  }

private:
  Renderer(WebGlContext context, Pipeline pipeline, CanvasExtent extent, MotionMode motion_mode)
      : context_{std::move(context)}, pipeline_{std::move(pipeline)}, extent_{extent},
        motion_mode_{motion_mode} {}

  [[nodiscard]] bool apply_extent(const CanvasExtent &extent) {
    if (emscripten_set_canvas_element_size(canvas_selector, extent.width(), extent.height()) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
      emscripten_log(EM_LOG_ERROR, "Unable to resize the background canvas");
      return false;
    }
    glViewport(0, 0, extent.width(), extent.height());
    pipeline_.set_resolution(extent);
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
    renderer.pipeline_.draw(static_cast<float>((timestamp - renderer.animation_start_) / 1000.0));
    return EM_TRUE;
  }

  static EM_BOOL on_resize(int, const EmscriptenUiEvent *, void *user_data) {
    auto &renderer = *static_cast<Renderer *>(user_data);
    if (renderer.resize() == ResizeResult::applied &&
        renderer.motion_mode_ == MotionMode::reduced) {
      renderer.pipeline_.draw(reduced_motion_frame);
    }
    return EM_TRUE;
  }

  WebGlContext context_;
  Pipeline pipeline_;
  CanvasExtent extent_;
  MotionMode motion_mode_;
  double animation_start_{};
};

} // namespace

namespace portfolio {

int run() { return Renderer::launch() ? 0 : 1; }

} // namespace portfolio
