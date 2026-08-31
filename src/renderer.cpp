#include "renderer.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

namespace {

constexpr auto canvas_selector = "#background";
constexpr double maximum_device_pixel_ratio = 2.0;
constexpr double maximum_canvas_dimension = 8192.0;
constexpr double maximum_pixel_count = 5'000'000.0;
constexpr double maximum_frame_step = 0.1;
constexpr int resize_settle_delay_milliseconds = 160;
constexpr float maximum_integration_step = 1.0F / 60.0F;
constexpr std::uint32_t fallback_random_state = 0x6D2B79F5U;
constexpr std::size_t maximum_center_count = 7U;
constexpr float placement_left = 0.32F;
constexpr float placement_right = 0.92F;
constexpr float placement_top = 0.08F;
constexpr float placement_bottom = 0.92F;
constexpr float minimum_center_distance = 0.21F;
constexpr float minimum_center_distance_squared = minimum_center_distance * minimum_center_distance;
constexpr float field_domain_scale = 1.08F;
constexpr float boundary_stiffness = 0.055F;
constexpr float interaction_radius = 0.24F;
constexpr float interaction_strength = 0.006F;
constexpr float maximum_drift_speed = 0.040F;
constexpr float tau = 6.283185307179586F;

struct NormalizedPoint final {
  float x;
  float y;
};

struct Bounds final {
  float minimum;
  float maximum;
};

constexpr Bounds horizontal_boundaries{.minimum = -0.76F, .maximum = 1.00F};
constexpr Bounds vertical_boundaries{.minimum = -1.00F, .maximum = 1.00F};

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

static_assert(fallback_positions.size() == maximum_center_count);
static_assert(center_count_cumulative_probabilities.size() + 1U == maximum_center_count);
static_assert(std::ranges::is_sorted(center_count_cumulative_probabilities));
static_assert(center_count_cumulative_probabilities.front() > 0.0F &&
              center_count_cumulative_probabilities.back() < 1.0F);
static_assert(valid_fallback_positions());

class Random final {
public:
  explicit Random(std::uint32_t state) : state_{state == 0U ? fallback_random_state : state} {}

  [[nodiscard]] std::uint32_t bits() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    return state_;
  }

  [[nodiscard]] float unit() { return static_cast<float>(bits() >> 8U) * (1.0F / 16777216.0F); }

  [[nodiscard]] float range(float minimum, float maximum) {
    return minimum + (maximum - minimum) * unit();
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

// Quintic value noise supplies a continuous, non-looping force target. Each
// source owns a separate seed and correlation time; no per-frame randomness or
// authored animation period is involved.
class SmoothNoise final {
public:
  struct Configuration final {
    std::uint32_t seed;
    float correlation_time;
  };

  SmoothNoise() = default;
  explicit SmoothNoise(Configuration configuration)
      : seed_{configuration.seed == 0U ? fallback_random_state : configuration.seed},
        correlation_time_{configuration.correlation_time} {}

  [[nodiscard]] float sample(double time) const {
    const auto coordinate = time / static_cast<double>(correlation_time_);
    const auto lattice = static_cast<std::uint64_t>(std::floor(coordinate));
    const auto fraction = static_cast<float>(coordinate - std::floor(coordinate));
    const auto blend =
        fraction * fraction * fraction * (fraction * (fraction * 6.0F - 15.0F) + 10.0F);
    return std::lerp(value_at(lattice), value_at(lattice + 1U), blend);
  }

private:
  [[nodiscard]] float value_at(std::uint64_t lattice) const {
    auto value =
        seed_ ^ static_cast<std::uint32_t>(lattice) ^ static_cast<std::uint32_t>(lattice >> 32U);
    value += 0x9E3779B9U;
    value = (value ^ (value >> 16U)) * 0x21F0AAADU;
    value = (value ^ (value >> 15U)) * 0x735A2D97U;
    value ^= value >> 15U;
    return static_cast<float>(value >> 8U) * (2.0F / 16777216.0F) - 1.0F;
  }

  std::uint32_t seed_{fallback_random_state};
  float correlation_time_{1.0F};
};

struct ScalarMotion final {
  float value{};
  float velocity{};
  float equilibrium{};
  float minimum{};
  float maximum{};
  float drive{};
  float restoring{};
  float damping{};
  SmoothNoise noise{};
};

struct NoiseRequest final {
  float minimum_time;
  float maximum_time;
  std::size_t channel;
};

struct ScalarRequest final {
  float equilibrium;
  Bounds range;
  Bounds drive;
  Bounds restoring;
  Bounds damping;
  Bounds correlation_time;
  std::size_t noise_channel;
};

struct Singularity final {
  NormalizedPoint position{};
  NormalizedPoint velocity{};
  SmoothNoise force_x{};
  SmoothNoise force_y{};
  float force_scale{};
  float damping{};
  ScalarMotion strength{};
  float orientation{};
  float angular_velocity{};
  float angular_drive{};
  float angular_damping{};
  SmoothNoise orientation_force{};
  ScalarMotion anisotropy{};
  ScalarMotion influence_radius{};
};

class FieldDynamics final {
public:
  [[nodiscard]] static FieldDynamics random() {
    Random random{browser_random_state()};
    FieldDynamics dynamics;
    dynamics.center_count_ = sample_center_count(random);
    const auto positions = sample_positions(random, dynamics.center_count_);
    std::size_t noise_channel{};

    for (std::size_t index = 0; index < dynamics.center_count_; ++index) {
      auto &singularity = dynamics.singularities_[index];
      singularity.position = NormalizedPoint{
          .x = (positions[index].x * 2.0F - 1.0F) * field_domain_scale,
          .y = (positions[index].y * 2.0F - 1.0F) * field_domain_scale,
      };
      singularity.velocity = NormalizedPoint{
          .x = random.range(-0.0035F, 0.0035F),
          .y = random.range(-0.0035F, 0.0035F),
      };
      singularity.force_x = make_noise(
          random,
          NoiseRequest{.minimum_time = 8.0F, .maximum_time = 18.0F, .channel = noise_channel++});
      singularity.force_y = make_noise(
          random,
          NoiseRequest{.minimum_time = 10.0F, .maximum_time = 23.0F, .channel = noise_channel++});
      singularity.force_scale = random.range(0.0030F, 0.0058F);
      singularity.damping = random.range(0.24F, 0.40F);

      singularity.strength =
          make_scalar(random, ScalarRequest{
                                  .equilibrium = random.range(0.40F, 0.88F),
                                  .range = Bounds{.minimum = 0.26F, .maximum = 0.98F},
                                  .drive = Bounds{.minimum = 0.0018F, .maximum = 0.0038F},
                                  .restoring = Bounds{.minimum = 0.016F, .maximum = 0.030F},
                                  .damping = Bounds{.minimum = 0.16F, .maximum = 0.28F},
                                  .correlation_time = Bounds{.minimum = 13.0F, .maximum = 31.0F},
                                  .noise_channel = noise_channel++,
                              });
      singularity.orientation = random.range(0.0F, tau);
      singularity.angular_velocity = random.range(-0.0040F, 0.0040F);
      singularity.angular_drive = random.range(0.0013F, 0.0032F);
      singularity.angular_damping = random.range(0.11F, 0.22F);
      singularity.orientation_force = make_noise(
          random,
          NoiseRequest{.minimum_time = 17.0F, .maximum_time = 39.0F, .channel = noise_channel++});
      singularity.anisotropy =
          make_scalar(random, ScalarRequest{
                                  .equilibrium = random.range(0.78F, 1.34F),
                                  .range = Bounds{.minimum = 0.62F, .maximum = 1.58F},
                                  .drive = Bounds{.minimum = 0.0018F, .maximum = 0.0042F},
                                  .restoring = Bounds{.minimum = 0.013F, .maximum = 0.026F},
                                  .damping = Bounds{.minimum = 0.14F, .maximum = 0.25F},
                                  .correlation_time = Bounds{.minimum = 16.0F, .maximum = 36.0F},
                                  .noise_channel = noise_channel++,
                              });
      singularity.influence_radius =
          make_scalar(random, ScalarRequest{
                                  .equilibrium = random.range(0.068F, 0.105F),
                                  .range = Bounds{.minimum = 0.052F, .maximum = 0.128F},
                                  .drive = Bounds{.minimum = 0.00016F, .maximum = 0.00036F},
                                  .restoring = Bounds{.minimum = 0.018F, .maximum = 0.034F},
                                  .damping = Bounds{.minimum = 0.16F, .maximum = 0.28F},
                                  .correlation_time = Bounds{.minimum = 19.0F, .maximum = 43.0F},
                                  .noise_channel = noise_channel++,
                              });
    }

    dynamics.refresh_uniforms();
    return dynamics;
  }

  void advance(double elapsed_seconds) {
    auto remaining = static_cast<float>(std::clamp(elapsed_seconds, 0.0, maximum_frame_step));
    while (remaining > 0.0F) {
      const auto step = std::min(remaining, maximum_integration_step);
      integrate(step);
      remaining -= step;
    }
    refresh_uniforms();
  }

  [[nodiscard]] const GLfloat *positions() const { return position_values_.data(); }
  [[nodiscard]] const GLfloat *parameters() const { return parameter_values_.data(); }
  [[nodiscard]] GLsizei center_count() const { return static_cast<GLsizei>(center_count_); }

private:
  using PositionValues = std::array<GLfloat, maximum_center_count * 2U>;
  using ParameterValues = std::array<GLfloat, maximum_center_count * 4U>;
  static constexpr int maximum_placement_attempts = 96;

  FieldDynamics() = default;

  [[nodiscard]] static SmoothNoise make_noise(Random &random, NoiseRequest request) {
    // The channel offset further separates already independent time scales.
    const auto channel_offset = static_cast<float>(request.channel) * 0.137F;
    return SmoothNoise{SmoothNoise::Configuration{
        .seed = random.bits(),
        .correlation_time =
            random.range(request.minimum_time, request.maximum_time) + channel_offset,
    }};
  }

  [[nodiscard]] static ScalarMotion make_scalar(Random &random, ScalarRequest request) {
    const auto span = request.range.maximum - request.range.minimum;
    return ScalarMotion{
        .value = std::clamp(request.equilibrium + random.range(-0.06F, 0.06F) * span,
                            request.range.minimum, request.range.maximum),
        .velocity = random.range(-0.002F, 0.002F) * span,
        .equilibrium = request.equilibrium,
        .minimum = request.range.minimum,
        .maximum = request.range.maximum,
        .drive = random.range(request.drive.minimum, request.drive.maximum),
        .restoring = random.range(request.restoring.minimum, request.restoring.maximum),
        .damping = random.range(request.damping.minimum, request.damping.maximum),
        .noise = make_noise(random, NoiseRequest{.minimum_time = request.correlation_time.minimum,
                                                 .maximum_time = request.correlation_time.maximum,
                                                 .channel = request.noise_channel}),
    };
  }

  [[nodiscard]] static std::size_t sample_center_count(Random &random) {
    const auto sample = random.unit();
    std::size_t count{1U};
    for (const auto probability : center_count_cumulative_probabilities) {
      if (sample < probability) {
        return count;
      }
      ++count;
    }
    return maximum_center_count;
  }

  [[nodiscard]] static std::array<NormalizedPoint, maximum_center_count>
  sample_positions(Random &random, std::size_t center_count) {
    std::array<NormalizedPoint, maximum_center_count> positions{};
    for (std::size_t index = 0; index < center_count; ++index) {
      const auto position = sample_position(
          random, PlacementRequest{.positions = &positions, .populated_count = index});
      if (!position) {
        return fallback_positions;
      }
      positions[index] = *position;
    }
    return positions;
  }

  struct PlacementRequest final {
    const std::array<NormalizedPoint, maximum_center_count> *positions;
    std::size_t populated_count;
  };

  [[nodiscard]] static std::optional<NormalizedPoint> sample_position(Random &random,
                                                                      PlacementRequest request) {
    for (auto attempt = 0; attempt < maximum_placement_attempts; ++attempt) {
      const NormalizedPoint candidate{
          .x = placement_left + (placement_right - placement_left) * random.unit(),
          .y = placement_top + (placement_bottom - placement_top) * random.unit(),
      };
      auto separated = true;
      for (std::size_t previous = 0; previous < request.populated_count; ++previous) {
        const auto delta_x = candidate.x - (*request.positions)[previous].x;
        const auto delta_y = candidate.y - (*request.positions)[previous].y;
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

  [[nodiscard]] static float boundary_force(float value, Bounds bounds) {
    if (value < bounds.minimum) {
      return boundary_stiffness * (bounds.minimum - value);
    }
    if (value > bounds.maximum) {
      return -boundary_stiffness * (value - bounds.maximum);
    }
    return 0.0F;
  }

  struct IntegrationInterval final {
    double time;
    float step;
  };

  static void integrate_scalar(ScalarMotion &motion, IntegrationInterval interval) {
    auto acceleration = motion.drive * motion.noise.sample(interval.time) -
                        motion.restoring * (motion.value - motion.equilibrium) -
                        motion.damping * motion.velocity;
    acceleration += boundary_stiffness * 2.0F *
                    ((motion.value < motion.minimum)
                         ? motion.minimum - motion.value
                         : (motion.value > motion.maximum ? motion.maximum - motion.value : 0.0F));
    motion.velocity += acceleration * interval.step;
    motion.value += motion.velocity * interval.step;

    const auto safety_margin = 0.25F * (motion.maximum - motion.minimum);
    motion.value =
        std::clamp(motion.value, motion.minimum - safety_margin, motion.maximum + safety_margin);
  }

  void integrate(float step) {
    std::array<NormalizedPoint, maximum_center_count> accelerations{};
    for (std::size_t index = 0; index < center_count_; ++index) {
      const auto &singularity = singularities_[index];
      accelerations[index] = NormalizedPoint{
          .x = singularity.force_scale * singularity.force_x.sample(simulation_time_) +
               boundary_force(singularity.position.x, horizontal_boundaries) -
               singularity.damping * singularity.velocity.x,
          .y = singularity.force_scale * singularity.force_y.sample(simulation_time_) +
               boundary_force(singularity.position.y, vertical_boundaries) -
               singularity.damping * singularity.velocity.y,
      };
    }

    const auto interaction_radius_squared = interaction_radius * interaction_radius;
    for (std::size_t first = 0; first < center_count_; ++first) {
      for (std::size_t second = first + 1U; second < center_count_; ++second) {
        const auto delta_x = singularities_[first].position.x - singularities_[second].position.x;
        const auto delta_y = singularities_[first].position.y - singularities_[second].position.y;
        const auto distance_squared = delta_x * delta_x + delta_y * delta_y;
        if (distance_squared >= interaction_radius_squared) {
          continue;
        }

        const auto distance = std::sqrt(std::max(distance_squared, 0.000001F));
        const auto proximity = 1.0F - distance / interaction_radius;
        const auto magnitude = interaction_strength * proximity * proximity;
        const auto direction_x = distance_squared > 0.000001F ? delta_x / distance : 1.0F;
        const auto direction_y = distance_squared > 0.000001F ? delta_y / distance : 0.0F;
        accelerations[first].x += direction_x * magnitude;
        accelerations[first].y += direction_y * magnitude;
        accelerations[second].x -= direction_x * magnitude;
        accelerations[second].y -= direction_y * magnitude;
      }
    }

    for (std::size_t index = 0; index < center_count_; ++index) {
      auto &singularity = singularities_[index];
      singularity.velocity.x += accelerations[index].x * step;
      singularity.velocity.y += accelerations[index].y * step;
      const auto speed_squared = singularity.velocity.x * singularity.velocity.x +
                                 singularity.velocity.y * singularity.velocity.y;
      if (speed_squared > maximum_drift_speed * maximum_drift_speed) {
        const auto scale = maximum_drift_speed / std::sqrt(speed_squared);
        singularity.velocity.x *= scale;
        singularity.velocity.y *= scale;
      }
      singularity.position.x += singularity.velocity.x * step;
      singularity.position.y += singularity.velocity.y * step;

      const IntegrationInterval interval{.time = simulation_time_, .step = step};
      integrate_scalar(singularity.strength, interval);
      integrate_scalar(singularity.anisotropy, interval);
      integrate_scalar(singularity.influence_radius, interval);
      const auto angular_acceleration =
          singularity.angular_drive * singularity.orientation_force.sample(simulation_time_) -
          singularity.angular_damping * singularity.angular_velocity;
      singularity.angular_velocity += angular_acceleration * step;
      singularity.orientation += singularity.angular_velocity * step;
      if (singularity.orientation < 0.0F || singularity.orientation >= tau) {
        singularity.orientation = std::fmod(singularity.orientation, tau);
        if (singularity.orientation < 0.0F) {
          singularity.orientation += tau;
        }
      }
    }
    simulation_time_ += static_cast<double>(step);
  }

  void refresh_uniforms() {
    for (std::size_t index = 0; index < center_count_; ++index) {
      const auto &singularity = singularities_[index];
      position_values_[index * 2U] = singularity.position.x;
      position_values_[index * 2U + 1U] = singularity.position.y;
      parameter_values_[index * 4U] = singularity.strength.value;
      parameter_values_[index * 4U + 1U] = singularity.orientation;
      parameter_values_[index * 4U + 2U] = singularity.anisotropy.value;
      parameter_values_[index * 4U + 3U] = singularity.influence_radius.value;
    }
  }

  std::array<Singularity, maximum_center_count> singularities_;
  PositionValues position_values_{};
  ParameterValues parameter_values_{};
  std::size_t center_count_{};
  double simulation_time_{};
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
  [[nodiscard]] static std::optional<Pipeline> create(const FieldDynamics &dynamics,
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
        .resolution = glGetUniformLocation(program, "u_resolution"),
        .poles = glGetUniformLocation(program, "u_poles[0]"),
        .singularities = glGetUniformLocation(program, "u_singularities[0]"),
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
    // This is the renderer's only pipeline, so its program and vertex array stay bound.
    pipeline.bind();
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glUniform2f(uniforms.resolution, extent.width_as_float(), extent.height_as_float());
    glUniform2fv(uniforms.poles, dynamics.center_count(), dynamics.positions());
    glUniform4fv(uniforms.singularities, dynamics.center_count(), dynamics.parameters());
    glUniform1i(uniforms.center_count, dynamics.center_count());
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
    glUniform2f(uniforms_.resolution, extent.width_as_float(), extent.height_as_float());
  }

  void draw(const FieldDynamics &dynamics) const {
    glUniform2fv(uniforms_.poles, dynamics.center_count(), dynamics.positions());
    glUniform4fv(uniforms_.singularities, dynamics.center_count(), dynamics.parameters());
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

private:
  struct Handles final {
    GLuint program;
    GLuint vertex_array;
  };

  struct Uniforms final {
    GLint resolution;
    GLint poles;
    GLint singularities;
    GLint center_count;

    [[nodiscard]] bool valid() const {
      return resolution >= 0 && poles >= 0 && singularities >= 0 && center_count >= 0;
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
    auto dynamics = FieldDynamics::random();
    auto pipeline = Pipeline::create(dynamics, *extent);
    if (!pipeline) {
      return std::nullopt;
    }
    return Renderer{std::move(*context), std::move(*pipeline), dynamics, *extent,
                    preferred_motion_mode()};
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
      pipeline_.draw(dynamics_);
      return true;
    }

    last_animation_timestamp_ = emscripten_get_now();
    pipeline_.draw(dynamics_);
    emscripten_request_animation_frame_loop(on_animation_frame, this);
    return true;
  }

private:
  Renderer(WebGlContext context, Pipeline pipeline, const FieldDynamics &dynamics,
           CanvasExtent extent, MotionMode motion_mode)
      : context_{std::move(context)}, pipeline_{std::move(pipeline)}, dynamics_{dynamics},
        extent_{extent}, motion_mode_{motion_mode} {}

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
    const auto elapsed_seconds = (timestamp - renderer.last_animation_timestamp_) / 1000.0;
    renderer.last_animation_timestamp_ = timestamp;
    renderer.dynamics_.advance(elapsed_seconds);
    renderer.pipeline_.draw(renderer.dynamics_);
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
      renderer.pipeline_.draw(renderer.dynamics_);
    }
  }

  WebGlContext context_;
  Pipeline pipeline_;
  FieldDynamics dynamics_;
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
