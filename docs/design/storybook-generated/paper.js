/* Measures the printable paper inside the generated book-half texture.
 *
 * The texture is generated art, so its paper has no exact geometry. This finds the
 * cream sheet in the pixels and fits smooth curves to it, deliberately following
 * the broad curvature rather than individual edge or lighting defects. It runs once
 * per texture size, never per frame.
 *
 * Returns, in design units of the left page (x across the page, y down from book.y):
 *   normalized    the texture redrawn with its exaggerated slant corrected
 *   profile       256 {top, bottom} samples of the whole paper stack
 *   sheetProfile  the same for the single top sheet that lifts during a turn
 *   outerEdge(v)  x of the top sheet's outer edge at height v (0..1)
 */
window.measureBookPaper = ({image, content, book, leafSize, leafResolution}) => {
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const ease = v => v * v * (3 - 2 * v);

  // Correct the art's slant row by row: shift each pixel row left, more at the top.
  const normalized = document.createElement('canvas');
  normalized.width = normalized.height = leafSize;
  const ctx = normalized.getContext('2d');
  const [sx, sy, sw, sh] = content;
  for (let row = 0; row < Math.ceil(book.h * leafResolution); row++) {
    const v = row / (book.h * leafResolution), shift = -70 * (1 - v) * leafResolution;
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, row, leafSize, 1);
    ctx.clip();
    ctx.drawImage(image, sx, sy, sw, sh, shift, 0, book.half * leafResolution - shift, book.h * leafResolution);
    ctx.restore();
  }

  const pixels = ctx.getImageData(0, 0, leafSize, leafSize).data;
  const at = (x, y) => (y * leafSize + x) * 4;
  const isIvory = p => pixels[p + 3] > 230 && pixels[p] > 210 && pixels[p + 1] > 195 && pixels[p + 2] > 160;
  // Cooler and brighter than isIvory: the top sheet, not the warm gold leaves below it.
  const isTopSheet = p => pixels[p + 3] > 240 && pixels[p + 1] > 230 && pixels[p + 2] > pixels[p] * .925;

  // Scan 256 columns for the cream paper above the red cover and page stack.
  const stack = [], sheet = [];
  for (let i = 0; i < 256; i++) {
    const x = Math.min(leafSize - 1, Math.round((i + .5) / 256 * book.half * leafResolution));
    let top = 0, bottom = Math.min(leafSize - 1, Math.floor(book.h * leafResolution) - 1);
    while (top < bottom && !isIvory(at(x, top))) top++;
    while (bottom > top && !isIvory(at(x, bottom))) bottom--;

    // The last bright pixel belongs to the lower stack. The top sheet ends at the
    // first strong light-to-shadow edge; the largest drop can be the red cover
    // below the whole stack, so take the first real one.
    let edge = bottom;
    const step = Math.max(2, Math.round(2 * leafResolution));
    for (let y = Math.floor(book.h * leafResolution * .80); y < bottom - step; y++) {
      const above = at(x, y - step), below = at(x, y + step);
      if (!isIvory(above)) continue;
      if (pixels[above + 1] - pixels[below + 1] > 12) { edge = y - step; break; }
    }
    stack.push({top: top / leafResolution, bottom: Math.max(top / leafResolution, bottom / leafResolution - 14)});
    sheet.push({top: top / leafResolution, bottom: Math.max(top / leafResolution, edge / leafResolution)});
  }

  const topCurve = fitContour(stack, 'top'), bottomCurve = fitContour(stack, 'bottom');
  const profile = stack.map((_, i) => ({top: topCurve(i / 255), bottom: bottomCurve(i / 255)}));

  // The fit ignores the last 6% of the texture, which left the lifting sheet
  // ~17px above the real fold. Restore the measured hinge, blending only through
  // the inner 10% of the page.
  const sheetBottom = fitContour(sheet, 'bottom');
  const hingeCorrection = book.h * book.paperHingeV - sheetBottom(1);
  const sheetProfile = stack.map((sample, i) => ({
    top: sample.top < book.h * .2 ? sample.top : topCurve(i / 255),
    bottom: sheetBottom(i / 255) + hingeCorrection * ease(clamp((i / 255 - .90) / .10, 0, 1)),
  }));

  // The outer vertical edge also shows the dense lower stack. Fit a line to the top
  // sheet's edge in the middle rows. A bright highlight on the cover can pass a
  // single-pixel test, so require a continuous run of top-sheet paper.
  const edgeSamples = [];
  for (let i = 0; i < 48; i++) {
    const v = .18 + i / 47 * .62, y = Math.round(v * book.h * leafResolution);
    for (let x = 0; x < Math.floor(book.half * leafResolution * .28); x++) {
      const p = at(x, y);
      if (!isTopSheet(p)) continue;
      let run = true;
      for (let dx = 1; dx <= Math.ceil(12 * leafResolution); dx++) if (!isTopSheet(p + dx * 4)) { run = false; break; }
      if (run) { edgeSamples.push([v, x / leafResolution]); break; }
    }
  }
  const count = edgeSamples.length;
  const meanV = edgeSamples.reduce((sum, p) => sum + p[0], 0) / count;
  const meanX = edgeSamples.reduce((sum, p) => sum + p[1], 0) / count;
  const slope = edgeSamples.reduce((sum, p) => sum + (p[0] - meanV) * (p[1] - meanX), 0)
    / edgeSamples.reduce((sum, p) => sum + (p[0] - meanV) ** 2, 0);
  // No inset: an inset clipped the turning paper inside the visible fore-edge.
  const outerEdge = v => count > 2 ? clamp(meanX + slope * (v - meanV), 0, book.half * .28) : book.half * .12;

  return {normalized, profile, sheetProfile, outerEdge};

  // Least-squares quartic through the samples' `key` values, over the trusted
  // middle of the page (u from .12 to .94) and only where the sample saw real paper.
  // Normalized coordinates keep the small solve well conditioned.
  function fitContour(samples, key) {
    const degree = 4, normal = Array.from({length: degree + 1}, () => Array(degree + 2).fill(0));
    samples.forEach((sample, i) => {
      const u = (i + .5) / samples.length;
      if (u < .12 || u > .94 || sample.top > book.h * .2 || sample.bottom < book.h * .8) return;
      const x = 2 * u - 1, powers = Array.from({length: degree * 2 + 1}, (_, n) => x ** n);
      for (let row = 0; row <= degree; row++) {
        for (let col = 0; col <= degree; col++) normal[row][col] += powers[row + col];
        normal[row][degree + 1] += sample[key] * powers[row];
      }
    });
    // Gauss-Jordan elimination with partial pivoting.
    for (let k = 0; k <= degree; k++) {
      let pivot = k;
      for (let j = k + 1; j <= degree; j++) if (Math.abs(normal[j][k]) > Math.abs(normal[pivot][k])) pivot = j;
      [normal[k], normal[pivot]] = [normal[pivot], normal[k]];
      const divisor = normal[k][k];
      if (Math.abs(divisor) < 1e-8) return () => key === 'top' ? 0 : book.h;
      for (let j = k; j <= degree + 1; j++) normal[k][j] /= divisor;
      for (let row = 0; row <= degree; row++) if (row !== k) {
        const factor = normal[row][k];
        for (let j = k; j <= degree + 1; j++) normal[row][j] -= factor * normal[k][j];
      }
    }
    return u => normal.reduce((value, row, n) => value + row[degree + 1] * (2 * clamp(u, .12, .94) - 1) ** n, 0);
  }
};
