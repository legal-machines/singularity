// Shared WebGL pieces for the heat field: program linking, the full-screen
// triangle, and the GLSL both renderers use to turn temperature into color,
// draw the H1 on first load and resolve the painting. GLSL comments inside
// the shader strings ship in the build.

export function linkProgram(gl, vertexSource, fragmentSource) {
  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (gl.getShaderParameter(shader, gl.COMPILE_STATUS)) return shader;
    gl.deleteShader(shader);
    return null;
  };
  const vertex = compile(gl.VERTEX_SHADER, vertexSource);
  const fragment = compile(gl.FRAGMENT_SHADER, fragmentSource);
  if (!vertex || !fragment) {
    if (vertex) gl.deleteShader(vertex);
    if (fragment) gl.deleteShader(fragment);
    return null;
  }
  const program = gl.createProgram();
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.bindAttribLocation(program, 0, "aPosition");
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    gl.deleteProgram(program);
    return null;
  }
  const uniforms = {};
  const count = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < count; i += 1) {
    const name = gl.getActiveUniform(program, i).name.replace(/\[0\]$/, "");
    uniforms[name] = gl.getUniformLocation(program, name);
  }
  return { program, uniforms };
}

// One triangle that covers the viewport, bound to attribute 0.
export function createTriangle(gl) {
  const buffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  return buffer;
}

export const NOISE_GLSL = `
float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
             mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
`;

// What a place of the painting holds of heat (its capacity), from its
// brightness `lum` (0 to 1): its brightness on the heat scale, the mean at
// brick and the documents running to pale yellow, as a temperature (the
// scale's inverse). Shared by the flow (PAINTING_HEAT_GLSL), the display
// and the page (governor.js meanCapacity()).
const PAINTING_MEAN = 0.24;
const PAINTING_SPREAD = 1.3;
const PAINTING_OPERATING = 0.4 / 0.85;
export function paintingHeatOf(lum) {
  const level = Math.min(0.97, Math.max(0.05, PAINTING_OPERATING + (lum - PAINTING_MEAN) * PAINTING_SPREAD));
  return -Math.log(1 - level) / 1.4;
}
const PAINTING_CAPACITY_GLSL = `
#define PAINTING_MEAN ${PAINTING_MEAN.toFixed(4)}
#define PAINTING_SPREAD ${PAINTING_SPREAD.toFixed(4)}
#define PAINTING_OPERATING ${PAINTING_OPERATING.toFixed(8)}
float paintingHeatOf(float lum) {
  float level = clamp(PAINTING_OPERATING + (lum - PAINTING_MEAN) * PAINTING_SPREAD, 0.05, 0.97);
  return -log(1.0 - level) / 1.4;
}
`;

// The heat map. The opening is tiled with hexagonal cells; each cell reads
// the temperature at its centre and takes one color from an ironbow scale
// drawn from the house palette: ink, deep brick, brick, ember, amber, a pale
// yellow at the hottest, no violet end. Cold cells are ink, so the lattice
// shows only where there is warmth. Each renderer defines heatAt(px), the
// temperature at a CSS pixel. The tiling fills the frame to its edges,
// under the transparent header too.
export const DISPLAY_GLSL = `${PAINTING_CAPACITY_GLSL}
uniform vec2 uSize;
uniform float uScale;
uniform float uTime;
uniform float uCell;
// The first-load entrance (statementLayer.js): x, the statement is drawn;
// y, seconds since its first frame; z, how far the heat has grown in, 0 to 1
// (2 when it is all there); w, the drawn statement's opacity.
uniform vec4 uEntrance;
// The film grain's strength: 1, and down to 0 as the opening's exit
// (exit.js) cools into flat brick.
uniform float uGrain;
// The copy under the heat (exit.js). The field is drawn behind the copy, as
// at rest (uLayer 0); while the opening exits, the same frame is also drawn
// as a layer over the copy (uLayer 1, HeatField.jsx lifts it onto a canvas
// above the text): over each block of copy (uCopy: left, top, right,
// bottom in CSS px, uCopyCount of them) each cell in its own colour, with
// its heat as its opacity, so the copy stays crisp over cold and dark-red
// cells and goes under a cell as the cell grows hot (COVER_FROM to
// COVER_TO; the operating temperature covers it whole). uCover eases it in
// as the exit starts.
#define COPY_BLOCKS 8
#define COVER_FROM 0.22
#define COVER_TO 0.42
uniform vec4 uCopy[COPY_BLOCKS];
uniform float uCopyCount;
uniform float uCover;
uniform float uLayer;
// The operating level is the heat the scale shows as brick (0.4 / 0.85):
// ramp(0.5) is --lm-brick.
#define OPERATING_LEVEL 0.47058824

float heatAt(vec2 px);

vec3 ramp(float t) {
  t = clamp(t, 0.0, 1.0) * 6.0;
  vec3 c0 = vec3(0.063, 0.063, 0.051);
  vec3 c1 = vec3(0.180, 0.078, 0.063);
  vec3 c2 = vec3(0.478, 0.188, 0.161);
  vec3 c3 = vec3(0.682, 0.286, 0.247);
  vec3 c4 = vec3(0.878, 0.478, 0.227);
  vec3 c5 = vec3(0.953, 0.714, 0.298);
  vec3 c6 = vec3(0.969, 0.867, 0.557);
  if (t < 1.0) return mix(c0, c1, t);
  if (t < 2.0) return mix(c1, c2, t - 1.0);
  if (t < 3.0) return mix(c2, c3, t - 2.0);
  if (t < 4.0) return mix(c3, c4, t - 3.0);
  if (t < 5.0) return mix(c4, c5, t - 4.0);
  return mix(c5, c6, t - 5.0);
}

// CSS pixel of this fragment, y down.
vec2 cssPixel() {
  return vec2(gl_FragCoord.x, uSize.y * uScale - gl_FragCoord.y) / uScale;
}

float level(float heat) {
  return 1.0 - exp(-1.4 * heat);
}

// Film grain, drawn in the same pass: a fine per-pixel noise fixed to the
// frame, so the warm air moves under it and nothing flickers. It shows on
// the warm cells, as an overlay blend would, and barely on ink.
float grainHash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

// The strength scales it (uGrain).
vec3 withGrain(vec3 color, float strength) {
  float n = grainHash(floor(cssPixel())) - 0.5;
  float lum = dot(color, vec3(0.299, 0.587, 0.114));
  float amount = 0.012 + 0.06 * smoothstep(0.03, 0.35, lum) * (1.0 - smoothstep(0.75, 1.0, lum));
  return clamp(color + n * amount * strength, 0.0, 1.0);
}

// Pointy-top hexagonal lattice in cell units: xy is the offset from the
// cell's centre, zw the centre itself.
vec4 hexCell(vec2 p) {
  const vec2 s = vec2(1.0, 1.7320508);
  vec4 c = floor(vec4(p, p - vec2(0.5, 1.0)) / s.xyxy) + 0.5;
  vec4 h = vec4(p - c.xy * s, p - (c.zw + 0.5) * s);
  return dot(h.xy, h.xy) < dot(h.zw, h.zw)
    ? vec4(h.xy, c.xy * s)
    : vec4(h.zw, (c.zw + 0.5) * s);
}

// Every cell is one flat color, the temperature at its centre.
// The bottom edge runs through the centres of one row, so the tiling ends
// in a row of equal half-cells. Cells meet without seams: each is one flat
// color, and the lattice shows only where neighbours differ.
vec2 cellCentre(vec2 px) {
  float row = 0.8660254 * uCell;
  vec2 origin = vec2(mod(uSize.x * 0.5, uCell), mod(uSize.y, row));
  return hexCell((px - origin) / uCell).zw * uCell + origin;
}

// The field's heat at a cell's centre. On first load the heat grows in from
// the bottom edge (uEntrance.z runs from 0 to 1; 2 once it is all there):
// the hottest air shows first, near the floor, and the warm masses fill out
// upward as the heat comes up.
float fieldHeat(vec2 centre) {
  float height = 1.0 - centre.y / uSize.y;
  float grow = uEntrance.z >= 2.0 ? 1.0 : clamp(uEntrance.z * 1.6 - height * 0.6, 0.0, 1.0);
  return heatAt(centre) * grow;
}

// A cold cell is ink: the lattice appears only where there is warmth.
float warmth(float t) {
  return smoothstep(0.05, 0.22, t);
}

vec3 heatColor(float t) {
  return mix(ramp(0.0), ramp(0.1 + 0.85 * t), warmth(t));
}

bool overCopy(vec2 centre) {
  for (int i = 0; i < COPY_BLOCKS; i++) {
    if (float(i) >= uCopyCount) break;
    vec4 b = uCopy[i];
    if (centre.x >= b.x && centre.x <= b.z && centre.y >= b.y && centre.y <= b.w) return true;
  }
  return false;
}

// The statement on first load (statementLayer.js, entrance.js). The H1 is a
// warm region of the field itself: the field's own cells, at the same
// places, coloured by the field's own heat map from a temperature taken from
// the real glyphs (hot in the strokes, cooler around them). The cells heat
// up, the deepest in the strokes first and in reading order, and breathe a
// little as the air does; then a front passes along each line that cools
// them, cell by cell, while the type itself shows inside each one: the H1
// rasterised, the image the DOM then draws over it, in place.
uniform sampler2D uTypeRaster;
uniform sampler2D uTypeCells;  // per cell: temperature, heating order, cooling order
uniform vec4 uTypeBox;         // the raster's box in CSS px: left, top, width, height
uniform vec4 uTypeLattice;     // the lattice origin (CSS px), the atlas size (texels)
uniform vec4 uTypeLevel;       // texel offset of cell (0, 0), cell size (CSS px)
uniform vec4 uTypeTiming;      // heating: first (s), spread (s), rise (s)
uniform vec4 uTypeCooling;     // cooling: first (s), spread (s), dwell (s)

const vec3 PAPER = vec3(0.918, 0.910, 0.894); // --lm-paper, the H1's colour

// Smooth value noise for the breathing of the warm cells.
float breathNoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(grainHash(i), grainHash(i + vec2(1.0, 0.0)), u.x),
             mix(grainHash(i + vec2(0.0, 1.0)), grainHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

vec3 withStatement(vec2 px, vec3 under) {
  vec2 local = px - uTypeBox.xy;
  if (local.x < 0.0 || local.y < 0.0 || local.x > uTypeBox.z || local.y > uTypeBox.w) return under;
  vec2 unit = hexCell((px - uTypeLattice.xy) / uTypeLevel.z).zw;
  vec2 centre = unit * uTypeLevel.z + uTypeLattice.xy;
  vec2 index = vec2(floor(unit.x + 0.25), floor(unit.y / 0.8660254 + 0.5));
  vec3 cell = texture2D(uTypeCells, (index + uTypeLevel.xy + 0.5) / uTypeLattice.zw).rgb;

  float heat = clamp((uEntrance.y - uTypeTiming.x - uTypeTiming.y * cell.g) / uTypeTiming.z, 0.0, 1.0);
  heat = heat * heat * (3.0 - 2.0 * heat);
  float cool = clamp((uEntrance.y - uTypeCooling.x - uTypeCooling.y * cell.b) / uTypeCooling.z, 0.0, 1.0);
  float breath = 0.92 + 0.16 * breathNoise(centre / 70.0 + vec2(0.0, uEntrance.y * 0.9));

  // The field's heat map, from this temperature.
  float t = level(2.0 * cell.r * heat * breath * (1.0 - cool));
  vec3 warm = mix(under, ramp(0.1 + 0.85 * t), smoothstep(0.05, 0.22, t));
  // The type inside the cell as it cools.
  float type = texture2D(uTypeRaster, local / uTypeBox.zw).r * smoothstep(0.25, 0.85, cool);
  return mix(under, mix(warm, PAPER, type), uEntrance.w);
}

// The painting on the wall (Wall.jsx, exit.js). The medium itself has
// taken the painting's heat (heatFluid.js): each place holds heat in the
// measure of its capacity (paintingHeatOf(), from its brightness), and
// conduction evens out the heat per unit of capacity, the cell's share.
// Here that share is taken down to the painting's own grain: each point
// holds its capacity times its cell's share, so as the heat settles into
// the painting (uPaintingResolve) the cells resolve into the heat's image of
// the painting, point by point, its dark dark red and its documents amber and
// pale yellow. As the room cools the share falls toward the room's resting
// warmth, the documents', which hold most, last (heatFluid.js
// HOLD_BY_CAPACITY), and as its share falls past REVEAL_AT each place
// turns, over REVEAL_SPAN, from the heat's image of the painting to the
// painting itself, the places that hold least first and the documents
// last: every point out of its own heat colour, a paper from
// amber into paper and a shadow from dark red into shadow. As it turns the
// painting is lit by the heat's own glow (HOT_LIGHT), which settles as the
// cell cools on (over HOT_SPAN), so it comes out warm and never through a
// pale red. The painting is never cold: the warmth each point still holds
// lights it (WARM_LIGHT), so the documents, which keep their warmth at
// rest, glow.
uniform sampler2D uPainting;     // the painting
uniform sampler2D uPaintingHeat; // the painting small and soft
uniform vec4 uPaintingBox;       // where it is drawn, CSS px: left, top, width, height
uniform float uPaintingResolve;  // the cells resolving into it, 0 to 1
// The share as the room starts to cool is about 0.95 of a place's capacity
// (exit.js share()), and the room cools it toward 0.145 (AMBIENT). The
// painting turns over most of that fall, so it comes out of its heat image
// as the heat leaves, never through a heat image gone dark.
#define REVEAL_AT 0.85
#define REVEAL_SPAN 0.4
#define REVEAL_SCATTER 0.04
#define HOT_SPAN 0.2
const vec3 HOT_LIGHT = vec3(1.45, 0.92, 0.52);
const vec3 WARM_LIGHT = vec3(1.34, 1.1, 0.8);
const vec3 LUMA = vec3(0.299, 0.587, 0.114);

vec3 warmLight(float t) {
  return mix(vec3(1.0), WARM_LIGHT, smoothstep(0.04, 0.5, t));
}

// The heat's image of the painting is shown at a wider spread than the
// flow's capacity (SHOWN_SPREAD against PAINTING_SPREAD), as a thermal camera
// shows it: the documents, which hold most, amber and pale yellow, the room's
// dark gone to ink. Each cell keeps one flat colour from its own share of the
// painting, the lattice of the field, and the painting's grain shows inside
// it at SHOWN_GRAIN, until the painting itself comes out.
#define SHOWN_SPREAD 2.4
#define SHOWN_GRAIN 0.5
float shownHeatOf(float lum) {
  float shown = clamp(PAINTING_OPERATING + (lum - PAINTING_MEAN) * SHOWN_SPREAD, 0.02, 0.97);
  return -log(1.0 - shown) / 1.4;
}

vec3 withPainting(vec2 px, vec2 centre, float heat, vec3 under) {
  float resolve = uPaintingResolve;
  if (resolve <= 0.0) return under;
  vec2 uv = clamp((px - uPaintingBox.xy) / uPaintingBox.zw, 0.0, 1.0);
  vec3 paint = texture2D(uPainting, uv).rgb;
  float lum = dot(paint, LUMA);
  vec2 at = clamp((centre - uPaintingBox.xy) / uPaintingBox.zw, 0.0, 1.0);
  float cellLum = dot(texture2D(uPaintingHeat, at).rgb, LUMA);
  float share = heat / paintingHeatOf(cellLum);
  float own = mix(level(shownHeatOf(cellLum) * share), level(shownHeatOf(lum) * share), SHOWN_GRAIN);
  vec3 color = mix(under, withGrain(heatColor(own), uGrain), resolve);
  float mark = REVEAL_AT + REVEAL_SCATTER * (grainHash(floor(centre) + 17.0) - 0.5);
  float shown = (1.0 - smoothstep(mark - REVEAL_SPAN, mark, share)) * resolve;
  float glow = smoothstep(mark - REVEAL_SPAN - HOT_SPAN, mark - REVEAL_SPAN, share);
  color = mix(color, min(paint * mix(warmLight(own), HOT_LIGHT, glow), 1.0), shown);
  return color;
}

// The display: the field, grained, and the statement over it while it is
// drawn (uLayer 0, opaque); or the layer over the copy (uLayer 1,
// premultiplied, clear away from the copy).
vec4 display() {
  vec2 px = cssPixel();
  vec2 centre = cellCentre(px);
  float heat = fieldHeat(centre);
  float t = level(heat);
  vec3 color = withGrain(heatColor(t), uGrain);
  if (uLayer > 0.5) {
    float a = overCopy(centre) ? smoothstep(COVER_FROM, COVER_TO, t) * uCover : 0.0;
    return vec4(color * a, a);
  }
  if (uEntrance.x > 0.5) color = withStatement(px, color);
  color = withPainting(px, centre, heat, color);
  return vec4(color, 1.0);
}
`;

// What each place of the painting holds of heat, for the flow
// (heatFluid.js), from the painting small and soft: `p` is a point on the
// painting, 0 to 1 across and down.
export const PAINTING_HEAT_GLSL = `${PAINTING_CAPACITY_GLSL}
float paintingHeatAt(sampler2D warmth, vec2 p) {
  return paintingHeatOf(dot(texture2D(warmth, clamp(p, 0.0, 1.0)).rgb, vec3(0.299, 0.587, 0.114)));
}
`;

// The painting (Wall.jsx): the image itself and the same image small and
// soft, for its warmth, on texture units clear of the fluid's (0 to 2) and
// the drawn H1's (6, 7; statementLayer.js).
const UNIT_PAINTING = 4;
const UNIT_PAINTING_HEAT = 5;
export function createPaintingTextures(gl, image, heat) {
  const make = (source, unit) => {
    gl.activeTexture(gl.TEXTURE0 + unit);
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return texture;
  };
  const painting = make(image, UNIT_PAINTING);
  const warmth = make(heat, UNIT_PAINTING_HEAT);
  gl.activeTexture(gl.TEXTURE0);
  return {
    // The painting, and the same small and soft, for the display.
    bind(context, uniforms) {
      context.activeTexture(context.TEXTURE0 + UNIT_PAINTING);
      context.bindTexture(context.TEXTURE_2D, painting);
      context.uniform1i(uniforms.uPainting, UNIT_PAINTING);
      context.activeTexture(context.TEXTURE0 + UNIT_PAINTING_HEAT);
      context.bindTexture(context.TEXTURE_2D, warmth);
      context.uniform1i(uniforms.uPaintingHeat, UNIT_PAINTING_HEAT);
      context.activeTexture(context.TEXTURE0);
    },
    // Its warmth, for the flow.
    bindHeat(context, uniforms) {
      context.activeTexture(context.TEXTURE0 + UNIT_PAINTING_HEAT);
      context.bindTexture(context.TEXTURE_2D, warmth);
      context.uniform1i(uniforms.uPaintingHeat, UNIT_PAINTING_HEAT);
      context.activeTexture(context.TEXTURE0);
    },
    dispose() {
      gl.deleteTexture(painting);
      gl.deleteTexture(warmth);
    },
  };
}

// `view.statement` is the entrance's drawn H1 ({ layer, time, alpha })
// while it plays; `view.rise` how far the heat has come up (2 outside the
// entrance); `view.grain` the film grain's strength; `view.copy` the blocks
// of copy (CSS px boxes) and `view.cover` how far the canvas covers them
// (0 at rest); `view.layer` 1 for the layer over the copy, 0 for the field;
// `view.painting` the painting while it is there ({ textures, box,
// resolve }), or null.
const copyBoxes = new Float32Array(32);
export function setDisplayUniforms(gl, uniforms, view) {
  const painting = view.painting;
  gl.uniform1f(uniforms.uPaintingResolve, painting?.resolve ?? 0);
  if (painting) {
    gl.uniform4fv(uniforms.uPaintingBox, painting.box);
    painting.textures.bind(gl, uniforms);
  }
  gl.uniform1f(uniforms.uGrain, view.grain ?? 1);
  const copy = view.copy ?? [];
  copyBoxes.fill(0);
  copy.slice(0, 8).forEach((box, i) => copyBoxes.set(box, i * 4));
  gl.uniform4fv(uniforms.uCopy, copyBoxes);
  gl.uniform1f(uniforms.uCopyCount, Math.min(copy.length, 8));
  gl.uniform1f(uniforms.uCover, view.cover ?? 0);
  gl.uniform1f(uniforms.uLayer, view.layer ?? 0);
  gl.uniform2f(uniforms.uSize, view.width, view.height);
  gl.uniform1f(uniforms.uScale, view.scale);
  gl.uniform1f(uniforms.uTime, view.time);
  gl.uniform1f(uniforms.uCell, view.cell);
  const statement = view.statement;
  gl.uniform4f(
    uniforms.uEntrance,
    statement ? 1 : 0,
    statement?.time ?? 0,
    view.rise ?? 2,
    statement?.alpha ?? 0,
  );
  statement?.layer.bind(gl, uniforms);
}
