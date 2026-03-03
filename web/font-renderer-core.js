export function parseHexColor(input, fallback) {
  const text = (input || fallback || '#e5fff0').trim();
  const hex = text.startsWith('#') ? text.slice(1) : text;
  if (hex.length === 3) {
    const r = parseInt(hex[0] + hex[0], 16);
    const g = parseInt(hex[1] + hex[1], 16);
    const b = parseInt(hex[2] + hex[2], 16);
    if (Number.isFinite(r) && Number.isFinite(g) && Number.isFinite(b)) return [r / 255, g / 255, b / 255];
  }
  if (hex.length === 6) {
    const r = parseInt(hex.slice(0, 2), 16);
    const g = parseInt(hex.slice(2, 4), 16);
    const b = parseInt(hex.slice(4, 6), 16);
    if (Number.isFinite(r) && Number.isFinite(g) && Number.isFinite(b)) return [r / 255, g / 255, b / 255];
  }
  return [0.9, 0.99, 0.94];
}

export function toRgbaBytes(bytes, channels) {
  if (channels === 4) return bytes;
  const pxCount = Math.floor(bytes.length / channels);
  const out = new Uint8Array(pxCount * 4);
  for (let i = 0; i < pxCount; i++) {
    const o = i * 4;
    if (channels === 1) {
      const v = bytes[i];
      out[o] = v;
      out[o + 1] = v;
      out[o + 2] = v;
      out[o + 3] = 255;
    } else {
      const s = i * channels;
      out[o] = bytes[s];
      out[o + 1] = bytes[s + 1];
      out[o + 2] = bytes[s + 2];
      out[o + 3] = 255;
    }
  }
  return out;
}

export function buildGlyphMap(meta) {
  const map = new Map();
  if (!meta || !meta.glyphs || !meta.atlas) return map;
  const aw = Number(meta.atlas.width) || 1;
  const ah = Number(meta.atlas.height) || 1;
  const em = Number(meta.atlas.size) || 1;
  const yOriginTop = meta.atlas.yOrigin === 'top';

  for (const g of meta.glyphs) {
    const code = g.unicode;
    const advancePx = Number(g.advance || 0) * em;
    if (!g.planeBounds || !g.atlasBounds) {
      map.set(code, { empty: true, advancePx });
      continue;
    }

    const ab = g.atlasBounds;
    const pb = g.planeBounds;
    const left = Number(ab.left);
    const right = Number(ab.right);
    const srcBottom = Number(ab.bottom);
    const srcTop = Number(ab.top);

    const bottom = yOriginTop ? ah - srcBottom : srcBottom;
    const top = yOriginTop ? ah - srcTop : srcTop;
    const widthPx = right - left;
    const heightPx = top - bottom;

    map.set(code, {
      empty: false,
      uv: [left / aw, bottom / ah, widthPx / aw, heightPx / ah],
      widthPx,
      heightPx,
      offsetXPx: widthPx * 0.5 + widthPx * Number(pb.left),
      baselineOffsetYPx: -(heightPx * 0.5 + heightPx * Number(pb.bottom)),
      advancePx
    });
  }

  return map;
}

function compile(gl, type, source) {
  const s = gl.createShader(type);
  gl.shaderSource(s, source);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
  return s;
}

export class DistanceTextRenderer {
  constructor(canvas, mode) {
    this.canvas = canvas;
    this.mode = mode;
    this.gl = null;
    this.program = null;
    this.vao = null;
    this.vbo = null;
    this.tex = null;
    this.meta = null;
    this.channels = 4;
    this.atlasBytes = null;
    this.glyphMap = new Map();
    this.text = '';
    this.options = {
      size: 56,
      color: '#d5fcf0',
      aa: 8,
      effect: 'fill',
      stroke: 2.5,
      glow: 2,
      shadowX: 4,
      shadowY: -4
    };
    this._drawPending = false;
    this.init();
  }

  init() {
    const gl = this.canvas.getContext('webgl2', { alpha: false, antialias: true });
    if (!gl) return;
    this.gl = gl;

    const vs = `#version 300 es
    precision highp float;
    layout(location=0) in vec2 aP;
    uniform vec2 uResolution;
    uniform vec4 uT;
    uniform vec2 uP;
    uniform vec4 uUV;
    out vec2 uv;
    void main() {
      vec2 aP_ = aP * 0.5 + 0.5;
      vec2 aPFlipY = vec2(aP_.x, 1.0 - aP_.y);
      uv = uUV.xy + aPFlipY * uUV.zw;
      vec2 pos = aP * mat2(uT) + uP;
      vec2 ndc = (pos / uResolution) * 2.0 - 1.0;
      gl_Position = vec4(ndc * vec2(1.0, -1.0), 0.0, 1.0);
    }`;

    const p = gl.createProgram();
    gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, vs));
    gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, this.mode === 'mtsdf' ? DistanceTextRenderer.mtsdfFS : DistanceTextRenderer.msdfFS));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));

    this.program = p;
    this.vao = gl.createVertexArray();
    this.vbo = gl.createBuffer();
    this.tex = gl.createTexture();

    const quad = new Float32Array([-1, -1, -1, 1, 1, -1, 1, 1]);
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, quad, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
  }

  setFontData(data) {
    if (!data) {
      this.meta = null;
      this.glyphMap = new Map();
      this.requestDraw();
      return;
    }
    this.meta = data.metaJson || data.meta || null;
    this.channels = Number(data.channels || 4);
    this.atlasBytes = data.atlasBytes || null;
    this.glyphMap = buildGlyphMap(this.meta);
    if (!this.gl || !this.tex || !this.meta || !this.atlasBytes) return;

    const gl = this.gl;
    const rgba = toRgbaBytes(this.atlasBytes, this.channels);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, Number(this.meta.atlas.width), Number(this.meta.atlas.height), 0, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    this.requestDraw();
  }

  setText(text) {
    this.text = text || '';
    this.requestDraw();
  }

  setOptions(next) {
    this.options = { ...this.options, ...next };
    this.requestDraw();
  }

  requestDraw() {
    if (this._drawPending) return;
    this._drawPending = true;
    requestAnimationFrame(() => {
      this._drawPending = false;
      this.draw();
    });
  }

  draw() {
    const gl = this.gl;
    if (!gl) return;

    const dpr = Math.max(1, Math.min(window.devicePixelRatio || 1, 2));
    const w = Math.max(1, Math.floor(this.canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }

    gl.viewport(0, 0, w, h);
    gl.clearColor(0.03, 0.08, 0.1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!this.meta || !this.atlasBytes || !this.glyphMap.size || !this.program) return;

    const fontSize = Number(this.options.size || 56);
    const aa = Number(this.options.aa || 8);
    const scale = fontSize / Number(this.meta.atlas.size || 48);
    const lineHeight = Number((this.meta.metrics && this.meta.metrics.lineHeight) || 1.2) * Number(this.meta.atlas.size || 48);

    gl.useProgram(this.program);
    gl.bindVertexArray(this.vao);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    gl.uniform1i(gl.getUniformLocation(this.program, 'uImg'), 0);
    gl.uniform2f(gl.getUniformLocation(this.program, 'uResolution'), w, h);

    if (this.mode === 'msdf') {
      const c = parseHexColor(this.options.color, '#d5fcf0');
      gl.uniform1f(gl.getUniformLocation(this.program, 'aa'), aa);
      gl.uniform3f(gl.getUniformLocation(this.program, 'color'), c[0], c[1], c[2]);
    } else {
      const effectNames = { fill: 0, outline: 1, glow: 2, shadow: 3, combo: 4 };
      const effectRaw = String(this.options.effect || 'fill').toLowerCase();
      const effect = Number.isFinite(Number(effectRaw)) ? Number(effectRaw) : (effectNames[effectRaw] ?? 0);
      const distRange = Number((this.meta.atlas && this.meta.atlas.distanceRange) || 8);

      gl.uniform1f(gl.getUniformLocation(this.program, 'aa'), aa);
      gl.uniform1f(gl.getUniformLocation(this.program, 'uDistRange'), distRange);
      gl.uniform1i(gl.getUniformLocation(this.program, 'uEffect'), effect);
      gl.uniform1f(gl.getUniformLocation(this.program, 'uStroke'), Number(this.options.stroke || 2.5));
      gl.uniform1f(gl.getUniformLocation(this.program, 'uGlow'), Number(this.options.glow || 8));
      gl.uniform2f(gl.getUniformLocation(this.program, 'uShadowPx'), Number(this.options.shadowX || 4), Number(this.options.shadowY || -4));
      gl.uniform2f(gl.getUniformLocation(this.program, 'uAtlasSize'), Number(this.meta.atlas.width), Number(this.meta.atlas.height));
    }

    const startX = 16;
    const startY = 18 + fontSize;
    const fallbackAdvance = Number(this.meta.atlas.size || 48) * 0.3;
    let penX = 0;
    let penY = 0;

    for (const ch of this.text) {
      if (ch === '\n') {
        penX = 0;
        penY += lineHeight;
        continue;
      }
      const glyph = this.glyphMap.get(ch.codePointAt(0));
      if (!glyph) {
        penX += fallbackAdvance;
        continue;
      }
      if (!glyph.empty) {
        const gw = glyph.widthPx * scale;
        const gh = glyph.heightPx * scale;
        const px = startX + (penX + glyph.offsetXPx) * scale;
        const py = startY + (penY + glyph.baselineOffsetYPx) * scale;
        gl.uniform2f(gl.getUniformLocation(this.program, 'uP'), px, py);
        gl.uniform4f(gl.getUniformLocation(this.program, 'uT'), gw * 0.5, 0, 0, gh * 0.5);
        gl.uniform4f(gl.getUniformLocation(this.program, 'uUV'), glyph.uv[0], glyph.uv[1], glyph.uv[2], glyph.uv[3]);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      }
      penX += glyph.advancePx;
    }
  }
}

DistanceTextRenderer.msdfFS = `#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D uImg;
uniform float aa;
uniform vec3 color;
out vec4 fragColor;
float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }
void main() {
  vec3 t = texture(uImg, uv).rgb;
  float sigDist = median(t.r, t.g, t.b) - 0.5;
  float alpha = clamp(sigDist * aa + 0.5, 0.0, 1.0);
  fragColor = vec4(color, alpha);
  if (fragColor.a < 0.001) discard;
}`;

DistanceTextRenderer.mtsdfFS = `#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D uImg;
uniform float aa;
uniform float uDistRange;
uniform int uEffect;
uniform float uStroke;
uniform float uGlow;
uniform vec2 uShadowPx;
uniform vec2 uAtlasSize;
out vec4 fragColor;
float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }
void main() {
  vec4 tex = texture(uImg, uv);
  float msdf = median(tex.r, tex.g, tex.b) - 0.5;
  float sdf = tex.a - 0.5;
  float fill = clamp(msdf * aa + 0.5, 0.0, 1.0);
  float distPx = sdf * uDistRange;
  vec3 fillColor = vec3(0.88, 0.99, 0.94);
  vec3 outlineColor = vec3(0.53, 0.93, 0.93);
  vec3 glowColor = vec3(0.30, 0.95, 0.75);
  vec3 shadowColor = vec3(0.02, 0.13, 0.12);
  float outline = 1.0 - smoothstep(max(0.0, uStroke - 1.0), uStroke + 1.0, abs(distPx));
  float outsideDist = max(0.0, -distPx);
  float glow = (1.0 - smoothstep(0.0, max(0.001, uGlow), outsideDist)) * (1.0 - fill);
  vec2 suv = uv + (uShadowPx / uAtlasSize);
  float sdist = (texture(uImg, suv).a - 0.5) * uDistRange;
  float shadowOutside = max(0.0, -sdist);
  float shadow = (1.0 - smoothstep(0.0, max(0.001, uGlow), shadowOutside)) * (1.0 - fill);
  vec3 color = fillColor;
  float alpha = fill;
  if (uEffect == 1) { color = mix(outlineColor, fillColor, fill); alpha = max(outline, fill); }
  else if (uEffect == 2) { color = mix(glowColor, fillColor, fill); alpha = max(fill, glow * 0.8); }
  else if (uEffect == 3) { color = mix(shadowColor, fillColor, fill); alpha = max(fill, shadow * 0.65); }
  else if (uEffect == 4) { vec3 combo = mix(glowColor, outlineColor, 0.5); color = mix(combo, fillColor, fill); alpha = max(fill, max(outline * 0.8, glow * 0.55)); }
  fragColor = vec4(color, alpha);
  if (fragColor.a < 0.001) discard;
}`;
