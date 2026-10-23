/* Bends flat page textures onto the curved paper with WebGL. Only the paper
 * surface lives here; the layout and all input stay in preview.js.
 *
 * render() draws one connected mesh, so no cracks open between strips. `mesh` is
 * a list of columns {u, normal, top: [x, y], bottom: [x, y]} in design units
 * (1672x941); `front` and `back` texture the sheet's two faces.
 */
window.createPageMesh = () => {
  'use strict';
  const canvas = document.createElement('canvas');
  const gl = canvas.getContext('webgl', {alpha: true, antialias: true, premultipliedAlpha: true});
  if (!gl) throw Error('This visual proof needs WebGL for its curved page animation.');

  function shader(kind, source) {
    const value = gl.createShader(kind);
    gl.shaderSource(value, source);
    gl.compileShader(value);
    if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) throw Error(gl.getShaderInfoLog(value));
    return value;
  }
  const program = gl.createProgram();
  gl.attachShader(program, shader(gl.VERTEX_SHADER, `
    attribute vec2 position;
    attribute vec2 texcoord;
    attribute float lighting;
    varying vec2 uv;
    varying float shade;
    void main() {
      // Design space to clip space.
      gl_Position = vec4(position.x / 836.0 - 1.0, 1.0 - position.y / 470.5, 0.0, 1.0);
      uv = texcoord;
      shade = lighting;
    }
  `));
  gl.attachShader(program, shader(gl.FRAGMENT_SHADER, `
    precision mediump float;
    uniform sampler2D front;
    uniform sampler2D back;
    varying vec2 uv;
    varying float shade;
    void main() {
      vec4 ink = gl_FrontFacing ? texture2D(front, uv) : texture2D(back, uv);
      gl_FragColor = vec4(ink.rgb * shade, ink.a);
    }
  `));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);

  // One interleaved vertex: position (2 floats), texcoord (2), lighting (1).
  const vertices = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
  for (const [name, size, offset] of [['position', 2, 0], ['texcoord', 2, 8], ['lighting', 1, 16]]) {
    const slot = gl.getAttribLocation(program, name);
    gl.enableVertexAttribArray(slot);
    gl.vertexAttribPointer(slot, size, gl.FLOAT, false, 20, offset);
  }
  const textures = [0, 1].map(index => {
    gl.activeTexture(gl.TEXTURE0 + index);
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.uniform1i(gl.getUniformLocation(program, index ? 'back' : 'front'), index);
    return texture;
  });
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);

  function upload(index, source) {
    gl.activeTexture(gl.TEXTURE0 + index);
    gl.bindTexture(gl.TEXTURE_2D, textures[index]);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
  }
  // The front texture is often the same canvas frame after frame, so it is only
  // uploaded when it changes (or when the caller redrew that canvas: refreshFront).
  let frontSource = null;

  function render(mesh, front, back, width, height, textureV, progress, refreshFront = false) {
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (refreshFront || frontSource !== front) {
      upload(0, front);
      frontSource = front;
    }
    upload(1, back);
    // Paper facing sideways mid-turn is darkened slightly.
    const point = (s, v) => [s.top[0], s.top[1] + (s.bottom[1] - s.top[1]) * v, s.u, v * textureV,
      1 - .15 * Math.sin(progress * Math.PI) * Math.abs(Math.sin(s.normal))];
    const data = [];
    for (let col = 0; col < mesh.length - 1; col++) for (let row = 0; row < 12; row++) {
      const a = point(mesh[col], row / 12), b = point(mesh[col], (row + 1) / 12);
      const c = point(mesh[col + 1], row / 12), d = point(mesh[col + 1], (row + 1) / 12);
      data.push(...a, ...b, ...c, ...c, ...b, ...d);
    }
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.STREAM_DRAW);
    gl.drawArrays(gl.TRIANGLES, 0, data.length / 5);
    return canvas;
  }

  return {canvas, render};
};
