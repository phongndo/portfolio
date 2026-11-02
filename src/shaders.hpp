#ifndef PORTFOLIO_SHADERS_HPP
#define PORTFOLIO_SHADERS_HPP

namespace portfolio::shaders {

inline constexpr auto vertex = R"glsl(#version 300 es

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main() {
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)glsl";

// A three-dimensional slice of a four-dimensional surface, the zero set of
//   F(x, y, z, w) = G(x, y, z) cos w + P(x, y, z) sin w,
// where G is Schoen's gyroid, sin x cos y + sin y cos z + sin z cos x, and P
// is Schwarz's P surface, cos x + cos y + cos z, scaled to the same range.
// The slice moves through the fourth dimension: it advances along w and tilts
// in the x-w and y-w planes, so different parts of it sit at different w and
// the surface turns from one shape into the other in waves across it. It is
// drawn as a thin sheet cut to a sphere that holds still, unless the visitor
// turns it; the motion through the fourth dimension turns with it.
inline constexpr auto fragment = R"glsl(#version 300 es

precision highp float;

uniform vec2 u_resolution;
// Seconds since an arbitrary start; it stands still when motion is reduced.
uniform float u_time;
// The right edge of the left-aligned text column, as a fraction of the width.
uniform float u_text_edge;
// The sculpture's orientation, a rotation from its own axes to the view's,
// built from the quaternion the visitor turns by dragging.
uniform mat3 u_orientation;
// How far the page has scrolled, in viewport heights.
uniform float u_scroll;

out vec4 fragment_color;

// The surfaces' frequency inside the unit sphere, and half the sheet's
// thickness in units of F.
const float frequency = 5.6;
const float sheet = 0.11;
const float camera_distance = 3.3;
const float focal_length = 2.05;
const int march_steps = 140;

float gyroid(vec3 point) {
  return dot(sin(point), cos(point.yzx));
}

float schwarz(vec3 point) {
  return 0.5 * (cos(point.x) + cos(point.y) + cos(point.z));
}

// The slice's place in four dimensions: cos and sin of its tilts in the x-w
// and y-w planes, and its offset along w.
vec4 tilt;
float offset_w;

float field(vec3 point) {
  float x = point.x * tilt.x;
  float w = point.x * tilt.y;
  float y = point.y * tilt.z - w * tilt.w;
  w = point.y * tilt.w + w * tilt.z + offset_w;
  vec3 slice = vec3(x, y, point.z);
  return gyroid(slice) * cos(w) + schwarz(slice) * sin(w);
}

// A distance bound to the sheet, and to the sheet cut by the unit sphere.
// F's gradient is bounded and the tilt is a rotation, so a fraction of F's
// distance from the sheet is a safe step.
float sheet_distance(vec3 point) {
  return 0.4 * (abs(field(frequency * point)) - sheet) / frequency;
}

float sculpture(vec3 point) {
  return max(sheet_distance(point), length(point) - 1.0);
}

vec3 sheet_normal(vec3 point) {
  vec3 at = frequency * point;
  const vec2 h = vec2(0.002, 0.0);
  vec3 gradient = vec3(field(at + h.xyy) - field(at - h.xyy), field(at + h.yxy) - field(at - h.yxy),
                       field(at + h.yyx) - field(at - h.yyx));
  return normalize(sign(field(at)) * gradient);
}

void main() {
  vec2 uv = gl_FragCoord.xy / u_resolution;
  float time = u_time;
  offset_w = 0.16 * time;
  float tilt_x = 0.60 * sin(0.070 * time);
  float tilt_y = 0.50 * sin(0.053 * time + 1.3);
  tilt = vec4(cos(tilt_x), sin(tilt_x), cos(tilt_y), sin(tilt_y));

  // Beside the text on wide screens. On narrow ones the text fills the width,
  // so the sculpture sits in the upper half, under the name, sized to the
  // width; the text starts below it and scrolls over it.
  float narrow = smoothstep(0.62, 0.80, u_text_edge);
  float aspect = u_resolution.x / u_resolution.y;
  vec2 center = mix(vec2(0.5 * (u_text_edge + 1.0) + 0.02, 0.5), vec2(0.5, 0.63), narrow);
  float focal = focal_length * mix(1.0, 1.22 * aspect, narrow);
  vec2 screen = (gl_FragCoord.xy - center * u_resolution) / (0.5 * u_resolution.y);

  // The camera looks down -z from +z. Rather than turn the sculpture, carry
  // the camera into the sculpture's own axes, where its surface is defined.
  mat3 to_sculpture = transpose(u_orientation);
  vec3 eye = to_sculpture * vec3(0.0, 0.0, camera_distance);
  vec3 right = to_sculpture * vec3(1.0, 0.0, 0.0);
  vec3 up = to_sculpture * vec3(0.0, 1.0, 0.0);
  vec3 forward = to_sculpture * vec3(0.0, 0.0, -1.0);
  vec3 direction = normalize(focal * forward + screen.x * right + screen.y * up);

  // March only inside the sphere that bounds the sculpture. Remember where
  // the ray passed closest to the surface, relative to a pixel's width there,
  // so silhouettes can be anti-aliased.
  float along = -dot(eye, direction);
  float miss_squared = dot(eye, eye) - along * along;
  float pixel = 2.0 / (u_resolution.y * focal);
  float shade = 0.0;
  float halo = exp(-max(sqrt(max(miss_squared, 0.0)) - 1.0, 0.0) * 3.2);
  if (miss_squared < 1.0) {
    float depth = sqrt(1.0 - miss_squared);
    float entry = along - depth;
    float exit = along + depth;
    float distance = entry;
    bool hit = false;
    float closest = 1.0e9;
    float closest_distance = entry;
    for (int index = 0; index < march_steps; ++index) {
      float step_length = sculpture(eye + direction * distance);
      float in_pixels = step_length / (pixel * distance);
      if (in_pixels < closest) {
        closest = in_pixels;
        closest_distance = distance;
      }
      if (step_length < 0.0003) {
        hit = true;
        break;
      }
      distance += step_length;
      if (distance > exit) {
        break;
      }
    }

    float coverage = hit ? 1.0 : 1.0 - smoothstep(0.0, 1.0, closest);
    if (coverage > 0.0) {
      float reached = hit ? distance : closest_distance;
      vec3 point = eye + direction * reached;
      // On the sphere's cut faces the normal is the sphere's; elsewhere it is
      // the sheet's, facing the side the ray came from.
      vec3 normal = length(point) - 1.0 > sheet_distance(point) ? normalize(point)
                                                                 : sheet_normal(point);
      if (dot(normal, direction) > 0.0) {
        normal = -normal;
      }

      // Lights move with the camera: a dim key from the upper left, and a
      // light behind the sculpture that catches its edges.
      vec3 key = normalize(-0.55 * right + 0.70 * up - 0.45 * forward);
      vec3 back = normalize(0.45 * right + 0.35 * up + 0.85 * forward);
      // Occlusion: how open the space is just off the surface.
      float occlusion = 0.0;
      for (int sample_index = 1; sample_index <= 4; ++sample_index) {
        float offset = 0.03 * float(sample_index);
        occlusion += max(offset - sculpture(point + normal * offset), 0.0) / offset;
      }
      occlusion = clamp(1.0 - 0.34 * occlusion, 0.0, 1.0);
      float diffuse = max(dot(normal, key), 0.0);
      float facing = max(dot(normal, -direction), 0.0);
      float rim = pow(1.0 - facing, 3.0) * (0.35 + 0.65 * max(dot(normal, back) + 0.4, 0.0));
      float sheen = pow(max(dot(reflect(direction, normal), key), 0.0), 10.0);
      // Surfaces deeper inside the sphere sit in shadow.
      float inside = exp(-1.1 * (reached - entry));
      shade = coverage * occlusion *
          ((0.012 + 0.30 * diffuse * diffuse + 0.05 * sheen) * (0.35 + 0.65 * inside) +
           0.34 * rim);
    }
  }

  // The sculpture recedes behind the text: beside the column on wide screens,
  // and as the text scrolls over it on narrow ones. Everything fades toward
  // the frame, over a faint glow around the sculpture.
  float beside = mix(0.30, 1.0, smoothstep(u_text_edge - 0.04, u_text_edge + 0.14, uv.x));
  float under = mix(1.0, 0.28, smoothstep(0.06, 0.45, u_scroll));
  float clearing = mix(beside, under, narrow);
  vec2 edge_in = smoothstep(vec2(0.0), vec2(0.08), uv);
  vec2 edge_out = smoothstep(vec2(0.0), vec2(0.08), 1.0 - uv);
  float frame = 0.65 + 0.35 * edge_in.x * edge_in.y * edge_out.x * edge_out.y;
  float background = 0.022 + 0.020 * halo * clearing;
  fragment_color = vec4(vec3((background + shade * clearing) * frame), 1.0);
}
)glsl";

} // namespace portfolio::shaders

#endif
