import {
  DISPLAY_GLSL,
  NOISE_GLSL,
  PAINTING_HEAT_GLSL,
  createTriangle,
  linkProgram,
  setDisplayUniforms,
} from "./heatGl.js";

// The heat field as a fluid: incompressible Navier-Stokes in the Boussinesq
// approximation, solved on the GPU with Stam's stable-fluids scheme. Each
// step adds heat at the thermals, lets temperature push the air upward
// (buoyancy) under a weak cross-draft, keeps a little of the swirl numerical
// diffusion removes (vorticity confinement), projects the velocity onto its
// divergence-free part (a Jacobi pressure solve), carries velocity and
// temperature along the flow (semi-Lagrangian advection) while both lose
// energy (viscous dissipation, cooling), then conducts the heat (the heat
// equation's Laplacian term). The temperature grid is coarse and conductive
// on purpose: the picture is warm air, soft at every edge, never flame.
// Velocities are in grid cells per second, y up; the forces below are set
// for the finest grid (REFERENCE_GRID cells on the short side) and scaled
// with the grid, so a coarser quality step moves the same way.

// Grid sizes and the Jacobi iteration count come from the caller's quality
// step (HeatField.jsx): { sim, heat, iterations }, cells on the short side.
// Lengths in the shaders are in units of the frame's short side, so the
// field keeps its proportions from a phone held upright to a wide monitor.
const PRESSURE_DECAY = 0.8; // warm start for the next solve
// The air is viscous and the heat holds, so a warm mass moves and cools
// slowly and keeps together, like a body rather than smoke.
const VELOCITY_DISSIPATION = 0.95; // per second
const HEAT_DISSIPATION = 0.15; // cooling, per second
// The pointer's heat cools faster than the thermals', so a trace follows the
// pointer, rises and fades instead of building up behind the copy while a
// reader moves the mouse over it.
const POINTER_COOLING = 0.65; // per second
// Cooling grows with height, rate × (1 + LAPSE · y²): the air above the
// plate is cooler, so thermals spend their heat low in the frame and the
// upper part, where the copy sits, stays ink. y is height over the caller's
// `reach`, which follows the room left under the copy in each column. The
// lapse is high against the slow HEAT_DISSIPATION, so the heat holds long
// near the floor and still stops under the copy.
const LAPSE = 35;
// Under a solid header the air in a band below its edge is cooled fast:
// warm cells there cool within about a third of a second, and when the bar
// clears the heat flows back on its own.
const HEADER_SINK = 10; // per second, at the bar's edge
const HEADER_BAND = 56; // CSS px below the edge over which the sink fades out
// The thermals' strength per column, as a share of the full strength: the
// title's column on the left runs warmer, the statement's column on the
// right, where the actions are, runs cooler, so its thermals stay low and
// soft. Blended across the same span as the reach; one column on a narrow
// screen takes the full strength.
const COLUMN_STRENGTH = [1.15, 0.5];
const BUOYANCY = 19; // thermals: cells per second squared per unit temperature
const POINTER_BUOYANCY = 6; // the pointer's heat rises slower, as a drifting trace
const CONDUCTION = 0.17; // one explicit heat-equation pass per 1/60 s, stable below 0.25
const DRAFT = 2.5; // cross-draft, cells per second squared
const VORTICITY = 5;
const FLOOR_HEAT = 0.15; // the heated plate along the bottom edge, per second
// Scrolling drags the air: scrolling down draws the warm air up after the
// page, scrolling up pushes it back down, and either stirs it sideways.
const WIND = 0.12; // cells per second squared per CSS px per second of scroll
const STIR = 0.6; // sideways share of the scroll's draw
const REFERENCE_GRID = 112;
const POINTER_FORCE = 0.06; // share of the pointer's velocity given to the air
const POINTER_RADIUS = 0.004;
const HEAT_RADIUS = 0.007;

// Under load (the opening's exit, exit.js) the laws above do not change;
// the conditions do. The air exchanges heat with the enclosure it fills, at
// the load's `exchange` rate, toward the enclosure's temperature
// (enclosureAt() below), and is buoyant against it; at rest there is no
// exchange and the air cools toward cold ink. The exchange is slow, so the
// thermals keep their heat as they rise through the whole frame and the air
// between them warms in their wake. With the load the plate heats harder,
// so the floor is warm from edge to edge, and the thermals run wider and
// hotter; the machine's parts warm in the medium itself, each a bloom that
// swells where it sits and rises (LOAD_SLOTS), higher up the frame as the
// load grows; the air lifts more readily (LOAD_LIFT); the lapse fades (a
// warm enclosure has no cool air above the plate), the two columns even
// out, the header's sink stops and the top of the frame closes; as the load
// holds, the sources settle.
const LOAD_PLATE = 2; // extra plate heat at full load, × FLOOR_HEAT
const LOAD_PUFFS = 0.8; // extra thermal heat at full load, × their strength
const LOAD_SWELL = 0.35; // the thermals are this much wider at full load
const LOAD_LIFT = 0.4; // the buoyant lift is this much stronger at full load
// As the medium settles the pointer's last trace cools this much faster, so
// it goes on cooling into the medium as the medium does.
const TRACE_SETTLE = 0.8; // per second
// As the load holds (exit.js hold), the heat in the frame drifts into the
// matter of the room, which is the painting: each place holds heat in
// proportion to the painting's light there (its capacity, heatGl.js
// PAINTING_HEAT_GLSL). The heat moves toward wherever it is short of its
// share: heat and capacity are averaged over regions 16 cells a side, the
// log of their ratio is eased over neighbouring regions, and its slope is
// the drift, a smooth velocity field on the 4 × 4 grid. Across each face
// between two cells the heat flows with the drift at the face, out of one
// cell and into the other, so it is carried, never made or lost, and never
// more than DRIFT_CFL of a cell's heat crosses one face in a pass; where
// every region holds its share the drift stops of itself. Within a region
// the heat finds its places by conduction, which evens out the heat per
// unit of capacity rather than the heat (Fourier's law with the harmonic
// mean of two neighbours' capacities as their conductance), so the heat
// comes to rest in proportion to capacity, most in the documents; the
// buoyant lift gives way (DRIFT_LIFT) so it stays where it has settled.
// That conduction is explicit, at SETTLE_RATE, half its stability limit
// (0.125: a neighbour's conductance can reach twice the cell's capacity,
// and near the limit the scheme rings from cell to cell), in SETTLE_PASSES
// passes a step, so the medium conducts as fast as it does at rest.
const DRIFT = 1200; // cells per second, per unit slope of the shortfall per cell, finest grid
const DRIFT_CFL = 0.22; // of a cell's heat through one face in one pass
const DRIFT_PASSES = 3; // passes per 1/60 s when the load holds
const DRIFT_LIFT = 0.8;
const SETTLE_RATE = 0.06; // the limit is 0.125
const SETTLE_PASSES = 4; // per 1/60 s when the load holds: 0.24 a step, as CONDUCTION at rest
// Over the painting a place gives its heat to the room at a rate that falls
// with its capacity to this power (1 for a uniform surface conductance;
// less, so the documents still cool within the runway).
const HOLD_BY_CAPACITY = 0.35;
// The heat as the display sees it, measured on a coarse grid of about this
// many CSS px, so the page can place the copy's masks on the real heat and
// the exit can keep the heat in the frame to its measure (HeatField.jsx).
const PROBE_CELL = 30;
const PROBE_RANGE = 4; // the heat the probe's mean reads up to

// Temperature has two channels that move and conduct together: the air
// the thermals heat (red, cooled faster with height) and the heat the pointer
// leaves (green, cooled evenly), so a trace drawn high in the frame rises and
// fades at its own pace while the thermals stay low.

// Thermals. A few puffs of heat are alive at once, each born just below the
// bottom edge at a random place, with a random width and strength; it swells
// and fades over its life, then the next one starts elsewhere after a short
// random pause. Buoyancy lifts each puff off as a bubble with a rolled cap,
// so the field never settles into fixed columns and no two visits look the
// same.
const PUFFS = 9;
// The last LOAD_SLOTS of the slots are the machine's parts: they bloom in
// the medium itself, anywhere up to the height the load has reached when
// they start (BLOOM_REACH of the frame at full load), apart from the other
// sources, with their strength times the load.
const LOAD_SLOTS = 5;
const BLOOM_REACH = 0.85; // frame heights
const BLOOM_STRENGTH = 0.55; // × PUFF_STRENGTH
const BLOOM_SPACING = 0.3; // short-side units between the centres of live sources
const PUFF_LIFE = [2.6, 4.2]; // seconds
const PUFF_RISE = 0.22; // share of a puff's life spent heating up
const PUFF_GAP = [0.3, 1.2]; // seconds before the next puff in a slot
const PUFF_RADIUS = [0.12, 0.19]; // short-side units
const PUFF_STRENGTH = [1.8, 2.6]; // heat per second at the centre
const PUFF_SPACING = 0.22; // minimum distance from other live puffs, width fraction
const PUFF_DEPTH = 0.05; // centre below the bottom edge, short-side units
// On a phone the frame is narrow, so fewer, wider thermals keep apart.
const NARROW = 620; // CSS px
const NARROW_WIDTH = 1.4; // × PUFF_RADIUS
const NARROW_STRENGTH = 0.75; // × PUFF_STRENGTH: wide puffs overlap across a narrow frame
const NARROW_SPACING = 0.3;
const NARROW_BLOOMS = 3; // of the LOAD_SLOTS: a narrow frame fills with fewer

const VERTEX = `
precision highp float;
attribute vec2 aPosition;
uniform vec2 texelSize;
varying vec2 vUv;
varying vec2 vL;
varying vec2 vR;
varying vec2 vT;
varying vec2 vB;
void main() {
  vUv = aPosition * 0.5 + 0.5;
  vL = vUv - vec2(texelSize.x, 0.0);
  vR = vUv + vec2(texelSize.x, 0.0);
  vT = vUv + vec2(0.0, texelSize.y);
  vB = vUv - vec2(0.0, texelSize.y);
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
`;

const HEADER = `
precision highp float;
precision highp sampler2D;
varying vec2 vUv;
varying vec2 vL;
varying vec2 vR;
varying vec2 vT;
varying vec2 vB;
`;

// A number as a GLSL float literal (a whole number written bare is an int,
// which GLSL ES will not mix with floats).
const glsl = (value) => value.toFixed(4);

// The enclosure's temperature under load (exit.js conditions()). Working,
// it is the heat the machine at the floor conducts up through it, as the
// heat equation gives over a heated plate, erfc(height / depth) of the
// floor's temperature, evening out into the operating temperature as it
// settles, the top first and the floor last. Resting (`cooling`, 0 to 1) it
// is the room: a resting warmth (`room`) times each place's capacity where
// there is a painting (`paintingOn`; `paintingBox`: where the painting lies
// over the frame, as fractions of it, y down).
const ENCLOSURE_GLSL = `${PAINTING_HEAT_GLSL}
uniform vec4 enclosure; // floor temperature, depth (frame heights), settle (0 to 1), operating temperature
uniform sampler2D uPaintingHeat;
uniform vec4 paintingBox;
uniform float paintingOn;
uniform float cooling;
uniform float room;
// erfc for z >= 0 (Abramowitz and Stegun 7.1.26, within 1.5e-7).
float erfcPositive(float z) {
  float t = 1.0 / (1.0 + 0.3275911 * z);
  return t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) * exp(-z * z);
}
float paintingWarmth(vec2 uv) {
  vec2 p = vec2((uv.x - paintingBox.x) / paintingBox.z, (1.0 - uv.y - paintingBox.y) / paintingBox.w);
  return paintingHeatAt(uPaintingHeat, p);
}
float enclosureAt(vec2 uv) {
  float heated = enclosure.x * erfcPositive(uv.y / max(enclosure.y, 0.001));
  float evened = smoothstep(0.0, 1.0, clamp(1.5 * enclosure.z - 0.5 * (1.0 - uv.y), 0.0, 1.0));
  float working = mix(heated, enclosure.w, evened);
  float resting = room * (paintingOn > 0.5 ? paintingWarmth(uv) : 1.0);
  return mix(working, resting, cooling);
}
`;

const SHADERS = {
  sources: `${HEADER}${NOISE_GLSL}
#define PUFFS ${PUFFS}
uniform sampler2D uTarget;
uniform vec2 unit;
uniform float dt;
uniform float time;
uniform vec4 uSources[PUFFS];
uniform float floorHeat;
uniform vec2 strength; // left and right column, as shares of the full strength
uniform vec2 split;    // where the left column blends into the right
void main() {
  vec2 p = vUv * unit;
  float heat = 0.0;
  for (int i = 0; i < PUFFS; i++) {
    vec2 d = p - uSources[i].xy;
    heat += uSources[i].w * exp(-dot(d, d) / (uSources[i].z * uSources[i].z));
  }
  heat *= 0.7 + 0.6 * noise(vec2(p.x * 4.0, time * 0.35));
  // A heated plate along the bottom edge. Its temperature varies a little
  // along the edge and in time, which seeds thermals that part from it at
  // their own spacing and rise with rolled caps, as in Rayleigh-Bénard
  // convection.
  float plate = exp(-p.y / 0.035) * (0.35 + noise(vec2(p.x * 7.0, time * 0.15)));
  heat += floorHeat * plate;
  heat *= mix(strength.x, strength.y, smoothstep(split.x, split.y, vUv.x));
  vec2 held = texture2D(uTarget, vUv).xy;
  gl_FragColor = vec4(held.x + heat * dt, held.y, 0.0, 1.0);
}`,
  splat: `${HEADER}
uniform sampler2D uTarget;
uniform vec2 unit;
uniform vec3 color;
uniform vec2 point;
uniform float radius;
void main() {
  vec2 p = (vUv - point) * unit;
  gl_FragColor = vec4(texture2D(uTarget, vUv).xyz + exp(-dot(p, p) / radius) * color, 1.0);
}`,
  forces: `${HEADER}${NOISE_GLSL}${ENCLOSURE_GLSL}
uniform sampler2D uVelocity;
uniform sampler2D uTemperature;
uniform vec2 unit;
uniform float dt;
uniform float time;
uniform float buoyancy;
uniform float pointerBuoyancy;
uniform float draft;
uniform float wind;
uniform float stir;
uniform float loaded; // under load the air is buoyant against the enclosure, at rest against cold ink
void main() {
  vec2 velocity = texture2D(uVelocity, vUv).xy;
  vec2 heat = texture2D(uTemperature, vUv).xy;
  float around = loaded > 0.0 ? loaded * enclosureAt(vUv) : 0.0;
  velocity.y += dt * (buoyancy * (heat.x - around) + pointerBuoyancy * heat.y);
  vec2 q = vUv * unit * 2.2;
  velocity.x += dt * draft * (noise(q + vec2(time * 0.07, -time * 0.05)) - 0.5);
  // Scrolling draws the air along, unevenly, so the warm masses stretch and mix.
  velocity.y += dt * wind * (0.4 + 1.2 * noise(q * 1.7 + vec2(3.1, time * 0.2)));
  velocity.x += dt * abs(wind) * stir * (2.0 * noise(q * 1.3 + vec2(7.7, -time * 0.3)) - 1.0);
  gl_FragColor = vec4(velocity, 0.0, 1.0);
}`,
  curl: `${HEADER}
uniform sampler2D uVelocity;
void main() {
  float L = texture2D(uVelocity, vL).y;
  float R = texture2D(uVelocity, vR).y;
  float T = texture2D(uVelocity, vT).x;
  float B = texture2D(uVelocity, vB).x;
  gl_FragColor = vec4(0.5 * (R - L - T + B), 0.0, 0.0, 1.0);
}`,
  vorticity: `${HEADER}
uniform sampler2D uVelocity;
uniform sampler2D uCurl;
uniform float curl;
uniform float dt;
void main() {
  float L = texture2D(uCurl, vL).x;
  float R = texture2D(uCurl, vR).x;
  float T = texture2D(uCurl, vT).x;
  float B = texture2D(uCurl, vB).x;
  float C = texture2D(uCurl, vUv).x;
  vec2 force = 0.5 * vec2(abs(T) - abs(B), abs(R) - abs(L));
  force /= length(force) + 0.0001;
  force *= curl * C;
  force.y *= -1.0;
  vec2 velocity = texture2D(uVelocity, vUv).xy + force * dt;
  gl_FragColor = vec4(clamp(velocity, -1000.0, 1000.0), 0.0, 1.0);
}`,
  // Walls reflect at the sides and the floor; the top is open at rest, so
  // warm air leaves the frame instead of pooling under the header, and
  // closes under load (`closed`).
  divergence: `${HEADER}
uniform sampler2D uVelocity;
uniform float closed; // 0: the top is open; 1: a lid (under load)
void main() {
  float L = texture2D(uVelocity, vL).x;
  float R = texture2D(uVelocity, vR).x;
  float T = texture2D(uVelocity, vT).y;
  float B = texture2D(uVelocity, vB).y;
  vec2 C = texture2D(uVelocity, vUv).xy;
  if (vL.x < 0.0) L = -C.x;
  if (vR.x > 1.0) R = -C.x;
  if (vB.y < 0.0) B = -C.y;
  if (vT.y > 1.0) T = mix(C.y, -C.y, closed);
  gl_FragColor = vec4(0.5 * (R - L + T - B), 0.0, 0.0, 1.0);
}`,
  clear: `${HEADER}
uniform sampler2D uTexture;
uniform float value;
void main() {
  gl_FragColor = value * texture2D(uTexture, vUv);
}`,
  pressure: `${HEADER}
uniform sampler2D uPressure;
uniform sampler2D uDivergence;
void main() {
  float L = texture2D(uPressure, vL).x;
  float R = texture2D(uPressure, vR).x;
  float T = texture2D(uPressure, vT).x;
  float B = texture2D(uPressure, vB).x;
  float divergence = texture2D(uDivergence, vUv).x;
  gl_FragColor = vec4((L + R + B + T - divergence) * 0.25, 0.0, 0.0, 1.0);
}`,
  gradient: `${HEADER}
uniform sampler2D uPressure;
uniform sampler2D uVelocity;
void main() {
  float L = texture2D(uPressure, vL).x;
  float R = texture2D(uPressure, vR).x;
  float T = texture2D(uPressure, vT).x;
  float B = texture2D(uPressure, vB).x;
  vec2 velocity = texture2D(uVelocity, vUv).xy - vec2(R - L, T - B);
  gl_FragColor = vec4(velocity, 0.0, 1.0);
}`,
  advection: `${HEADER}${ENCLOSURE_GLSL}
uniform sampler2D uVelocity;
uniform sampler2D uSource;
uniform vec2 velocityTexel;
uniform float dt;
uniform float dissipation;
uniform float dissipation2; // the second channel's rate (the pointer's heat)
uniform float lapse;
uniform vec2 height; // frame height over the reach, left and right column
uniform vec2 split; // where the left column's reach blends into the right's
uniform float sink;   // extra cooling under a solid header, per second
uniform vec2 ceiling; // the header's edge and the sink's band, in uv (y up)
uniform float exchange; // the first channel's exchange with the enclosure, per second (0 at rest)
uniform float gathered; // heat held in the room's matter, moved by conservative face fluxes
uniform float trim; // the frame's heat kept to the exit's measure, a factor for this step (1 at rest)
#define HOLD_BY_CAPACITY ${glsl(HOLD_BY_CAPACITY)}
void main() {
  vec2 coord = vUv - dt * texture2D(uVelocity, vUv).xy * velocityTexel;
  float y = vUv.y * mix(height.x, height.y, smoothstep(split.x, split.y, vUv.x));
  vec3 rate = vec3(dissipation * (1.0 + lapse * y * y), dissipation2, dissipation);
  rate.xy += sink * smoothstep(ceiling.x - ceiling.y, ceiling.x, vUv.y);
  // Newton's law, implicit in time (stable at any rate): the decay toward
  // cold ink, and the exchange with the enclosure toward its temperature.
  // As the room rests, what it exchanges is its matter's temperature, the
  // heat per unit of capacity, so what holds more heat keeps it longer (in
  // the measure HOLD_BY_CAPACITY, against the capacity of the operating
  // temperature): the documents are warm longest.
  float held_by = paintingOn > 0.5 ? pow(enclosure.w / paintingWarmth(vUv), HOLD_BY_CAPACITY) : 1.0;
  float k = exchange * mix(1.0, held_by, cooling);
  float gain = k > 0.0 ? k * enclosureAt(vUv) : 0.0;
  rate.x += k;
  // Semi-Lagrangian interpolation does not conserve the integral of heat.
  // As heat enters the room's matter, its transport passes continuously to
  // the conservative drift and conduction below. At full gather there is
  // no resampling loss and no source, only heat exchanged across cell faces.
  vec3 held = mix(texture2D(uSource, coord).xyz, texture2D(uSource, vUv).xyz, gathered);
  vec3 next = (held + dt * vec3(gain, 0.0, 0.0)) / (1.0 + rate * dt);
  // The pointer's trace gives its remaining warmth to the medium instead
  // of disappearing from the closed room's energy budget.
  next.x += (held.y - next.y) * gathered;
  // The whole kept to its measure (exit.js share()): every place in the
  // same proportion, so the heat keeps its shape.
  next.xy *= trim;
  gl_FragColor = vec4(next, 1.0);
}`,
  // Conduction while the heat settles into the room's matter: between a
  // cell and each of its four neighbours what flows is the difference of
  // their heat per unit of capacity times their shared conductance, in the
  // share `hold`, and the plain difference otherwise; nothing flows across
  // the frame's edge. The pointer's trace conducts plainly.
  settle: `${HEADER}${ENCLOSURE_GLSL}
uniform sampler2D uHeat;
uniform vec2 heatTexel;
uniform float rate;
uniform float hold;
float flow(float heat, float capacity, vec2 at) {
  if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) return 0.0;
  float other = texture2D(uHeat, at).x;
  float theirs = paintingWarmth(at);
  float shared = 2.0 * theirs * capacity / (theirs + capacity);
  return mix(other - heat, shared * (other / theirs - heat / capacity), hold);
}
void main() {
  vec2 C = texture2D(uHeat, vUv).xy;
  vec2 L = texture2D(uHeat, vL).xy;
  vec2 R = texture2D(uHeat, vR).xy;
  vec2 T = texture2D(uHeat, vT).xy;
  vec2 B = texture2D(uHeat, vB).xy;
  float capacity = paintingWarmth(vUv);
  float sum = flow(C.x, capacity, vL) + flow(C.x, capacity, vR) + flow(C.x, capacity, vT) + flow(C.x, capacity, vB);
  gl_FragColor = vec4(C.x + rate * sum, C.y + rate * (L.y + R.y + T.y + B.y - 4.0 * C.y), 0.0, 1.0);
}`,
  // The heat and the capacity averaged over a quarter of the grid a side,
  // from four linear taps (the first reduction takes the heat and the
  // painting's capacity; the next, the first's averages).
  share: `${HEADER}${ENCLOSURE_GLSL}
uniform sampler2D uSource;
uniform vec2 sourceTexel;
uniform float first;
vec2 tap(vec2 at) {
  vec2 held = texture2D(uSource, at).xy;
  return first > 0.5 ? vec2(held.x, paintingWarmth(at)) : held;
}
void main() {
  vec2 d = sourceTexel;
  vec2 sum = tap(vUv - d) + tap(vUv + d) + tap(vUv + vec2(d.x, -d.y)) + tap(vUv + vec2(-d.x, d.y));
  gl_FragColor = vec4(sum * 0.25, 0.0, 1.0);
}`,
  // How far each region is short of its share of the heat, as a log
  // (DRIFT above), on the 4 × 4 grid from the 16 × 16 averages, eased over
  // the neighbouring regions (a binomial filter over five of them each way).
  want: `${HEADER}
uniform sampler2D uBroad;
uniform vec2 broadTexel;
float ratio(vec2 at) {
  vec2 b = texture2D(uBroad, at).xy;
  return log((b.y + 0.05) / (b.x + 0.05));
}
void main() {
  float sum = 0.0;
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      vec2 k = vec2(float(i), float(j)) - 2.0;
      vec2 w = 6.0 - 2.0 * abs(k) - step(1.5, abs(k));
      sum += w.x * w.y * ratio(vUv + k * broadTexel * 0.5);
    }
  }
  gl_FragColor = vec4(sum / 256.0, 0.0, 0.0, 1.0);
}`,
  // The drift on the 4 × 4 grid: the slope of the shortfall per cell of the
  // finest grid (`cells`, how many of them a texel of this grid spans).
  flow: `${HEADER}
uniform sampler2D uWant;
uniform vec2 cells;
void main() {
  float l = texture2D(uWant, vL).x;
  float r = texture2D(uWant, vR).x;
  float b = texture2D(uWant, vB).x;
  float t = texture2D(uWant, vT).x;
  gl_FragColor = vec4((r - l) / (2.0 * cells.x), (t - b) / (2.0 * cells.y), 0.0, 1.0);
}`,
  // One pass of the drift: across each face the heat flows with the drift
  // there (sampled at the face, so the two cells either side see the same),
  // taken from the cell it leaves (upwind), at most DRIFT_CFL of it; nothing
  // crosses the frame's edge.
  drift: `${HEADER}
uniform sampler2D uHeat;
uniform sampler2D uFlow;
uniform vec2 heatTexel;
uniform float strength; // share of a cell's heat per pass, per unit slope of the regions' shortfall
float across(float heat, vec2 at, float v) {
  if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) return 0.0;
  v = clamp(strength * v, -${glsl(DRIFT_CFL)}, ${glsl(DRIFT_CFL)});
  return v > 0.0 ? -v * heat : -v * texture2D(uHeat, at).x;
}
void main() {
  vec2 held = texture2D(uHeat, vUv).xy;
  vec2 hx = vec2(0.5 * heatTexel.x, 0.0);
  vec2 hy = vec2(0.0, 0.5 * heatTexel.y);
  float change =
      across(held.x, vR, texture2D(uFlow, vUv + hx).x)
    + across(held.x, vL, -texture2D(uFlow, vUv - hx).x)
    + across(held.x, vT, texture2D(uFlow, vUv + hy).y)
    + across(held.x, vB, -texture2D(uFlow, vUv - hy).y);
  gl_FragColor = vec4(max(0.0, held.x + change), held.y, 0.0, 1.0);
}`,
  // The room at rest (rest() below): each place at its resting warmth, as
  // the cooling over the painting leaves it.
  room: `${HEADER}${ENCLOSURE_GLSL}
void main() {
  gl_FragColor = vec4(enclosureAt(vUv), 0.0, 0.0, 1.0);
}`,
  conduction: `${HEADER}
uniform sampler2D uHeat;
uniform float rate;
void main() {
  vec2 L = texture2D(uHeat, vL).xy;
  vec2 R = texture2D(uHeat, vR).xy;
  vec2 T = texture2D(uHeat, vT).xy;
  vec2 B = texture2D(uHeat, vB).xy;
  vec2 C = texture2D(uHeat, vUv).xy;
  gl_FragColor = vec4(C + rate * (L + R + T + B - 4.0 * C), 0.0, 1.0);
}`,
  // The heat as the display maps it (heatGl.js level()), for the probe:
  // all of it, and the air's alone (without the pointer's trace); and all
  // of it as it is, over PROBE_RANGE, in two bytes (the last two channels),
  // for the frame's mean.
  probe: `${HEADER}
uniform sampler2D uTemperature;
void main() {
  vec2 heat = texture2D(uTemperature, vUv).xy;
  float whole = clamp((heat.x + heat.y) / ${glsl(PROBE_RANGE)}, 0.0, 1.0) * 255.0;
  gl_FragColor = vec4(1.0 - exp(-1.4 * (heat.x + heat.y)), 1.0 - exp(-1.4 * heat.x), floor(whole) / 255.0, fract(whole));
}`,
  // The display blends the last two steps (uBlend from 0 to 1), so frames
  // between simulation steps move smoothly.
  display: `${HEADER}${DISPLAY_GLSL}
uniform sampler2D uTemperature;
uniform sampler2D uPrevious;
uniform float uBlend;
float heatAt(vec2 px) {
  vec2 uv = vec2(px.x / uSize.x, 1.0 - px.y / uSize.y);
  vec2 heat = mix(texture2D(uPrevious, uv).xy, texture2D(uTemperature, uv).xy, uBlend);
  return heat.x + heat.y;
}
void main() {
  gl_FragColor = display();
}`,
};

// Half-float render targets: WebGL2 core formats, or WebGL1 with the
// half-float and linear-filtering extensions. Each format is proven by a
// complete framebuffer before it is used.
function renderFormats(gl, webgl2) {
  const works = (internal, format, type) => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, internal, 4, 4, 0, format, type, null);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    const complete = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(texture);
    return complete ? { internal, format } : null;
  };
  if (webgl2) {
    gl.getExtension("EXT_color_buffer_float");
    gl.getExtension("EXT_color_buffer_half_float");
    const type = gl.HALF_FLOAT;
    const rgba = works(gl.RGBA16F, gl.RGBA, type);
    if (!rgba) return null;
    const rg = works(gl.RG16F, gl.RG, type) || rgba;
    const r = works(gl.R16F, gl.RED, type) || rg;
    return { type, rgba, rg, r };
  }
  const half = gl.getExtension("OES_texture_half_float");
  const linear = gl.getExtension("OES_texture_half_float_linear");
  if (!half || !linear) return null;
  gl.getExtension("EXT_color_buffer_half_float");
  const rgba = works(gl.RGBA, gl.RGBA, half.HALF_FLOAT_OES);
  return rgba ? { type: half.HALF_FLOAT_OES, rgba, rg: rgba, r: rgba } : null;
}

function gridSize(width, height, resolution) {
  const aspect = width / height;
  return aspect >= 1
    ? { width: Math.round(resolution * aspect), height: resolution }
    : { width: resolution, height: Math.round(resolution / aspect) };
}

// `random` places the thermals; a seeded one gives a fixed composition.
export function createFluidField(gl, webgl2, random = Math.random) {
  const formats = renderFormats(gl, webgl2);
  if (!formats) return null;
  const programs = {};
  for (const [name, source] of Object.entries(SHADERS)) {
    programs[name] = linkProgram(gl, VERTEX, source);
    if (!programs[name]) {
      Object.values(programs).forEach((p) => p && gl.deleteProgram(p.program));
      return null;
    }
  }
  const triangle = createTriangle(gl);
  const targets = [];

  const target = (width, height, format) => {
    gl.activeTexture(gl.TEXTURE0);
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, format.internal, width, height, 0, format.format, formats.type, null);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const result = {
      texture,
      fbo,
      width,
      height,
      texel: [1 / width, 1 / height],
      attach(unit) {
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        return unit;
      },
    };
    targets.push(result);
    return result;
  };
  const pair = (width, height, format) => {
    let read = target(width, height, format);
    let write = target(width, height, format);
    return {
      width,
      height,
      texel: read.texel,
      get read() {
        return read;
      },
      get write() {
        return write;
      },
      swap() {
        [read, write] = [write, read];
      },
    };
  };
  const release = () => {
    targets.forEach((t) => {
      gl.deleteTexture(t.texture);
      gl.deleteFramebuffer(t.fbo);
    });
    targets.length = 0;
  };

  let velocity;
  let pressure;
  let heat;
  let previous; // the heat of the step before, for the display's blend
  let shareMid; // the heat and the capacity averaged over 4 × 4 cells
  let shareBroad; // and over 16 × 16
  let want; // how far each region is short of its share
  let flow; // the drift
  let divergence;
  let curl;
  let unit = [1, 1];
  let lapseHeight = [1, 1];
  let split = [0, 1];
  let strength = [1, 1];
  let sinkRate = 0;
  let sinkEdge = 2;
  let sinkBand = 0.05;
  let grid = 1; // this grid's resolution over REFERENCE_GRID
  let heatGrid = 1;
  let narrow = false;
  let time = 0;
  let iterations = 20;
  let wind = 0;
  // The conditions under load (exit.js conditions()); null at rest.
  let load = null;
  const pending = [];
  const sources = new Float32Array(PUFFS * 4);
  const puffs = [];

  const draw = (destination) => {
    if (destination) {
      gl.viewport(0, 0, destination.width, destination.height);
      gl.bindFramebuffer(gl.FRAMEBUFFER, destination.fbo);
    } else {
      gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  };
  const program = (name) => {
    const { program, uniforms } = programs[name];
    gl.useProgram(program);
    return uniforms;
  };

  // Copies a pair's current state into a single target (the clear program
  // at value 1 samples linearly, so it also resamples across sizes).
  const copy = (source, destination) => {
    const u = program("clear");
    gl.uniform2fv(u.texelSize, destination.texel);
    gl.uniform1f(u.value, 1);
    gl.uniform1i(u.uTexture, source.read.attach(0));
    draw(destination);
  };

  const splat = (field, x, y, color, radius) => {
    const u = program("splat");
    gl.uniform2fv(u.texelSize, field.texel);
    gl.uniform1i(u.uTarget, field.read.attach(0));
    gl.uniform2fv(u.unit, unit);
    gl.uniform2f(u.point, x, y);
    gl.uniform3fv(u.color, color);
    gl.uniform1f(u.radius, radius);
    draw(field.write);
    field.swap();
  };

  const setEnclosure = (u) => {
    gl.uniform4f(u.enclosure, load?.floor ?? 0, load?.depth ?? 1, load?.settle ?? 0, load?.operating ?? 0);
    const painting = load?.painting;
    gl.uniform1f(u.paintingOn, painting ? 1 : 0);
    gl.uniform1f(u.cooling, load?.resting ?? load?.cooling ?? 0);
    gl.uniform1f(u.room, load?.room ?? 0);
    if (painting) {
      gl.uniform4fv(u.paintingBox, painting.frame);
      painting.textures.bindHeat(gl, u);
    }
  };

  const between = ([low, high]) => low + random() * (high - low);
  // A floor puff keeps apart from the others along the floor; a bloom, from
  // every live source, anywhere below the height the load has reached. A
  // narrow frame leaves some bloom slots silent.
  const silent = (puff) => puff.bloom && narrow && puff.slot >= NARROW_BLOOMS;
  const respawn = (puff, born) => {
    const spacing = narrow ? NARROW_SPACING : PUFF_SPACING;
    const reach = puff.bloom ? BLOOM_REACH * (load?.load ?? 0) : 0;
    const place = () => ({ x: between([0.04, 0.96]), height: reach * random() });
    const apart = ({ x, height }) =>
      puffs.every((other) => {
        if (other === puff || other.x < 0 || silent(other)) return true;
        if (!puff.bloom && !other.bloom) return Math.abs(other.x - x) > spacing;
        const dx = (other.x - x) * unit[0];
        const dy = (other.height - height) * unit[1];
        return dx * dx + dy * dy > BLOOM_SPACING * BLOOM_SPACING;
      });
    let spot = place();
    for (let tries = 0; tries < 6 && !apart(spot); tries += 1) spot = place();
    const radius = between(PUFF_RADIUS) * (narrow ? NARROW_WIDTH : 1);
    const strength = between(PUFF_STRENGTH) * (narrow ? NARROW_STRENGTH : 1) * (puff.bloom ? BLOOM_STRENGTH : 1);
    Object.assign(puff, { ...spot, born, life: between(PUFF_LIFE), radius, strength });
  };
  // Staggered start: each slot is somewhere in its life at time zero.
  for (let i = 0; i < PUFFS; i += 1) {
    const slot = i - (PUFFS - LOAD_SLOTS);
    puffs.push({ x: -1, height: 0, bloom: slot >= 0, slot });
  }
  puffs.forEach((puff, i) => respawn(puff, -between(PUFF_LIFE) * (i / PUFFS)));

  const step = (dt) => {
    time += dt;
    gl.disable(gl.BLEND);

    copy(heat, previous);

    // Under load (exit.js): how hard the machine works, and how far the
    // sources have settled.
    const loaded = load?.load ?? 0;
    const settle = load ? 1 - load.steady : 1;
    puffs.forEach((puff, i) => {
      let age = (time - puff.born) / puff.life;
      if (age >= 1) {
        respawn(puff, time + between(PUFF_GAP));
        age = (time - puff.born) / puff.life;
      }
      // A puff heats up quickly and fades slowly: a short rise to its full
      // strength, then a long fall.
      const swell =
        age <= 0
          ? 0
          : age < PUFF_RISE
            ? Math.sin((Math.PI / 2) * (age / PUFF_RISE)) ** 2
            : Math.cos((Math.PI / 2) * ((age - PUFF_RISE) / (1 - PUFF_RISE))) ** 2;
      const share = silent(puff) ? 0 : puff.bloom ? loaded : 1;
      sources.set(
        [
          puff.x * unit[0],
          puff.bloom ? puff.height * unit[1] : -PUFF_DEPTH,
          puff.radius * (1 + LOAD_SWELL * loaded),
          puff.strength * swell * share * (1 + LOAD_PUFFS * loaded) * settle,
        ],
        i * 4,
      );
    });
    let u = program("sources");
    gl.uniform2fv(u.texelSize, heat.texel);
    gl.uniform1i(u.uTarget, heat.read.attach(0));
    gl.uniform2fv(u.unit, unit);
    gl.uniform1f(u.dt, dt);
    gl.uniform1f(u.time, time);
    gl.uniform4fv(u.uSources, sources);
    gl.uniform1f(u.floorHeat, FLOOR_HEAT * (1 + LOAD_PLATE * loaded) * settle);
    gl.uniform2f(u.strength, strength[0] + (1 - strength[0]) * loaded, strength[1] + (1 - strength[1]) * loaded);
    gl.uniform2fv(u.split, split);
    draw(heat.write);
    heat.swap();

    pending.splice(0).forEach((s) => {
      if (s.vx || s.vy) splat(velocity, s.x, s.y, [s.vx, s.vy, 0], POINTER_RADIUS);
      if (s.heat) splat(heat, s.x, s.y, [0, s.heat, 0], HEAT_RADIUS);
    });

    u = program("forces");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    gl.uniform1i(u.uTemperature, heat.read.attach(1));
    gl.uniform2fv(u.unit, unit);
    gl.uniform1f(u.dt, dt);
    gl.uniform1f(u.time, time);
    gl.uniform1f(u.buoyancy, BUOYANCY * grid * (1 + LOAD_LIFT * loaded) * (1 - DRIFT_LIFT * (load?.hold ?? 0)));
    gl.uniform1f(u.pointerBuoyancy, POINTER_BUOYANCY * grid);
    gl.uniform1f(u.draft, DRAFT * grid);
    gl.uniform1f(u.wind, wind * grid);
    gl.uniform1f(u.stir, STIR);
    gl.uniform1f(u.loaded, loaded);
    setEnclosure(u);
    draw(velocity.write);
    velocity.swap();

    u = program("curl");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    draw(curl);

    u = program("vorticity");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    gl.uniform1i(u.uCurl, curl.attach(1));
    gl.uniform1f(u.curl, VORTICITY * grid);
    gl.uniform1f(u.dt, dt);
    draw(velocity.write);
    velocity.swap();

    u = program("divergence");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    gl.uniform1f(u.closed, loaded);
    draw(divergence);

    u = program("clear");
    gl.uniform2fv(u.texelSize, pressure.texel);
    gl.uniform1i(u.uTexture, pressure.read.attach(0));
    gl.uniform1f(u.value, PRESSURE_DECAY);
    draw(pressure.write);
    pressure.swap();

    u = program("pressure");
    gl.uniform2fv(u.texelSize, pressure.texel);
    gl.uniform1i(u.uDivergence, divergence.attach(0));
    for (let i = 0; i < iterations; i += 1) {
      gl.uniform1i(u.uPressure, pressure.read.attach(1));
      draw(pressure.write);
      pressure.swap();
    }

    u = program("gradient");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform1i(u.uPressure, pressure.read.attach(0));
    gl.uniform1i(u.uVelocity, velocity.read.attach(1));
    draw(velocity.write);
    velocity.swap();

    u = program("advection");
    gl.uniform2fv(u.texelSize, velocity.texel);
    gl.uniform2fv(u.velocityTexel, velocity.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    gl.uniform1i(u.uSource, velocity.read.attach(0));
    gl.uniform1f(u.dt, dt);
    gl.uniform1f(u.dissipation, VELOCITY_DISSIPATION);
    gl.uniform1f(u.dissipation2, VELOCITY_DISSIPATION);
    gl.uniform1f(u.lapse, 0);
    gl.uniform1f(u.sink, 0);
    gl.uniform2f(u.ceiling, 2, 0.01);
    gl.uniform1f(u.exchange, 0);
    gl.uniform1f(u.gathered, 0);
    gl.uniform1f(u.trim, 1);
    setEnclosure(u);
    gl.uniform2fv(u.height, lapseHeight);
    gl.uniform2fv(u.split, split);
    draw(velocity.write);
    velocity.swap();

    gl.uniform2fv(u.texelSize, heat.texel);
    gl.uniform1i(u.uVelocity, velocity.read.attach(0));
    gl.uniform1i(u.uSource, heat.read.attach(1));
    // Under load the air's cooling toward cold ink gives way to its exchange
    // with the enclosure.
    gl.uniform1f(u.dissipation, HEAT_DISSIPATION * (1 - loaded));
    // As the medium settles, the pointer's last trace cools into it.
    gl.uniform1f(u.dissipation2, POINTER_COOLING + TRACE_SETTLE * (load?.calm ?? 0));
    gl.uniform1f(u.lapse, LAPSE * (1 - loaded));
    gl.uniform1f(u.sink, sinkRate * (1 - loaded));
    gl.uniform2f(u.ceiling, sinkEdge, sinkBand);
    gl.uniform1f(u.exchange, load?.exchange ?? 0);
    gl.uniform1f(u.gathered, load?.painting ? (load.hold ?? 0) : 0);
    gl.uniform1f(u.trim, Math.exp((load?.trim ?? 0) * dt));
    draw(heat.write);
    heat.swap();

    // The load holds: the heat drifts into the room's matter.
    const hold = load?.painting ? (load.hold ?? 0) : 0;
    if (hold > 0) {
      u = program("share");
      gl.uniform1f(u.first, 1);
      setEnclosure(u);
      gl.uniform2fv(u.texelSize, shareMid.texel);
      gl.uniform2fv(u.sourceTexel, heat.texel);
      gl.uniform1i(u.uSource, heat.read.attach(0));
      draw(shareMid);
      gl.uniform1f(u.first, 0);
      gl.uniform2fv(u.texelSize, shareBroad.texel);
      gl.uniform2fv(u.sourceTexel, shareMid.texel);
      gl.uniform1i(u.uSource, shareMid.attach(0));
      draw(shareBroad);

      u = program("want");
      gl.uniform2fv(u.texelSize, want.texel);
      gl.uniform2fv(u.broadTexel, shareBroad.texel);
      gl.uniform1i(u.uBroad, shareBroad.attach(0));
      draw(want);

      u = program("flow");
      gl.uniform2fv(u.texelSize, flow.texel);
      gl.uniform2f(u.cells, heat.width / flow.width, heat.height / flow.height);
      gl.uniform1i(u.uWant, want.attach(0));
      draw(flow);

      // More passes, not larger ones, for a longer step or a fast hand;
      // capped, so a scrollbar jump or a restored position never turns one
      // frame into hundreds of GPU passes (the rest drifts on later frames).
      const passes = Math.min(12, Math.max(1, Math.round(dt * 60 * DRIFT_PASSES * (1 + (load.hurry ?? 0)))));
      u = program("drift");
      gl.uniform2fv(u.texelSize, heat.texel);
      gl.uniform2fv(u.heatTexel, heat.texel);
      gl.uniform1f(u.strength, (DRIFT * heatGrid * hold) / (60 * DRIFT_PASSES));
      gl.uniform1i(u.uFlow, flow.attach(1));
      for (let i = 0; i < passes; i += 1) {
        gl.uniform1i(u.uHeat, heat.read.attach(0));
        draw(heat.write);
        heat.swap();
      }
    }

    // Conduction is explicit, so a longer step takes more passes; while the
    // heat settles into the room, it goes by capacity, in smaller passes
    // (SETTLE_PASSES), bounded as the drift's are.
    const rate = Math.min(0.24, CONDUCTION * heatGrid * heatGrid);
    u = program(hold > 0 ? "settle" : "conduction");
    gl.uniform2fv(u.texelSize, heat.texel);
    let steps = Math.max(1, Math.round(dt * 60));
    if (hold > 0) {
      gl.uniform2fv(u.heatTexel, heat.texel);
      gl.uniform1f(u.rate, Math.min(SETTLE_RATE, rate));
      gl.uniform1f(u.hold, hold);
      setEnclosure(u);
      steps = Math.min(16, Math.max(1, Math.round(dt * 60 * (1 + (SETTLE_PASSES - 1) * hold) * (1 + (load.hurry ?? 0)))));
    } else {
      gl.uniform1f(u.rate, rate);
    }
    for (let i = steps; i > 0; i -= 1) {
      gl.uniform1i(u.uHeat, heat.read.attach(0));
      draw(heat.write);
      heat.swap();
    }
  };

  let css = { width: 1, height: 1 };

  // The probe: an 8-bit target of the display's heat level, a few
  // kilobytes. It is drawn at the end of a frame and read on a later one.
  // With WebGL2 the copy back goes through a pixel buffer behind a fence and
  // is read only once the GPU has signalled it, so the page never waits on
  // the GPU; with WebGL1 it is read at the start of the next frame, before
  // that frame gives the GPU anything.
  let probe = null;
  const dropProbe = () => {
    if (!probe) return;
    gl.deleteTexture(probe.texture);
    gl.deleteFramebuffer(probe.fbo);
    if (probe.fence) gl.deleteSync(probe.fence);
    if (probe.buffer) gl.deleteBuffer(probe.buffer);
    probe = null;
  };
  const probeTarget = () => {
    const cols = Math.max(4, Math.round(css.width / PROBE_CELL));
    const rows = Math.max(4, Math.round(css.height / PROBE_CELL));
    if (probe && probe.width === cols && probe.height === rows) return probe;
    dropProbe();
    gl.activeTexture(gl.TEXTURE0);
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, cols, rows, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    const pixels = new Uint8Array(cols * rows * 4);
    const buffer = webgl2 ? gl.createBuffer() : null;
    const levels = new Uint8Array(cols * rows);
    const air = new Uint8Array(cols * rows);
    probe = { texture, fbo, buffer, fence: null, drawn: false, width: cols, height: rows, pixels, levels, air };
    return probe;
  };

  return {
    kind: "fluid",
    // The conditions under load (exit.js conditions()), or null at rest.
    load(next) {
      load = next;
    },
    // Draws a measurement of the heat, for readProbe() on a later frame;
    // true when it drew one (none while the last is still on its way back).
    drawProbe() {
      if (!heat) return false;
      const target = probeTarget();
      if (target.drawn) return false;
      const u = program("probe");
      gl.uniform2fv(u.texelSize, [1 / target.width, 1 / target.height]);
      gl.uniform1i(u.uTemperature, heat.read.attach(0));
      draw(target);
      if (target.buffer) {
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, target.buffer);
        // A fresh store each time, so the browser keeps its fast copy for
        // the read (Chrome discards it when a fenced buffer is written over).
        gl.bufferData(gl.PIXEL_PACK_BUFFER, target.pixels.byteLength, gl.STREAM_READ);
        gl.readPixels(0, 0, target.width, target.height, gl.RGBA, gl.UNSIGNED_BYTE, 0);
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
        target.fence = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.flush();
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      target.drawn = true;
      return true;
    },
    // The measurement drawn on an earlier frame, top row first, 0 to 255:
    // `levels` as the display shows it, `air` without the pointer's trace
    // (both rewritten by the next read); `mean`, the frame's mean heat; null
    // when none is waiting.
    readProbe() {
      if (!probe?.drawn) return null;
      const { width, height, pixels, levels, air } = probe;
      if (probe.fence) {
        if (gl.clientWaitSync(probe.fence, 0, 0) === gl.TIMEOUT_EXPIRED) return null;
        gl.deleteSync(probe.fence);
        probe.fence = null;
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, probe.buffer);
        gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, pixels);
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
      } else {
        gl.bindFramebuffer(gl.FRAMEBUFFER, probe.fbo);
        gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      }
      probe.drawn = false;
      let sum = 0;
      for (let row = 0; row < height; row += 1) {
        for (let col = 0; col < width; col += 1) {
          const at = ((height - 1 - row) * width + col) * 4;
          levels[row * width + col] = pixels[at];
          air[row * width + col] = pixels[at + 1];
          sum += pixels[at + 2] + pixels[at + 3] / 255;
        }
      }
      const mean = (sum / (width * height) / 255) * PROBE_RANGE;
      return { cols: width, rows: height, levels, air, mean };
    },
    // `reach` is the thermals' reach per column in CSS px:
    // { left, right, from, to }, from and to as fractions of the width.
    // Returns true when the flow carried over from the previous grids.
    resize(width, height, quality, reach) {
      css = { width, height };
      const short = Math.min(width, height);
      const { left = short, right = short, from = 0, to = 1 } = reach || {};
      unit = [width / short, height / short];
      lapseHeight = [height / left, height / right];
      split = to > from + 0.01 ? [from, to] : [0, 1];
      strength = to > from + 0.01 && right < left ? COLUMN_STRENGTH : [1, 1];
      iterations = quality.iterations;
      grid = quality.sim / REFERENCE_GRID;
      heatGrid = quality.heat / REFERENCE_GRID;
      narrow = width < NARROW;
      const before = { velocity, pressure, heat };
      const old = targets.splice(0);
      const sim = gridSize(width, height, quality.sim);
      const hot = gridSize(width, height, quality.heat);
      velocity = pair(sim.width, sim.height, formats.rg);
      pressure = pair(sim.width, sim.height, formats.r);
      divergence = target(sim.width, sim.height, formats.r);
      curl = target(sim.width, sim.height, formats.r);
      heat = pair(hot.width, hot.height, formats.rg);
      previous = target(hot.width, hot.height, formats.rg);
      const quarter = (n) => Math.max(2, Math.round(n / 4));
      shareMid = target(quarter(hot.width), quarter(hot.height), formats.rg);
      shareBroad = target(quarter(shareMid.width), quarter(shareMid.height), formats.rg);
      want = target(shareMid.width, shareMid.height, formats.r);
      flow = target(shareMid.width, shareMid.height, formats.rg);
      gl.bindBuffer(gl.ARRAY_BUFFER, triangle);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
      const carried = Boolean(before.velocity);
      // A fresh flow starts its thermals at the size this frame calls for.
      if (!carried) puffs.forEach((puff, i) => respawn(puff, time - between(PUFF_LIFE) * (i / PUFFS)));
      // Resample the old flow onto the new grids (the targets filter
      // linearly), so a resize or a quality step never restarts it.
      if (carried) {
        const u = program("clear");
        gl.uniform1f(u.value, 1);
        [
          [before.velocity, velocity],
          [before.pressure, pressure],
          [before.heat, heat],
        ].forEach(([source, destination]) => {
          gl.uniform2fv(u.texelSize, destination.texel);
          gl.uniform1i(u.uTexture, source.read.attach(0));
          draw(destination.write);
          destination.swap();
        });
      }
      copy(heat, previous);
      old.forEach((t) => {
        gl.deleteTexture(t.texture);
        gl.deleteFramebuffer(t.fbo);
      });
      return carried;
    },
    // x, y in CSS px from the top left; vx, vy in CSS px per second.
    splat({ x, y, vx = 0, vy = 0, heat: amount = 0 }) {
      const cells = velocity ? velocity.height / css.height : 0;
      pending.push({
        x: x / css.width,
        y: 1 - y / css.height,
        vx: vx * cells * POINTER_FORCE,
        vy: -vy * cells * POINTER_FORCE,
        heat: amount,
      });
      if (pending.length > 24) pending.shift();
    },
    step,
    // Sets the heat to the room at rest under `conditions` (exit.js, with
    // the painting): where the runway has run out and the painting goes up
    // with the page, the field shows it as the cooling leaves it, however
    // the page came there, and need not run.
    rest(conditions) {
      if (!heat || !conditions?.painting) return;
      load = conditions;
      const u = program("room");
      gl.uniform2fv(u.texelSize, heat.texel);
      setEnclosure(u);
      draw(heat.write);
      heat.swap();
      copy(heat, previous);
    },
    // The header's lower edge in CSS px from the frame's top, and whether the
    // bar is solid (then the air under it is cooled).
    ceiling(edge, solid) {
      sinkEdge = 1 - edge / css.height;
      sinkBand = HEADER_BAND / css.height;
      sinkRate = solid ? HEADER_SINK : 0;
    },
    // Scroll velocity in CSS px per second, positive when the page moves up.
    scroll(velocity) {
      wind = velocity * WIND;
    },
    // Runs the flow forward by `seconds` in steps of `dt`, so the first
    // frame shows developed convection rather than the start from rest.
    warm(seconds, dt) {
      for (let t = 0; t < seconds; t += dt) step(dt);
    },
    render(view) {
      const u = program("display");
      gl.uniform2fv(u.texelSize, heat.texel);
      gl.uniform1i(u.uTemperature, heat.read.attach(0));
      gl.uniform1i(u.uPrevious, previous.attach(1));
      gl.uniform1f(u.uBlend, view.blend ?? 1);
      setDisplayUniforms(gl, u, view);
      draw(null);
    },
    dispose() {
      dropProbe();
      release();
      gl.deleteBuffer(triangle);
      Object.values(programs).forEach((p) => gl.deleteProgram(p.program));
    },
  };
}
