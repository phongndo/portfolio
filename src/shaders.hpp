#ifndef PORTFOLIO_SHADERS_HPP
#define PORTFOLIO_SHADERS_HPP

namespace portfolio::shaders {

inline constexpr auto vertex = R"glsl(#version 300 es

precision highp float;

uniform float u_time;
uniform vec2 u_resolution;
uniform vec4 u_seed[12];

flat out vec4 v_poles01;
flat out vec4 v_poles23;
flat out vec4 v_poles45;
flat out vec4 v_transform;
flat out vec4 v_residues01;
flat out vec4 v_residues23;
flat out vec4 v_residues45;
flat out vec4 v_fold;
flat out vec4 v_circulation03;
flat out vec2 v_circulation45;
flat out vec4 v_secondary0;
flat out vec4 v_secondary1;
flat out vec4 v_secondary2;
flat out vec4 v_dynamics;

const float animation_speed = 0.20;
const float tau = 6.283185307179586;
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

vec2 complex_multiply(vec2 a, vec2 b) {
  return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complex_divide(vec2 a, vec2 b) {
  return complex_multiply(a, vec2(b.x, -b.y)) / max(dot(b, b), 0.0001);
}

float random_value(int channel) {
  return u_seed[channel / 4][channel % 4];
}

float random_signed(int channel) {
  return random_value(channel) * 2.0 - 1.0;
}

vec2 random_coordinate(int index) {
  int channel = index * 2;
  vec2 point = vec2(random_value(channel), random_value(channel + 1)) * 2.0 - 1.0;
  point.x *= u_resolution.x / u_resolution.y;
  return point * 1.08;
}

vec2 warp_coordinate(vec2 coordinate, vec2 bend, vec2 offset) {
  return complex_divide(
      coordinate + offset,
      vec2(1.0, 0.0) + complex_multiply(bend, coordinate));
}

vec2 animated_pole(
    int index,
    float t,
    float phase,
    float x_frequency,
    float y_frequency,
    float phase_shift,
    vec2 bend,
    vec2 offset) {
  float amplitude = 0.028 + 0.020 * random_value(18 + index);
  vec2 orbit = amplitude * vec2(
      sin(t * x_frequency + phase + phase_shift),
      0.86 * cos(t * y_frequency + phase * 0.83 - phase_shift));
  return warp_coordinate(random_coordinate(index), bend, offset) + orbit;
}

float evolving_strength(int index, float t, float phase) {
  float strength =
      0.30 + 0.40 * random_value(28 + index) +
      0.065 * sin(t * (0.17 + 0.018 * float(index)) + phase * 0.31);
  return clamp(strength, 0.26, 0.76);
}

vec2 residue(int index, float strength, float t, float phase) {
  float angle =
      tau * random_value(34 + index) +
      0.075 * sin(t * (0.21 + 0.011 * float(index)) + phase * 0.23);
  float magnitude = 0.026 + 0.050 * strength;
  return magnitude * vec2(cos(angle), sin(angle));
}

float circulation(int index, float strength, float t, float phase) {
  return
      (0.0035 + 0.0070 * strength + 0.0020 * random_value(40 + index)) *
      (0.90 + 0.10 * sin(t * (0.15 + 0.012 * float(index)) + phase * 0.19));
}

void main() {
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);

  float t = u_time * animation_speed;
  float phase0 = tau * random_value(12);
  float phase1 = tau * random_value(13);
  float phase2 = tau * random_value(14);
  float phase3 = tau * random_value(15);
  float phase4 = tau * random_value(16);
  float phase5 = tau * random_value(17);

  vec2 bend = vec2(
      0.064 + 0.014 * random_signed(24) + 0.013 * sin(t * 0.25 + phase0 * 0.31),
      0.018 * random_signed(25) + 0.038 * cos(t * 0.21 + phase3 * 0.27));
  vec2 offset = vec2(
      0.010 * random_signed(26) + 0.022 * sin(t * 0.34 + phase2 * 0.25),
      0.009 * random_signed(27) + 0.019 * cos(t * 0.29 + phase5 * 0.23));

  vec2 pole0 = animated_pole(0, t, phase0, 0.41, 0.33, 0.0, bend, offset);
  vec2 pole1 = animated_pole(1, t, phase1, 0.31, 0.43, 0.7, bend, offset);
  vec2 pole2 = animated_pole(2, t, phase2, 0.37, 0.29, 1.4, bend, offset);
  vec2 pole3 = animated_pole(3, t, phase3, 0.27, 0.39, 2.1, bend, offset);
  vec2 pole4 = animated_pole(4, t, phase4, 0.35, 0.25, 2.8, bend, offset);
  vec2 pole5 = animated_pole(5, t, phase5, 0.29, 0.37, 3.5, bend, offset);

  float strengths[6] = float[](
      evolving_strength(0, t, phase0),
      evolving_strength(1, t, phase1),
      evolving_strength(2, t, phase2),
      evolving_strength(3, t, phase3),
      evolving_strength(4, t, phase4),
      evolving_strength(5, t, phase5));
  int dominant_index = int(floor(random_value(46) * 6.0));
  int secondary_index = (dominant_index + 3) % 6;
  strengths[dominant_index] = 0.90 + 0.08 * sin(t * 0.13 + phase4 * 0.29);
  strengths[secondary_index] = max(
      strengths[secondary_index],
      0.68 + 0.07 * cos(t * 0.16 + phase1 * 0.21));

  vec2 residue0 = residue(0, strengths[0], t, phase0);
  vec2 residue1 = residue(1, strengths[1], t, phase1);
  vec2 residue2 = residue(2, strengths[2], t, phase2);
  vec2 residue3 = residue(3, strengths[3], t, phase3);
  vec2 residue4 = residue(4, strengths[4], t, phase4);
  vec2 residue5 = residue(5, strengths[5], t, phase5);

  vec2 fold_center =
      (pole1 + pole4) * 0.5 +
      0.055 * vec2(sin(t * 0.23 + phase2), cos(t * 0.19 + phase5));
  float fold_angle = phase0 * 0.37 + phase5 * 0.63 + 0.09 * sin(t * 0.18 + phase3);
  float fold_magnitude =
      (0.009 + 0.007 * random_value(47)) *
      (0.82 + 0.18 * sin(t * 0.24 + phase2 * 0.31));
  vec2 fold_residue = fold_magnitude * vec2(cos(fold_angle), sin(fold_angle));

  // A separate, regularized field contributes only a weak shear. Its centers
  // orbit independently and never enter the primary phase topology.
  float secondary_strength = clamp(
      0.060 + 0.020 * random_value(47) + 0.006 * sin(t * 0.15 + phase1),
      0.050,
      0.090);
  vec2 secondary_center0 =
      pole0 + 0.105 * vec2(sin(t * 0.17 + phase3), 0.76 * cos(t * 0.21 + phase5));
  vec2 secondary_center1 =
      pole2 + 0.115 * vec2(cos(t * 0.19 + phase5), 0.72 * sin(t * 0.16 + phase0));
  vec2 secondary_center2 =
      pole4 + 0.100 * vec2(sin(t * 0.14 + phase1), 0.80 * cos(t * 0.18 + phase3));
  vec2 secondary_residue0 = secondary_strength * complex_multiply(
      residue0,
      vec2(cos(phase2 * 0.17 + 0.40), sin(phase2 * 0.17 + 0.40)));
  vec2 secondary_residue1 = secondary_strength * complex_multiply(
      residue2,
      vec2(cos(phase4 * 0.19 + 1.10), sin(phase4 * 0.19 + 1.10)));
  vec2 secondary_residue2 = secondary_strength * complex_multiply(
      residue4,
      vec2(cos(phase0 * 0.23 + 1.80), sin(phase0 * 0.23 + 1.80)));

  v_poles01 = vec4(pole0, pole1);
  v_poles23 = vec4(pole2, pole3);
  v_poles45 = vec4(pole4, pole5);
  v_transform = vec4(bend, offset);
  v_residues01 = vec4(residue0, residue1);
  v_residues23 = vec4(residue2, residue3);
  v_residues45 = vec4(residue4, residue5);
  v_fold = vec4(fold_center, fold_residue);
  v_circulation03 = vec4(
      circulation(0, strengths[0], t, phase0),
      circulation(1, strengths[1], t, phase1),
      circulation(2, strengths[2], t, phase2),
      circulation(3, strengths[3], t, phase3));
  v_circulation45 = vec2(
      circulation(4, strengths[4], t, phase4),
      circulation(5, strengths[5], t, phase5));
  v_secondary0 = vec4(secondary_center0, secondary_residue0);
  v_secondary1 = vec4(secondary_center1, secondary_residue1);
  v_secondary2 = vec4(secondary_center2, secondary_residue2);
  v_dynamics = vec4(
      0.0045 + 0.0020 * random_value(47) + 0.0010 * sin(t * 0.19 + phase3 * 0.17),
      0.018 * random_signed(47),
      phase0 * 0.37 + phase3 * 0.63 + t * 0.22,
      phase2 * 0.61 + phase5 * 0.39 - t * 0.18);
}
)glsl";

inline constexpr auto fragment = R"glsl(#version 300 es

precision highp float;

uniform vec2 u_resolution;

flat in vec4 v_poles01;
flat in vec4 v_poles23;
flat in vec4 v_poles45;
flat in vec4 v_transform;
flat in vec4 v_residues01;
flat in vec4 v_residues23;
flat in vec4 v_residues45;
flat in vec4 v_fold;
flat in vec4 v_circulation03;
flat in vec2 v_circulation45;
flat in vec4 v_secondary0;
flat in vec4 v_secondary1;
flat in vec4 v_secondary2;
flat in vec4 v_dynamics;

out vec4 fragment_color;

const float line_density = 96.0;
const float phase_weight = 0.5;
const float tau = 6.283185307179586;

vec2 complex_multiply(vec2 a, vec2 b) {
  return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complex_divide(vec2 a, vec2 b) {
  return complex_multiply(a, vec2(b.x, -b.y)) / max(dot(b, b), 0.0001);
}

vec2 inverse_pole(vec2 z, vec2 center, vec2 coefficient, float core) {
  vec2 delta = z - center;
  vec2 inverse = vec2(delta.x, -delta.y) / (dot(delta, delta) + core);
  return complex_multiply(coefficient, inverse);
}

float contour(float value, float thickness) {
  float signal = sin(3.141592653589793 * value);
  float width = max(fwidth(signal), 0.0007);
  float line = 1.0 - smoothstep(width * thickness, width * (thickness + 0.95), abs(signal));
  float frequency_visibility =
      1.0 - 0.45 * smoothstep(0.45, 0.95, fwidth(value));
  return line * frequency_visibility;
}

float density_tone(float coordinate) {
  // For level-set contours, |gradient(coordinate)| is the reciprocal distance
  // to the neighboring line in pixels and the magnitude of the induced field.
  vec2 gradient = vec2(dFdx(coordinate), dFdy(coordinate));
  float inverse_spacing = length(gradient);
  float compression = inverse_spacing / (inverse_spacing + 0.15);
  return mix(0.68, 1.50, compression);
}

float pole_contrast(vec2 delta, vec2 residue) {
  float magnitude_squared = dot(residue, residue);
  vec2 direction = residue * inversesqrt(max(magnitude_squared, 0.000001));
  float along = dot(delta, direction);
  float across = dot(delta, vec2(-direction.y, direction.x));
  float flow_distance = 0.68 * along * along + 1.32 * across * across;
  float strength = smoothstep(0.0015, 0.0055, magnitude_squared);
  float center_contrast = mix(0.42, 0.66, strength);
  return mix(center_contrast, 1.0, smoothstep(0.0002, 0.0032, flow_distance));
}

void main() {
  vec2 uv = gl_FragCoord.xy / u_resolution;
  vec2 z = uv * 2.0 - 1.0;
  z.x *= u_resolution.x / u_resolution.y;
  z *= 1.08;

  // Two rotated, slowly drifting scales bend the domain without changing the
  // topology or introducing an isotropic noise texture.
  vec2 low_warp = vec2(
      sin(0.72 * z.x + 1.05 * z.y + v_dynamics.z * 0.23),
      sin(-0.91 * z.x + 0.64 * z.y + v_dynamics.w * 0.21));
  vec2 medium_warp = vec2(
      sin(2.40 * z.x - 1.70 * z.y + v_dynamics.w * 0.37),
      sin(1.90 * z.x + 2.60 * z.y - v_dynamics.z * 0.33));
  float low_variation = 0.5 * (low_warp.x + low_warp.y);
  float medium_variation = 0.5 * (medium_warp.x + medium_warp.y);
  vec2 q = z + 0.0090 * low_warp + 0.0032 * medium_warp;

  vec2 pole0 = v_poles01.xy;
  vec2 pole1 = v_poles01.zw;
  vec2 pole2 = v_poles23.xy;
  vec2 pole3 = v_poles23.zw;
  vec2 pole4 = v_poles45.xy;
  vec2 pole5 = v_poles45.zw;
  vec2 bend = v_transform.xy;
  vec2 offset = v_transform.zw;

  vec2 warped = complex_divide(q + offset, vec2(1.0, 0.0) + complex_multiply(bend, q));
  vec2 delta0 = warped - pole0;
  vec2 delta1 = warped - pole1;
  vec2 delta2 = warped - pole2;
  vec2 delta3 = warped - pole3;
  vec2 delta4 = warped - pole4;
  vec2 delta5 = warped - pole5;

  vec2 numerator = complex_multiply(complex_multiply(delta0, delta2), delta4);
  vec2 denominator = complex_multiply(complex_multiply(delta1, delta3), delta5);
  vec2 rational_map = complex_divide(numerator, denominator);
  float phase_turns = atan(rational_map.y, rational_map.x) / tau;

  vec2 field = warped * vec2(0.33, 0.38);
  field += inverse_pole(warped, pole0, v_residues01.xy, 0.0060);
  field += inverse_pole(warped, pole1, v_residues01.zw, 0.0065);
  field += inverse_pole(warped, pole2, v_residues23.xy, 0.0055);
  field += inverse_pole(warped, pole3, v_residues23.zw, 0.0070);
  field += inverse_pole(warped, pole4, v_residues45.xy, 0.0062);
  field += inverse_pole(warped, pole5, v_residues45.zw, 0.0058);
  field += inverse_pole(warped, v_fold.xy, v_fold.zw, 0.0120);
  vec2 secondary_field =
      inverse_pole(warped, v_secondary0.xy, v_secondary0.zw, 0.0220) +
      inverse_pole(warped, v_secondary1.xy, v_secondary1.zw, 0.0240) +
      inverse_pole(warped, v_secondary2.xy, v_secondary2.zw, 0.0200);
  field += secondary_field;

  float radius0 = 0.5 * log(dot(delta0, delta0) + 0.0060);
  float radius1 = 0.5 * log(dot(delta1, delta1) + 0.0065);
  float radius2 = 0.5 * log(dot(delta2, delta2) + 0.0055);
  float radius3 = 0.5 * log(dot(delta3, delta3) + 0.0070);
  float radius4 = 0.5 * log(dot(delta4, delta4) + 0.0062);
  float radius5 = 0.5 * log(dot(delta5, delta5) + 0.0058);
  float circulation =
      v_circulation03.x * radius0 -
      v_circulation03.y * radius1 +
      v_circulation03.z * radius2 -
      v_circulation03.w * radius3 +
      v_circulation45.x * radius4 -
      v_circulation45.y * radius5;

  float curvature =
      sin(1.75 * field.x + 0.80 * warped.y + v_dynamics.z) *
      sin(1.45 * field.y - 0.65 * warped.x + 0.20 * field.x * field.y + v_dynamics.w);
  float density_variation =
      sin(0.92 * field.x + 0.58 * warped.y + v_dynamics.z * 0.71) +
      0.62 * sin(1.31 * field.y - 0.43 * warped.x + v_dynamics.w * 0.67);
  float field_coupling = 0.105 + 0.007 * density_variation;
  float stream =
      phase_weight * phase_turns + 0.100 * warped.y + field_coupling * field.y + circulation +
      v_dynamics.x * curvature + v_dynamics.y;

  float phase_perturbation =
      0.075 * sin(0.68 * field.x + 1.08 * warped.y + v_dynamics.z * 0.29) +
      0.025 * sin(1.62 * field.y - 0.58 * field.x + v_dynamics.w * 0.27);
  float regular_coordinate =
      stream * line_density + 0.20 * curvature + 0.10 * density_variation +
      phase_perturbation;

  // The phase contributes exactly 48 levels per winding. Every modulation and
  // hierarchy period divides 48, preserving continuity across the phase seam.
  float spacing_phase = 0.62 * low_variation + 0.25 * curvature;
  float line_coordinate =
      regular_coordinate +
      0.13 * sin(tau * regular_coordinate / 16.0 + spacing_phase) +
      0.045 * sin(tau * regular_coordinate / 8.0 + 0.80 * medium_variation);

  float width_variation = clamp(
      1.0 + 0.10 * low_variation + 0.04 * medium_variation,
      0.84,
      1.16);
  float opacity_variation = clamp(
      0.92 +
      0.09 * (0.65 * low_variation + 0.25 * curvature + 0.10 * medium_variation) +
      0.035 * sin(tau * regular_coordinate / 12.0 + v_dynamics.w * 0.09),
      0.78,
      1.06);

  float fine_lines = contour(line_coordinate, 0.21 * width_variation);
  float middle_lines = contour(line_coordinate / 3.0, 0.18 * width_variation);
  float major_lines = contour(line_coordinate / 8.0, 0.15 * width_variation);

  float local_density = density_tone(line_coordinate);
  float singularity_contrast =
      pole_contrast(delta0, v_residues01.xy) *
      pole_contrast(delta1, v_residues01.zw) *
      pole_contrast(delta2, v_residues23.xy) *
      pole_contrast(delta3, v_residues23.zw) *
      pole_contrast(delta4, v_residues45.xy) *
      pole_contrast(delta5, v_residues45.zw);
  float structure = opacity_variation * local_density * singularity_contrast * (
      fine_lines * 0.0520 +
      middle_lines * 0.0320 +
      major_lines * 0.0740);

  float left_contrast = mix(0.74, 1.0, smoothstep(0.28, 0.52, uv.x));
  vec2 edge_in = smoothstep(vec2(0.0), vec2(0.055), uv);
  vec2 edge_out = smoothstep(vec2(0.0), vec2(0.055), 1.0 - uv);
  float edge_contrast =
      0.82 + 0.18 * edge_in.x * edge_in.y * edge_out.x * edge_out.y;

  float field_luminance = 1.68 * structure;
  float luminance =
      min(0.6200, 0.0314 + field_luminance * left_contrast * edge_contrast);
  fragment_color = vec4(vec3(luminance), 1.0);
}
)glsl";

} // namespace portfolio::shaders

#endif
