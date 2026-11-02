import { DISPLAY_GLSL, NOISE_GLSL, createTriangle, linkProgram, setDisplayUniforms } from "./heatGl.js";

// Adapted from sites/public/src/sections/Hero/heatAnalytic.js. The onboarding
// adaptation cools the field smoothly under the title and controls.
// The fallback heat field, for browsers without half-float render targets
// and for software rendering (there drawn once, still). No simulation: rising
// noise stands in for the thermals, held to the same band under the copy as
// the fluid (the caller's `reach`), and each pointer deposit spreads and
// cools as the point-source solution of the heat equation. It does not play
// the exit: the page draws it instead (HeatField.jsx exitLive()).

const LIFETIME = 4.5; // seconds a deposit is kept
const START_TIME = 14; // the composition shown when the field never steps

const VERTEX = `
precision highp float;
attribute vec2 aPosition;
void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }
`;

const fragment = (count) => `
precision highp float;
#define DEPOSITS ${count}
${NOISE_GLSL}
${DISPLAY_GLSL}
uniform vec4 uDeposits[DEPOSITS];
uniform vec4 uReach; // reach left, reach right, blend from, blend to (CSS px)

const float S0 = 26.0;   // initial radius of a deposit, CSS px
const float D = 520.0;   // diffusivity, CSS px^2 per second
const float COOL = 0.55; // Newtonian cooling, per second

float fbm(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 3; i++) {
    v += a * noise(p);
    p = p * 2.03 + vec2(1.7, 9.2);
    a *= 0.5;
  }
  return v;
}

float heatAt(vec2 px) {
  float m = max(mix(uReach.x, uReach.y, smoothstep(uReach.z, uReach.w, px.x)), 1.0);
  float x = px.x / m;
  float y = (uSize.y - px.y) / m; // height above the bottom edge, in reach
  float t = uTime;

  // Rounded warm masses over a warm floor, fading with height.
  vec2 flow = vec2(x * 2.4, y * 2.0 - t * 0.2);
  vec2 warp = vec2(fbm(flow + vec2(0.0, t * 0.05)), fbm(flow + vec2(5.3, t * 0.04)));
  float plume = fbm(flow + 1.4 * warp);
  float heat = 1.6 * (0.35 + 0.9 * smoothstep(0.3, 0.8, plume)) * exp(-(y * y) / 0.048);

  // A second, smaller warm mass by the painting balances the broad heat at
  // the foot. The title keeps the cool left side of this asymmetrical field.
  vec2 upper = (px / uSize - vec2(0.96, 0.20)) / vec2(0.28, 0.25);
  heat += 0.72 * exp(-dot(upper, upper)) * (0.65 + 0.65 * plume);

  for (int i = 0; i < DEPOSITS; i++) {
    vec4 s = uDeposits[i];
    if (s.w <= 0.0) continue;
    float variance = S0 * S0 + 2.0 * D * s.z;
    vec2 d = px - s.xy;
    heat += s.w * (S0 * S0 / variance) * exp(-dot(d, d) / (2.0 * variance) - COOL * s.z);
  }
  // A soft cool region protects the entry's contrast without rectangular
  // masks. Cursor heat remains visible in the open field around the words.
  for (int i = 0; i < COPY_BLOCKS; i++) {
    if (float(i) >= uCopyCount) break;
    vec4 box = uCopy[i];
    vec2 outside = max(max(box.xy - px, px - box.zw), vec2(0.0));
    float shelter = 1.0 - smoothstep(0.0, 60.0, length(outside));
    heat = mix(heat, min(heat, 0.30), shelter);
  }
  return heat;
}

void main() {
  gl_FragColor = display();
}
`;

export function createAnalyticField(gl) {
  const available = gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_VECTORS) || 16;
  // The display's own uniforms, the entrance's included, take about 24 vectors.
  const count = Math.max(8, Math.min(48, available - 24));
  const linked = linkProgram(gl, VERTEX, fragment(count));
  if (!linked) return null;
  const triangle = createTriangle(gl);
  const deposits = [];
  const data = new Float32Array(count * 4);
  let time = START_TIME;
  let reach = [1, 1, 0, 1];

  return {
    kind: "analytic",
    resize(width, height, quality, value) {
      const short = Math.min(width, height);
      const { left = short, right = short, from = 0, to = 1 } = value || {};
      reach = [left, right, from * width, Math.max(to, from + 0.01) * width];
      return false;
    },
    splat({ x, y, heat = 0 }) {
      if (heat <= 0) return;
      deposits.push({ x, y, strength: heat * 0.7, born: time });
      if (deposits.length > count) deposits.shift();
    },
    step(dt) {
      time += dt;
      while (deposits.length && time - deposits[0].born > LIFETIME) deposits.shift();
    },
    warm() {},
    scroll() {},
    render(view) {
      data.fill(0);
      deposits.forEach((d, i) => data.set([d.x, d.y, time - d.born, d.strength], i * 4));
      gl.useProgram(linked.program);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
      setDisplayUniforms(gl, linked.uniforms, { ...view, time });
      gl.uniform4fv(linked.uniforms.uDeposits, data);
      gl.uniform4fv(linked.uniforms.uReach, reach);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    },
    dispose() {
      gl.deleteBuffer(triangle);
      gl.deleteProgram(linked.program);
    },
  };
}
