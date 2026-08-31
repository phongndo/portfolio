#ifndef PORTFOLIO_SHADERS_HPP
#define PORTFOLIO_SHADERS_HPP

namespace portfolio::shaders {

inline constexpr auto vertex = R"glsl(#version 300 es

precision highp float;
precision highp int;

uniform vec2 u_resolution;
uniform vec2 u_poles[7];
uniform vec4 u_singularities[7];
uniform int u_center_count;

flat out vec4 v_poles01;
flat out vec4 v_poles23;
flat out vec4 v_poles45;
flat out vec4 v_pole6_residue6;
flat out vec4 v_transform;
flat out vec4 v_residues01;
flat out vec4 v_residues23;
flat out vec4 v_residues45;
flat out vec4 v_anisotropy0123;
flat out vec3 v_anisotropy456;
flat out vec4 v_radii0123;
flat out vec3 v_radii456;
flat out vec4 v_dynamics;
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

vec2 pole_position(int index) {
  vec2 position = u_poles[index];
  position.x *= u_resolution.x / u_resolution.y;
  return position;
}

vec2 residue(vec4 singularity) {
  float magnitude = 0.026 + 0.050 * singularity.x;
  return magnitude * vec2(cos(singularity.y), sin(singularity.y));
}

void main() {
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);

  vec2 pole0 = pole_position(0);
  vec2 pole1 = pole_position(1);
  vec2 pole2 = pole_position(2);
  vec2 pole3 = pole_position(3);
  vec2 pole4 = pole_position(4);
  vec2 pole5 = pole_position(5);
  vec2 pole6 = pole_position(6);

  vec4 singularity0 = u_singularities[0];
  vec4 singularity1 = u_singularities[1];
  vec4 singularity2 = u_singularities[2];
  vec4 singularity3 = u_singularities[3];
  vec4 singularity4 = u_singularities[4];
  vec4 singularity5 = u_singularities[5];
  vec4 singularity6 = u_singularities[6];

  vec2 residue0 = residue(singularity0);
  vec2 residue1 = residue(singularity1);
  vec2 residue2 = residue(singularity2);
  vec2 residue3 = residue(singularity3);
  vec2 residue4 = residue(singularity4);
  vec2 residue5 = residue(singularity5);
  vec2 residue6 = residue(singularity6);

  int last_index = max(u_center_count - 1, 0);
  vec2 last_pole = pole_position(last_index);
  vec4 last_singularity = u_singularities[last_index];
  vec2 bend = vec2(
      0.060 + 0.012 * (singularity0.z - 1.0),
      0.020 * (last_singularity.z - 1.0));
  vec2 offset = 0.006 * (pole0 + last_pole);

  v_poles01 = vec4(pole0, pole1);
  v_poles23 = vec4(pole2, pole3);
  v_poles45 = vec4(pole4, pole5);
  v_pole6_residue6 = vec4(pole6, residue6);
  v_transform = vec4(bend, offset);
  v_residues01 = vec4(residue0, residue1);
  v_residues23 = vec4(residue2, residue3);
  v_residues45 = vec4(residue4, residue5);
  v_anisotropy0123 = vec4(
      singularity0.z, singularity1.z, singularity2.z, singularity3.z);
  v_anisotropy456 = vec3(singularity4.z, singularity5.z, singularity6.z);
  v_radii0123 = vec4(
      singularity0.w, singularity1.w, singularity2.w, singularity3.w);
  v_radii456 = vec3(singularity4.w, singularity5.w, singularity6.w);
  v_dynamics = vec4(
      0.0045 + 0.0015 * 0.5 * (singularity0.x + last_singularity.x),
      0.012 * (singularity0.z - last_singularity.z),
      0.37 * singularity0.y + 0.63 * last_singularity.y + 0.08 * pole0.y,
      0.61 * singularity0.y - 0.39 * last_singularity.y + 0.07 * last_pole.x);
}
)glsl";

inline constexpr auto fragment = R"glsl(#version 300 es

precision highp float;
precision highp int;

uniform vec2 u_resolution;
uniform int u_center_count;

flat in vec4 v_poles01;
flat in vec4 v_poles23;
flat in vec4 v_poles45;
flat in vec4 v_pole6_residue6;
flat in vec4 v_transform;
flat in vec4 v_residues01;
flat in vec4 v_residues23;
flat in vec4 v_residues45;
flat in vec4 v_anisotropy0123;
flat in vec3 v_anisotropy456;
flat in vec4 v_radii0123;
flat in vec3 v_radii456;
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

float anisotropic_distance_squared(
    vec2 delta,
    vec2 orientation,
    float anisotropy) {
  float magnitude_squared = dot(orientation, orientation);
  vec2 direction = orientation * inversesqrt(max(magnitude_squared, 0.000001));
  float along = dot(delta, direction);
  float across = dot(delta, vec2(-direction.y, direction.x));
  float shape = clamp(anisotropy, 0.55, 1.80);
  return along * along / shape + across * across * shape;
}

vec2 inverse_pole(
    vec2 z,
    vec2 center,
    vec2 coefficient,
    float radius,
    float anisotropy) {
  vec2 delta = z - center;
  float distance_squared = anisotropic_distance_squared(delta, coefficient, anisotropy);
  vec2 inverse =
      vec2(delta.x, -delta.y) / (distance_squared + radius * radius);
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

float circulation_strength(vec2 residue, float radius, float anisotropy) {
  float strength = clamp((length(residue) - 0.026) / 0.050, 0.0, 1.0);
  float radius_variation = (radius - 0.085) * 0.018;
  float shape_variation = (anisotropy - 1.0) * 0.0015;
  return 0.0035 + 0.0070 * strength + radius_variation + shape_variation;
}

float pole_contrast(
    vec2 delta,
    vec2 residue,
    float radius,
    float anisotropy) {
  float magnitude_squared = dot(residue, residue);
  float flow_distance = anisotropic_distance_squared(delta, residue, anisotropy);
  float normalized_distance = flow_distance / max(radius * radius, 0.000001);
  float strength = smoothstep(0.0015, 0.0055, magnitude_squared);
  float center_contrast = mix(0.42, 0.66, strength);
  return mix(center_contrast, 1.0, smoothstep(0.035, 0.55, normalized_distance));
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
  vec2 pole6 = v_pole6_residue6.xy;
  vec2 bend = v_transform.xy;
  vec2 offset = v_transform.zw;

  vec2 warped = complex_divide(q + offset, vec2(1.0, 0.0) + complex_multiply(bend, q));
  vec2 delta0 = warped - pole0;
  vec2 delta1 = warped - pole1;
  vec2 delta2 = warped - pole2;
  vec2 delta3 = warped - pole3;
  vec2 delta4 = warped - pole4;
  vec2 delta5 = warped - pole5;
  vec2 delta6 = warped - pole6;

  vec2 numerator = delta0;
  vec2 denominator = vec2(1.0, 0.0);
  if (u_center_count > 1) {
    denominator = complex_multiply(denominator, delta1);
  }
  if (u_center_count > 2) {
    numerator = complex_multiply(numerator, delta2);
  }
  if (u_center_count > 3) {
    denominator = complex_multiply(denominator, delta3);
  }
  if (u_center_count > 4) {
    numerator = complex_multiply(numerator, delta4);
  }
  if (u_center_count > 5) {
    denominator = complex_multiply(denominator, delta5);
  }
  if (u_center_count > 6) {
    numerator = complex_multiply(numerator, delta6);
  }
  vec2 rational_map = complex_divide(numerator, denominator);
  float phase_turns = atan(rational_map.y, rational_map.x) / tau;

  vec2 field = warped * vec2(0.33, 0.38);
  field += inverse_pole(
      warped, pole0, v_residues01.xy, v_radii0123.x, v_anisotropy0123.x);
  if (u_center_count > 1) {
    field += inverse_pole(
        warped, pole1, v_residues01.zw, v_radii0123.y, v_anisotropy0123.y);
  }
  if (u_center_count > 2) {
    field += inverse_pole(
        warped, pole2, v_residues23.xy, v_radii0123.z, v_anisotropy0123.z);
  }
  if (u_center_count > 3) {
    field += inverse_pole(
        warped, pole3, v_residues23.zw, v_radii0123.w, v_anisotropy0123.w);
  }
  if (u_center_count > 4) {
    field += inverse_pole(
        warped, pole4, v_residues45.xy, v_radii456.x, v_anisotropy456.x);
  }
  if (u_center_count > 5) {
    field += inverse_pole(
        warped, pole5, v_residues45.zw, v_radii456.y, v_anisotropy456.y);
  }
  if (u_center_count > 6) {
    field += inverse_pole(
        warped, pole6, v_pole6_residue6.zw, v_radii456.z, v_anisotropy456.z);
  }

  float radial0 = 0.5 * log(
      anisotropic_distance_squared(delta0, v_residues01.xy, v_anisotropy0123.x) +
      v_radii0123.x * v_radii0123.x);
  float radial1 = 0.5 * log(
      anisotropic_distance_squared(delta1, v_residues01.zw, v_anisotropy0123.y) +
      v_radii0123.y * v_radii0123.y);
  float radial2 = 0.5 * log(
      anisotropic_distance_squared(delta2, v_residues23.xy, v_anisotropy0123.z) +
      v_radii0123.z * v_radii0123.z);
  float radial3 = 0.5 * log(
      anisotropic_distance_squared(delta3, v_residues23.zw, v_anisotropy0123.w) +
      v_radii0123.w * v_radii0123.w);
  float radial4 = 0.5 * log(
      anisotropic_distance_squared(delta4, v_residues45.xy, v_anisotropy456.x) +
      v_radii456.x * v_radii456.x);
  float radial5 = 0.5 * log(
      anisotropic_distance_squared(delta5, v_residues45.zw, v_anisotropy456.y) +
      v_radii456.y * v_radii456.y);
  float radial6 = 0.5 * log(
      anisotropic_distance_squared(delta6, v_pole6_residue6.zw, v_anisotropy456.z) +
      v_radii456.z * v_radii456.z);
  float circulation = circulation_strength(
      v_residues01.xy, v_radii0123.x, v_anisotropy0123.x) * radial0;
  if (u_center_count > 1) {
    circulation -= circulation_strength(
        v_residues01.zw, v_radii0123.y, v_anisotropy0123.y) * radial1;
  }
  if (u_center_count > 2) {
    circulation += circulation_strength(
        v_residues23.xy, v_radii0123.z, v_anisotropy0123.z) * radial2;
  }
  if (u_center_count > 3) {
    circulation -= circulation_strength(
        v_residues23.zw, v_radii0123.w, v_anisotropy0123.w) * radial3;
  }
  if (u_center_count > 4) {
    circulation += circulation_strength(
        v_residues45.xy, v_radii456.x, v_anisotropy456.x) * radial4;
  }
  if (u_center_count > 5) {
    circulation -= circulation_strength(
        v_residues45.zw, v_radii456.y, v_anisotropy456.y) * radial5;
  }
  if (u_center_count > 6) {
    circulation += circulation_strength(
        v_pole6_residue6.zw, v_radii456.z, v_anisotropy456.z) * radial6;
  }

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
  float singularity_contrast = pole_contrast(
      delta0, v_residues01.xy, v_radii0123.x, v_anisotropy0123.x);
  if (u_center_count > 1) {
    singularity_contrast *= pole_contrast(
        delta1, v_residues01.zw, v_radii0123.y, v_anisotropy0123.y);
  }
  if (u_center_count > 2) {
    singularity_contrast *= pole_contrast(
        delta2, v_residues23.xy, v_radii0123.z, v_anisotropy0123.z);
  }
  if (u_center_count > 3) {
    singularity_contrast *= pole_contrast(
        delta3, v_residues23.zw, v_radii0123.w, v_anisotropy0123.w);
  }
  if (u_center_count > 4) {
    singularity_contrast *= pole_contrast(
        delta4, v_residues45.xy, v_radii456.x, v_anisotropy456.x);
  }
  if (u_center_count > 5) {
    singularity_contrast *= pole_contrast(
        delta5, v_residues45.zw, v_radii456.y, v_anisotropy456.y);
  }
  if (u_center_count > 6) {
    singularity_contrast *= pole_contrast(
        delta6, v_pole6_residue6.zw, v_radii456.z, v_anisotropy456.z);
  }
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
