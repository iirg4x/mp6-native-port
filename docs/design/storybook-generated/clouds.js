/* Native cloud silhouettes, drawn as vector shapes rather than scaled textures.
 * All clouds share one contour and rim style. To fill a gap, place another cloud;
 * never resize a shape. Only the whole UI's DPI scale applies.
 */
window.createStoryClouds = () => {
  const style = {rim: '#fff9e8', outline: '#173489', rimWidth: 9, outlineWidth: 2.4, top: '#f1fcff', bottom: '#a7dcf7'};
  const shapes = {
    'cloud-round': {width: 252, height: 118, path: new Path2D('M 15 113 C 3 113 1 106 3 96 C 5 85 13 79 24 78 C 19 55 29 34 49 31 C 62 28 73 34 80 44 C 82 19 96 4 117 4 C 139 4 153 21 154 45 C 166 32 183 32 196 42 C 207 50 211 64 208 77 C 232 69 249 83 249 99 C 249 108 244 113 234 113 Z')},
    'cloud-bank': {width: 252, height: 83, path: new Path2D('M 13 78 C 3 78 1 71 5 62 C 9 53 19 49 29 51 C 27 31 39 18 56 18 C 70 18 80 27 83 39 C 86 16 101 4 120 4 C 141 4 155 19 156 39 C 165 29 179 25 191 31 C 205 37 212 49 208 60 C 224 48 244 56 248 66 C 251 74 245 78 235 78 Z')},
    'cloud': {width: 252, height: 82, path: new Path2D('M 15 77 C 4 77 1 69 4 60 C 7 49 19 43 30 46 C 26 29 36 14 52 12 C 68 10 79 20 84 34 C 92 14 106 4 124 4 C 146 4 160 21 160 41 C 172 29 189 30 201 40 C 209 47 212 54 211 62 C 226 51 243 55 248 66 C 251 73 245 77 236 77 Z')},
  };

  // A wide pale rim first, then the gradient body, then the thin navy outline on top.
  function paint(ctx, id, x, y, flip = false) {
    const shape = shapes[id];
    ctx.save();
    ctx.translate(x + (flip ? shape.width : 0), y);
    if (flip) ctx.scale(-1, 1);
    ctx.lineJoin = 'round';
    ctx.strokeStyle = style.rim;
    ctx.lineWidth = style.rimWidth;
    ctx.stroke(shape.path);
    const fill = ctx.createLinearGradient(0, 0, 0, shape.height);
    fill.addColorStop(0, style.top);
    fill.addColorStop(1, style.bottom);
    ctx.fillStyle = fill;
    ctx.fill(shape.path);
    ctx.strokeStyle = style.outline;
    ctx.lineWidth = style.outlineWidth;
    ctx.stroke(shape.path);
    ctx.restore();
  }

  return {style, shapes, paint};
};
