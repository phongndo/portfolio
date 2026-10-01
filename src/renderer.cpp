#include "renderer.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
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
constexpr float minimum_center_distance = 0.21F;
constexpr float minimum_center_distance_squared = minimum_center_distance * minimum_center_distance;
constexpr float field_domain_scale = 1.08F;
constexpr float boundary_stiffness = 0.055F;
constexpr float interaction_radius = 0.24F;
constexpr float interaction_strength = 0.006F;
constexpr float maximum_drift_speed = 0.040F;
constexpr float tau = 2.0F * std::numbers::pi_v<float>;

struct Vec2 final {
  float x{};
  float y{};

  friend constexpr Vec2 operator+(Vec2 lhs, Vec2 rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y}; }
  friend constexpr Vec2 operator-(Vec2 lhs, Vec2 rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y}; }
  friend constexpr Vec2 operator*(Vec2 vector, float scale) {
    return {vector.x * scale, vector.y * scale};
  }
  friend constexpr Vec2 operator/(Vec2 vector, float divisor) {
    return {vector.x / divisor, vector.y / divisor};
  }
  constexpr Vec2 &operator+=(Vec2 other) { return *this = *this + other; }
  constexpr Vec2 &operator-=(Vec2 other) { return *this = *this - other; }
  constexpr Vec2 &operator*=(float scale) { return *this = *this * scale; }
};

[[nodiscard]] constexpr float length_squared(Vec2 vector) {
  return vector.x * vector.x + vector.y * vector.y;
}

struct Bounds final {
  float minimum;
  float maximum;

  [[nodiscard]] constexpr float span() const { return maximum - minimum; }
  [[nodiscard]] constexpr bool contains(float value) const {
    return value >= minimum && value <= maximum;
  }
  // The signed distance that returns an outside value to the nearest bound.
  [[nodiscard]] constexpr float overshoot_correction(float value) const {
    if (value < minimum) {
      return minimum - value;
    }
    if (value > maximum) {
      return maximum - value;
    }
    return 0.0F;
  }
};

// Placements are fractions of the canvas; boundaries are field coordinates.
constexpr Bounds fallback_placement_x{.minimum = 0.32F, .maximum = 0.92F};
constexpr Bounds placement_y{.minimum = 0.08F, .maximum = 0.92F};
constexpr Bounds boundary_y{.minimum = -1.00F, .maximum = 1.00F};
constexpr float default_text_edge = 0.40F;
constexpr float text_clearance = 0.06F;
constexpr float minimum_placement_width = 0.30F;

[[nodiscard]] constexpr float field_coordinate(float canvas_fraction) {
  return (canvas_fraction * 2.0F - 1.0F) * field_domain_scale;
}

// Singularities are placed and held to the right of the left-aligned text
// column. When the column fills a narrow screen, they sit at the right edge,
// partly off canvas, so only their outer field lines cross the text.
struct FieldLayout final {
  float text_edge;
  Bounds placement_x;
  Bounds boundary_x;

  [[nodiscard]] static constexpr FieldLayout beside(float text_edge) {
    const auto minimum = std::clamp(text_edge + text_clearance, 0.32F, 0.80F);
    const Bounds placement{
        .minimum = minimum,
        .maximum = std::max(0.92F, minimum + minimum_placement_width),
    };
    return FieldLayout{
        .text_edge = text_edge,
        .placement_x = placement,
        .boundary_x = {.minimum = field_coordinate(placement.minimum) - 0.08F,
                       .maximum = field_coordinate(placement.maximum) + 0.09F},
    };
  }
};

// The right edge of the text column as a fraction of the canvas width.
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

constexpr std::array fallback_positions{
    Vec2{0.36F, 0.18F}, Vec2{0.65F, 0.15F}, Vec2{0.90F, 0.24F}, Vec2{0.40F, 0.58F},
    Vec2{0.69F, 0.52F}, Vec2{0.88F, 0.80F}, Vec2{0.55F, 0.85F},
};

// A discretized normal distribution centered on five (sigma 1.25). About 80%
// of visits receive four to six centers, while a single center occurs on about
// one in every 500 visits.
constexpr std::array center_count_cumulative_probabilities{
    0.00194628F, 0.02022834F, 0.11077998F, 0.34727336F, 0.67295498F, 0.90944836F,
};

[[nodiscard]] constexpr bool is_placeable(Vec2 candidate, Bounds placement_x,
                                          std::span<const Vec2> placed) {
  return placement_x.contains(candidate.x) && placement_y.contains(candidate.y) &&
         std::ranges::none_of(placed, [candidate](Vec2 other) {
           return length_squared(candidate - other) < minimum_center_distance_squared;
         });
}

[[nodiscard]] constexpr bool valid_fallback_positions() {
  const std::span positions{fallback_positions};
  for (std::size_t index = 0; index < positions.size(); ++index) {
    if (!is_placeable(positions[index], fallback_placement_x, positions.first(index))) {
      return false;
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

  [[nodiscard]] float range(Bounds bounds) { return bounds.minimum + bounds.span() * unit(); }

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

struct TimeStep final {
  double time;
  float duration;
};

// A damped oscillator driven by noise toward its equilibrium, softly held
// inside its range.
struct ScalarMotion final {
  float value;
  float velocity;
  float equilibrium;
  Bounds range;
  float drive;
  float restoring;
  float damping;
  SmoothNoise noise;

  void integrate(TimeStep step) {
    const auto acceleration = drive * noise.sample(step.time) - restoring * (value - equilibrium) -
                              damping * velocity +
                              boundary_stiffness * 2.0F * range.overshoot_correction(value);
    velocity += acceleration * step.duration;
    value += velocity * step.duration;

    const auto safety_margin = 0.25F * range.span();
    value = std::clamp(value, range.minimum - safety_margin, range.maximum + safety_margin);
  }
};

struct AngularMotion final {
  float orientation;
  float velocity;
  float drive;
  float damping;
  SmoothNoise noise;

  void integrate(TimeStep step) {
    const auto acceleration = drive * noise.sample(step.time) - damping * velocity;
    velocity += acceleration * step.duration;
    orientation += velocity * step.duration;
    if (orientation < 0.0F || orientation >= tau) {
      orientation = std::fmod(orientation, tau);
      if (orientation < 0.0F) {
        orientation += tau;
      }
    }
  }
};

struct PlanarMotion final {
  Vec2 position;
  Vec2 velocity;
  SmoothNoise noise_x;
  SmoothNoise noise_y;
  float drive;
  float damping;

  [[nodiscard]] Vec2 acceleration(double time, Bounds boundary_x) const {
    const Vec2 noise{noise_x.sample(time), noise_y.sample(time)};
    const Vec2 containment{
        boundary_stiffness * boundary_x.overshoot_correction(position.x),
        boundary_stiffness * boundary_y.overshoot_correction(position.y),
    };
    return noise * drive + containment - velocity * damping;
  }

  void integrate(Vec2 acceleration, float duration) {
    velocity += acceleration * duration;
    const auto speed_squared = length_squared(velocity);
    if (speed_squared > maximum_drift_speed * maximum_drift_speed) {
      velocity *= maximum_drift_speed / std::sqrt(speed_squared);
    }
    position += velocity * duration;
  }
};

// Declaration order is also the order in which random parameters are drawn.
struct Singularity final {
  PlanarMotion drift;
  ScalarMotion strength;
  AngularMotion rotation;
  ScalarMotion anisotropy;
  ScalarMotion influence_radius;
};

struct ScalarRequest final {
  float equilibrium;
  Bounds range;
  Bounds drive;
  Bounds restoring;
  Bounds damping;
  Bounds correlation_time;
};

// Draws motion parameters, giving every noise source its own channel.
class MotionSampler final {
public:
  explicit MotionSampler(Random &random) : random_{random} {}

  [[nodiscard]] float range(Bounds bounds) { return random_.range(bounds); }

  [[nodiscard]] SmoothNoise noise(Bounds correlation_time) {
    // The channel offset further separates already independent time scales.
    const auto channel_offset = static_cast<float>(next_channel_++) * 0.137F;
    return SmoothNoise{SmoothNoise::Configuration{
        .seed = random_.bits(),
        .correlation_time = random_.range(correlation_time) + channel_offset,
    }};
  }

  [[nodiscard]] ScalarMotion scalar(const ScalarRequest &request) {
    const auto span = request.range.span();
    return ScalarMotion{
        .value = std::clamp(request.equilibrium + range({-0.06F, 0.06F}) * span,
                            request.range.minimum, request.range.maximum),
        .velocity = range({-0.002F, 0.002F}) * span,
        .equilibrium = request.equilibrium,
        .range = request.range,
        .drive = range(request.drive),
        .restoring = range(request.restoring),
        .damping = range(request.damping),
        .noise = noise(request.correlation_time),
    };
  }

  [[nodiscard]] Singularity singularity(Vec2 placement) {
    return Singularity{
        .drift =
            PlanarMotion{
                .position = (placement * 2.0F - Vec2{1.0F, 1.0F}) * field_domain_scale,
                .velocity = Vec2{range({-0.0035F, 0.0035F}), range({-0.0035F, 0.0035F})},
                .noise_x = noise({8.0F, 18.0F}),
                .noise_y = noise({10.0F, 23.0F}),
                .drive = range({0.0030F, 0.0058F}),
                .damping = range({0.24F, 0.40F}),
            },
        .strength = scalar({
            .equilibrium = range({0.40F, 0.88F}),
            .range = {0.26F, 0.98F},
            .drive = {0.0018F, 0.0038F},
            .restoring = {0.016F, 0.030F},
            .damping = {0.16F, 0.28F},
            .correlation_time = {13.0F, 31.0F},
        }),
        .rotation =
            AngularMotion{
                .orientation = range({0.0F, tau}),
                .velocity = range({-0.0040F, 0.0040F}),
                .drive = range({0.0013F, 0.0032F}),
                .damping = range({0.11F, 0.22F}),
                .noise = noise({17.0F, 39.0F}),
            },
        .anisotropy = scalar({
            .equilibrium = range({0.78F, 1.34F}),
            .range = {0.62F, 1.58F},
            .drive = {0.0018F, 0.0042F},
            .restoring = {0.013F, 0.026F},
            .damping = {0.14F, 0.25F},
            .correlation_time = {16.0F, 36.0F},
        }),
        .influence_radius = scalar({
            .equilibrium = range({0.068F, 0.105F}),
            .range = {0.052F, 0.128F},
            .drive = {0.00016F, 0.00036F},
            .restoring = {0.018F, 0.034F},
            .damping = {0.16F, 0.28F},
            .correlation_time = {19.0F, 43.0F},
        }),
    };
  }

private:
  Random &random_;
  std::size_t next_channel_{};
};

class FieldDynamics final {
public:
  [[nodiscard]] static FieldDynamics random(const FieldLayout &layout) {
    Random random{browser_random_state()};
    const auto count = sample_center_count(random);
    const auto placements = sample_placements(random, count, layout.placement_x);

    MotionSampler sampler{random};
    FieldDynamics dynamics{count, layout.boundary_x};
    for (const auto [singularity, placement] :
         std::views::zip(dynamics.active(), std::span{placements}.first(count))) {
      singularity = sampler.singularity(placement);
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

  void set_layout(const FieldLayout &layout) { boundary_x_ = layout.boundary_x; }

  [[nodiscard]] const GLfloat *positions() const { return position_values_.data(); }
  [[nodiscard]] const GLfloat *parameters() const { return parameter_values_.data(); }
  [[nodiscard]] GLsizei center_count() const { return static_cast<GLsizei>(center_count_); }

private:
  using Placements = std::array<Vec2, maximum_center_count>;
  static constexpr int maximum_placement_attempts = 96;

  FieldDynamics(std::size_t center_count, Bounds boundary_x)
      : boundary_x_{boundary_x}, center_count_{center_count} {}

  [[nodiscard]] std::span<Singularity> active() {
    return std::span{singularities_}.first(center_count_);
  }

  [[nodiscard]] static std::size_t sample_center_count(Random &random) {
    const auto sample = random.unit();
    const auto below = std::ranges::upper_bound(center_count_cumulative_probabilities, sample);
    return static_cast<std::size_t>(below - center_count_cumulative_probabilities.begin()) + 1U;
  }

  [[nodiscard]] static Placements sample_placements(Random &random, std::size_t center_count,
                                                    Bounds placement_x) {
    Placements placements{};
    for (std::size_t index = 0; index < center_count; ++index) {
      const auto placement =
          sample_placement(random, placement_x, std::span{placements}.first(index));
      if (!placement) {
        return fallback_placements(placement_x);
      }
      placements[index] = *placement;
    }
    return placements;
  }

  // Stretches the fallback positions horizontally into the available range.
  [[nodiscard]] static Placements fallback_placements(Bounds placement_x) {
    Placements placements = fallback_positions;
    for (auto &placement : placements) {
      const auto fraction =
          (placement.x - fallback_placement_x.minimum) / fallback_placement_x.span();
      placement.x = placement_x.minimum + fraction * placement_x.span();
    }
    return placements;
  }

  [[nodiscard]] static std::optional<Vec2> sample_placement(Random &random, Bounds placement_x,
                                                            std::span<const Vec2> placed) {
    for (auto attempt = 0; attempt < maximum_placement_attempts; ++attempt) {
      const Vec2 candidate{random.range(placement_x), random.range(placement_y)};
      if (is_placeable(candidate, placement_x, placed)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  void integrate(float duration) {
    const auto singularities = active();
    const TimeStep step{.time = simulation_time_, .duration = duration};

    std::array<Vec2, maximum_center_count> accelerations{};
    for (std::size_t index = 0; index < singularities.size(); ++index) {
      accelerations[index] = singularities[index].drift.acceleration(step.time, boundary_x_);
    }

    constexpr auto interaction_radius_squared = interaction_radius * interaction_radius;
    for (std::size_t first = 0; first < singularities.size(); ++first) {
      for (std::size_t second = first + 1U; second < singularities.size(); ++second) {
        const auto delta =
            singularities[first].drift.position - singularities[second].drift.position;
        const auto distance_squared = length_squared(delta);
        if (distance_squared >= interaction_radius_squared) {
          continue;
        }

        const auto distance = std::sqrt(std::max(distance_squared, 0.000001F));
        const auto proximity = 1.0F - distance / interaction_radius;
        const auto direction = distance_squared > 0.000001F ? delta / distance : Vec2{1.0F, 0.0F};
        const auto repulsion = direction * (interaction_strength * proximity * proximity);
        accelerations[first] += repulsion;
        accelerations[second] -= repulsion;
      }
    }

    for (std::size_t index = 0; index < singularities.size(); ++index) {
      auto &singularity = singularities[index];
      singularity.drift.integrate(accelerations[index], duration);
      singularity.strength.integrate(step);
      singularity.anisotropy.integrate(step);
      singularity.influence_radius.integrate(step);
      singularity.rotation.integrate(step);
    }
    simulation_time_ += static_cast<double>(duration);
  }

  void refresh_uniforms() {
    for (std::size_t index = 0; const auto &singularity : active()) {
      position_values_[index * 2U] = singularity.drift.position.x;
      position_values_[index * 2U + 1U] = singularity.drift.position.y;
      parameter_values_[index * 4U] = singularity.strength.value;
      parameter_values_[index * 4U + 1U] = singularity.rotation.orientation;
      parameter_values_[index * 4U + 2U] = singularity.anisotropy.value;
      parameter_values_[index * 4U + 3U] = singularity.influence_radius.value;
      ++index;
    }
  }

  std::array<Singularity, maximum_center_count> singularities_{};
  std::array<GLfloat, maximum_center_count * 2U> position_values_{};
  std::array<GLfloat, maximum_center_count * 4U> parameter_values_{};
  Bounds boundary_x_;
  std::size_t center_count_;
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
  [[nodiscard]] static std::optional<Pipeline> create(GLsizei center_count) {
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
        .text_edge = glGetUniformLocation(program, "u_text_edge"),
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
    // resolution, layout, and field uniforms are set before the first draw.
    glUseProgram(program);
    glBindVertexArray(vertex_array);
    glUniform1i(uniforms.center_count, center_count);
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

  void set_resolution(const CanvasExtent &extent) const {
    glUniform2f(uniforms_.resolution, extent.width_as_float(), extent.height_as_float());
  }

  void set_layout(const FieldLayout &layout) const {
    glUniform1f(uniforms_.text_edge, layout.text_edge);
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
    GLint text_edge;

    [[nodiscard]] bool valid() const {
      return resolution >= 0 && poles >= 0 && singularities >= 0 && center_count >= 0 &&
             text_edge >= 0;
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
    auto dynamics = FieldDynamics::random(FieldLayout::beside(measure_text_edge()));
    auto pipeline = Pipeline::create(dynamics.center_count());
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

    pipeline_.draw(dynamics_);
    if (motion_mode_ == MotionMode::animated) {
      last_animation_timestamp_ = emscripten_get_now();
      emscripten_request_animation_frame_loop(on_animation_frame, this);
    }
    return true;
  }

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
    // The text column only moves when the viewport, and so the canvas, resizes.
    const auto layout = FieldLayout::beside(measure_text_edge());
    pipeline_.set_layout(layout);
    dynamics_.set_layout(layout);
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
