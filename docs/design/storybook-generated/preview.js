/* Visual proof only: no game imports, settings API, controllers or audio.
 * Intent: a player browsing MP6's picture book, not a launcher.
 * Palette: day yellow, night blue, ivory pages, red selection, gold, navy ink.
 * Depth: soft paper layers; smooth shading, no grain. Typography: bubbly Titan One.
 * Spacing: measured 1672x941 reference anchors, 8-unit details, 73-unit rows.
 *
 * How the book is drawn
 *   Everything is laid out in a fixed 1672x941 design space and scaled to the canvas.
 *   Each half of the book is a leaf: a square texture holding one page. Print goes
 *   onto the leaf flat and is then bent onto the curved paper (page-mesh.js), so
 *   text, photo and ornaments all follow the same deformation as the paper.
 *
 *   book.baseRestPages   both pages at rest, with only the print that never changes
 *                        with the selection: ornaments, illustration, caption, note.
 *   interaction layer    rows, ribbon, arrows and help text. Redrawn when the
 *                        selection or a value changes and composited on top.
 *   book.pages, turnPage, turnBackBase, restPages
 *                        fully printed copies for turning a sheet. Selecting makes
 *                        them stale, so they are rebuilt just before a turn.
 */
(async () => {
  'use strict';

  // Assets -----------------------------------------------------------------------

  const manifest = await fetch('manifest.json').then(response => response.json());
  const assets = manifest.assets, images = {};
  const referenceOnly = new Set(['cloud', 'cloud-round', 'cloud-bank']); // drawn as native shapes instead
  await Promise.all(Object.entries(assets).filter(([id]) => !referenceOnly.has(id)).map(async ([id, asset]) => {
    const image = new Image();
    image.src = asset.file;
    await image.decode();
    images[id] = image;
  }));
  await document.fonts.load('35px Chapters');

  // Layout, in design units ----------------------------------------------------------

  const W = 1672, H = 941;
  const ink = '#000000', blueInk = '#0900ed', paperColor = '#fbf9ee';
  const layout = {
    // Top-sheet fold measured at source y=975 in the generated book's content
    // rectangle y=4..1020. Its shaded edge is not reliably found by color cutoff.
    book: {x: 20, y: 82, half: 816, h: 779, spine: 836, paperHingeV: (975 - 4) / 1016},
    // Values are centered between the arrows' inner edges (563.5 to 747.5) and fit
    // valueWidth, which keeps 7.5 units of clearance from each arrow. The left arrow
    // stops short of the longest label (533) even at the peak of its 7-unit kick.
    rows: {y: 195, pitch: 73, label: 183, value: 655.5, valueWidth: 169, left: 555, right: 756},
    ribbon: [145, 147, 666, 72], cursor: [119, 156, 57, 59], photo: [896, 92, 640, 344],
    footer: [438, 877, 796, 56],
  };
  const helpColumn = 1216; // center line of the right-hand page's illustration and text
  const visibleRowCount = 6;
  // Click targets. Drawing positions stay next to the code that draws them.
  const hitZones = {
    tabsBottom: 105, // below this the book covers the tabs
    rows: {left: 140, right: 812, valueFrom: 540, decreaseUntil: 595},
    pager: {left: 365, right: 575, split: 470, top: 625, bottom: 680},
    footer: {choose: [520, 710], back: [724, 866], previous: [895, 976], next: [985, 1170]},
  };
  const motionTiming = {chapter: .64, selection: .19, value: .24, description: .20, tab: .28};

  // Menu and scenery -------------------------------------------------------------

  const params = new URLSearchParams(location.search);
  const chapters = window.createStoryChapters(), menu = window.createStoryMenu(chapters);
  const tabWidth = 245, tabGap = 16, tabStart = (W - chapters.length * tabWidth - (chapters.length - 1) * tabGap) / 2;
  const tabs = chapters.map((chapter, i) => [chapter.name, tabStart + i * (tabWidth + tabGap), tabWidth]);
  const clouds = window.createStoryClouds();
  const backdrop = window.createStoryBackdrop({width: W, height: H, mask: images['zigzag-mask']});
  // ?seed=N pins the confetti. Screenshot modes default to a fixed seed so captures
  // from separate loads agree.
  const seedParam = params.get('seed');
  const confettiSeed = seedParam !== null ? Number(seedParam) >>> 0 : params.has('still') || params.has('capture') ? 1 : undefined;
  const confetti = window.createConfettiEmitter({width: W, height: H, seed: confettiSeed});
  // Twinkling stars in the night sky: [x, y, size, color].
  const skyStars = [
    [1267, 15, 22, '#fff83b'], [1454, 29, 42, '#fff83b'], [1504, 64, 29, '#f6fcff'], [1649, 114, 25, '#f6fcff'],
    [1654, 176, 45, '#fff83b'], [1660, 252, 19, '#f6fcff'], [1653, 319, 34, '#fff83b'], [1665, 475, 17, '#f6fcff'],
    [1380, 897, 36, '#fff83b'], [1540, 920, 35, '#fff83b'], [1647, 914, 20, '#f6fcff'],
    // Continue behind both pages; the book occludes a complete star field.
    [940, 155, 28, '#fff83b'], [1130, 230, 36, '#f6fcff'], [1375, 180, 24, '#fff83b'],
    [1550, 340, 31, '#fff83b'], [995, 380, 22, '#f6fcff'], [1245, 420, 40, '#fff83b'],
    [1440, 520, 26, '#fff83b'], [910, 595, 34, '#f6fcff'], [1135, 645, 25, '#fff83b'],
    [1570, 720, 37, '#fff83b'], [1340, 755, 30, '#f6fcff'], [975, 830, 23, '#fff83b'],
  ];
  // [shape, x, y, mirrored]. The first five sit behind the characters, the rest in front.
  const cloudInstances = [
    ['cloud-round', -167, 593, false], ['cloud-bank', -84, 738, false], ['cloud', 24, 787, false],
    ['cloud-round', 1564, 610, true], ['cloud-bank', 1392, 773, true],
    ['cloud-round', -99, 792, false], ['cloud-bank', 1453, 802, true],
  ];

  // State ------------------------------------------------------------------------

  const reducedMotion = () => matchMedia('(prefers-reduced-motion: reduce)').matches;
  const state = {playing: !params.has('still') && !reducedMotion(), time: 0, chapter: 0, selected: 0,
    turnStart: null, pageProgress: 0, frames: 0};
  if (params.has('capture')) document.body.classList.add('capture');
  const selections = chapters.map(() => 0); // last selected row of each chapter
  let rows = chapters[0].rows;
  let transition = null; // chapter turn in flight
  let selectionMotion = null, valueMotion = null, descriptionMotion = null, tabMotion = null;
  let hoverTab = -1;
  let pauseAfterTurn = false;
  let lastTime = performance.now();

  // Canvases and caches ------------------------------------------------------------

  const canvas = document.querySelector('#stage'), stage = canvas.getContext('2d');
  const liveRegion = document.querySelector('#status'), motionButton = document.querySelector('#motion');
  const pageRenderer = window.createPageMesh(), printRenderer = window.createPageMesh();
  let resolution = 1;     // canvas pixels per design unit
  let leafSize = 2048;    // leaf texture size, always a power of two
  let leafResolution = 1; // leaf pixels per design unit
  let paper = {size: 0, profile: [], sheetProfile: []}; // measured once per leafSize
  let book = null;
  let bookStale = false;  // book.pages lag behind select/adjust until the next turn
  let needsDraw = true;
  let printLayer, rowInkLayer, turnBack; // scratch leaves
  const interaction = {key: '', pixels: null};
  const cursor = {key: '', face: null, tile: createCanvas(128, 128)};

  function createCanvas(width, height) {
    const out = document.createElement('canvas');
    out.width = width;
    out.height = height;
    return out;
  }
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const ease = v => v * v * (3 - 2 * v);
  const outCubic = p => 1 - (1 - p) ** 3;
  const leafTextureV = () => layout.book.h * leafResolution / leafSize; // page height within a leaf texture

  const rowOffset = (selected = state.selected) => Math.floor(selected / visibleRowCount) * visibleRowCount;
  const visibleRow = (selected = state.selected) => selected - rowOffset(selected);
  const visibleRows = () => rows.slice(rowOffset(), rowOffset() + visibleRowCount);
  const motionProgress = (motion, duration, t = state.time) => !motion || t < motion.start ? 1 : clamp((t - motion.start) / duration, 0, 1);
  // Ribbon position in rows, easing toward the selected row after a move.
  const rowPosition = (t = state.time) => selectionMotion
    ? selectionMotion.from + (visibleRow() - selectionMotion.from) * outCubic(motionProgress(selectionMotion, motionTiming.selection, t))
    : visibleRow();
  function settleInteractions() {
    selectionMotion = valueMotion = descriptionMotion = null;
    interaction.key = '';
  }

  // Drawing primitives -----------------------------------------------------------

  const tintedSprites = new Map();
  // Tints a sprite by multiplying, keeping its own alpha.
  function tintedSprite(id, color) {
    const key = id + color;
    if (tintedSprites.has(key)) return tintedSprites.get(key);
    const [w, h] = assets[id].size, tile = createCanvas(w, h), ctx = tile.getContext('2d');
    ctx.drawImage(images[id], 0, 0);
    ctx.globalCompositeOperation = 'multiply';
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, w, h);
    ctx.globalCompositeOperation = 'destination-in';
    ctx.drawImage(images[id], 0, 0);
    tintedSprites.set(key, tile);
    return tile;
  }

  // Draws a sprite's content rectangle into (x, y, w, h), rotated about its center.
  function art(ctx, id, x, y, w, h, angle = 0, flip = false, tint = null) {
    const [sx, sy, sw, sh] = assets[id].content;
    ctx.save();
    ctx.translate(x + w / 2, y + h / 2);
    ctx.rotate(angle);
    if (flip) ctx.scale(-1, 1);
    ctx.drawImage(tint ? tintedSprite(id, tint) : images[id], sx, sy, sw, sh, -w / 2, -h / 2, w, h);
    ctx.restore();
  }

  // Row text under the ribbon is recolored through the ribbon's own silhouette,
  // so the contrast follows its exact shape as it slides between rows.
  const ribbonInk = createCanvas(...assets['selection-ribbon'].size);
  {
    const ctx = ribbonInk.getContext('2d');
    ctx.drawImage(images['selection-ribbon'], 0, 0);
    ctx.globalCompositeOperation = 'source-in';
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, ribbonInk.width, ribbonInk.height);
  }

  // Shadow sizes are in design units; the canvas applies shadows in device pixels.
  function shadow(ctx, color, blur, x = 0, y = 0) {
    const matrix = ctx.getTransform(), scale = Math.hypot(matrix.a, matrix.b);
    ctx.shadowColor = color;
    ctx.shadowBlur = blur * scale;
    ctx.shadowOffsetX = x * scale;
    ctx.shadowOffsetY = y * scale;
  }

  // Text that does not fit maxWidth gets a smaller font; glyphs are never squeezed.
  function text(ctx, value, x, y, size = 40, color = ink, align = 'left', maxWidth = Infinity) {
    ctx.font = size + 'px Chapters';
    const width = ctx.measureText(value).width;
    if (width > maxWidth) ctx.font = (size * maxWidth / width) + 'px Chapters';
    ctx.textBaseline = 'alphabetic';
    ctx.textAlign = align;
    ctx.fillStyle = color;
    ctx.fillText(value, x, y);
  }

  function footerText(ctx, value, x, y) {
    text(ctx, value, x, y, 26, ink, 'left', value === 'Chapters' ? 146 : 130);
  }

  // Native labels such as "Auto (keep current)" keep the value column's size;
  // the parenthetical becomes a small second line instead of shrinking the word.
  function valueText(ctx, row, x, y, color) {
    const shown = menu.displayValue(row);
    const [, main, detail] = /^(.*) \((.*)\)$/.exec(shown) || [, shown];
    text(ctx, main, x, y, 32, color, 'center', layout.rows.valueWidth);
    if (detail) text(ctx, detail, x, y + 19, 17, color, 'center', 150);
  }

  // Help text on the right-hand page: wrapped to 600 units, centered vertically.
  function helpText(ctx, value) {
    ctx.font = '26px Chapters';
    const lines = [];
    let line = '';
    for (const word of value.split(' ')) {
      const next = line ? line + ' ' + word : word;
      if (line && ctx.measureText(next).width > 600) {
        lines.push(line);
        line = word;
      } else line = next;
    }
    if (line) lines.push(line);
    lines.forEach((wrapped, i) => text(ctx, wrapped, helpColumn, 552 - (lines.length - 1) * 16 + i * 32, 26, blueInk, 'center', 600));
  }

  function panel(ctx, x, y, w, h, r, top, bottom, stroke) {
    ctx.beginPath();
    ctx.roundRect(x, y, w, h, r);
    const fill = ctx.createLinearGradient(0, y, 0, y + h);
    fill.addColorStop(0, top);
    fill.addColorStop(1, bottom);
    ctx.fillStyle = fill;
    ctx.fill();
    if (stroke) {
      ctx.strokeStyle = stroke;
      ctx.lineWidth = 2;
      ctx.stroke();
    }
  }

  function star(ctx, x, y, size, color, angle = 0) {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(angle);
    ctx.beginPath();
    for (let i = 0; i < 10; i++) {
      const a = -Math.PI / 2 + i * Math.PI / 5, r = i % 2 ? size * .22 : size * .5, px = Math.cos(a) * r, py = Math.sin(a) * r;
      i ? ctx.lineTo(px, py) : ctx.moveTo(px, py);
    }
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.fill();
    ctx.restore();
  }

  // A gold photo corner; rot turns it to face each corner of the photo.
  function photoCorner(ctx, x, y, rot) {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(rot);
    shadow(ctx, '#9d761e55', 3, 0, 2);
    const fill = ctx.createLinearGradient(0, 0, 45, 45);
    fill.addColorStop(0, '#fff3a1');
    fill.addColorStop(1, '#ffd749');
    ctx.fillStyle = fill;
    ctx.beginPath();
    ctx.moveTo(5, 0);
    ctx.lineTo(51, 0);
    ctx.lineTo(0, 51);
    ctx.lineTo(0, 5);
    ctx.quadraticCurveTo(0, 0, 5, 0);
    ctx.fill();
    ctx.restore();
  }

  // A gold chevron pointing right (dir 1) or left (dir -1).
  function arrow(ctx, x, y, dir) {
    ctx.save();
    ctx.translate(x, y);
    ctx.scale(dir, 1);
    ctx.strokeStyle = '#ffbc16';
    ctx.lineWidth = 7;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.beginPath();
    ctx.moveTo(-5, -12);
    ctx.lineTo(5, 0);
    ctx.lineTo(-5, 12);
    ctx.stroke();
    ctx.restore();
  }

  // Footer controller buttons. A and B share one grayscale texture, tinted.
  function button(ctx, label, x, y, kind) {
    const round = kind === 'round', labelX = x + (round ? 24.5 : 28);
    ctx.save();
    shadow(ctx, '#16163566', 2.5, 0, 2);
    if (round) art(ctx, 'button-round', x, y, 49, 49, 0, false, label === 'A' ? '#11f64e' : '#ff1818');
    else art(ctx, 'button-shoulder', x, y, 54, 42, 0, label === 'R');
    ctx.shadowColor = 'transparent';
    ctx.font = '30px Chapters';
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.strokeStyle = '#171727';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'alphabetic';
    ctx.strokeText(label, labelX, y + 34);
    ctx.fillStyle = '#fff';
    ctx.fillText(label, labelX, y + 34);
    ctx.restore();
  }

  // The selected tab is a nine-slice of its texture: corners keep their concave
  // shape and inset thickness while the edges stretch. Drawn as one shell, so the
  // shadow is cast once rather than by nine tiles.
  const tabStyle = {sourceCorner: 40, cornerSize: 36};
  const tabTiles = new Map();
  function tabTexture(w, h) {
    const key = w + ':' + h;
    if (tabTiles.has(key)) return tabTiles.get(key);
    const tile = createCanvas(512, 256), ctx = tile.getContext('2d');
    const [sx, sy, sw, sh] = assets['tab-selected'].content, sourceEdge = tabStyle.sourceCorner, edge = tabStyle.cornerSize;
    const sourceX = [sx, sx + sourceEdge, sx + sw - sourceEdge, sx + sw], sourceY = [sy, sy + sourceEdge, sy + sh - sourceEdge, sy + sh];
    // Slice boundaries land on whole tile pixels. On a fractional boundary the two
    // neighbouring slices each cover part of the pixel, which left see-through seams
    // that split the tab into a visible 3x3 grid. Rounding the corners down keeps
    // them at or under their nominal size, and equal on opposite sides.
    const scaleX = 512 / w, scaleY = 256 / h;
    const destX = [0, Math.floor(edge * scaleX), Math.ceil((w - edge) * scaleX), 512];
    const destY = [0, Math.floor(edge * scaleY), Math.ceil((h - edge) * scaleY), 256];
    for (let row = 0; row < 3; row++) for (let col = 0; col < 3; col++) {
      ctx.drawImage(images['tab-selected'],
        sourceX[col], sourceY[row], sourceX[col + 1] - sourceX[col], sourceY[row + 1] - sourceY[row],
        destX[col], destY[row], destX[col + 1] - destX[col], destY[row + 1] - destY[row]);
    }
    tabTiles.set(key, tile);
    return tile;
  }

  // A shallow rounded binding joining the two cover halves, drawn under the leaves.
  function drawSpine(ctx) {
    const b = layout.book, x = b.spine, base = b.y + b.h;
    ctx.save();
    shadow(ctx, '#4a220a55', 2, 0, 2);
    const fill = ctx.createLinearGradient(0, base - 12, 0, base + 9);
    fill.addColorStop(0, '#f52b0c');
    fill.addColorStop(.35, '#dd1806');
    fill.addColorStop(1, '#821005');
    ctx.fillStyle = fill;
    ctx.beginPath();
    ctx.moveTo(x - 58, base - 10);
    ctx.bezierCurveTo(x - 29, base - 10, x - 21, base - 3, x, base - 3);
    ctx.bezierCurveTo(x + 21, base - 3, x + 29, base - 10, x + 58, base - 10);
    ctx.lineTo(x + 58, base - 2);
    ctx.bezierCurveTo(x + 28, base - 2, x + 21, base + 9, x, base + 9);
    ctx.bezierCurveTo(x - 21, base + 9, x - 28, base - 2, x - 58, base - 2);
    ctx.closePath();
    ctx.fill();
    ctx.shadowColor = 'transparent';
    ctx.strokeStyle = '#ff5127';
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    ctx.moveTo(x - 55, base - 8);
    ctx.bezierCurveTo(x - 27, base - 8, x - 18, base - 1, x, base - 1);
    ctx.bezierCurveTo(x + 18, base - 1, x + 27, base - 8, x + 55, base - 8);
    ctx.stroke();
    ctx.restore();
  }

  // Book geometry ------------------------------------------------------------------

  // The paper sags toward the spine; the drop is strongest at the top edge.
  function pagePoint(side, x, y) {
    const b = layout.book, u = (x - b.x - side * b.half) / b.half, drop = 27 * Math.pow(side ? 1 - u : u, 10);
    return [x, y + drop * (1 - (y - b.y) / b.h)];
  }

  // The paper's {top, bottom} at canvas x, in units below book.y.
  function pageBand(side, x, profile = paper.profile) {
    const b = layout.book, u = clamp((x - b.x - side * b.half) / b.half, 0, 1);
    if (!profile.length) return {top: 0, bottom: b.h};
    const index = clamp((side ? 1 - u : u) * 255, 0, 255), lo = Math.floor(index), hi = Math.min(255, lo + 1), t = index - lo;
    return {top: profile[lo].top * (1 - t) + profile[hi].top * t, bottom: profile[lo].bottom * (1 - t) + profile[hi].bottom * t};
  }

  // Maps a point in flat page layout onto the curved paper, and back.
  function surfacePoint(side, x, y) {
    const b = layout.book, band = pageBand(side, x);
    return pagePoint(side, x, b.y + band.top + (y - b.y) / b.h * (band.bottom - band.top));
  }
  function unprojectPoint(side, x, y) {
    const b = layout.book, top = surfacePoint(side, x, b.y)[1], bottom = surfacePoint(side, x, b.y + b.h)[1];
    return [x, b.y + (y - top) * b.h / (bottom - top)];
  }

  // The turning sheet as 65 columns hinged at the spine. Positive z lifts toward the
  // viewer, so perspective enlarges raised paper. The curl follows the side of the
  // binding the sheet is on, so it never folds back across its own hinge.
  function leafMesh(progress) {
    const b = layout.book, angle = progress * Math.PI, bend = .5 * Math.sin(2 * angle), steps = 64;
    const samples = [{u: 0, x: 0, z: 0, normal: angle}];
    let x = 0, z = 0;
    for (let i = 1; i <= steps; i++) {
      const u = (i - .5) / steps, a = angle - bend * Math.sin(u * Math.PI), distance = b.half / steps;
      x += Math.cos(a) * distance;
      z += Math.sin(a) * distance;
      samples.push({u: i / steps, x, z, normal: a});
    }
    return samples.map(sample => {
      const {u, x, z} = sample, perspective = 4800 / (4800 - z), drop = 27 * (1 - u) ** 10;
      const project = y => [b.spine + x * perspective, 470 + (b.y + drop + y * (b.h - drop) - 470) * perspective];
      return {...sample, top: project(0), bottom: project(1)};
    });
  }
  function meshPoint(sample, v) {
    return [sample.top[0], sample.top[1] + v * (sample.bottom[1] - sample.top[1])];
  }

  // Fits the captured game frame inside the photo area without cropping it.
  function photoBounds() {
    const [x, y, w, h] = layout.photo, [, , sw, sh] = assets['game-preview'].content, scale = Math.min(w / sw, h / sh);
    return [x + (w - sw * scale) / 2, y + (h - sh * scale) / 2, sw * scale, sh * scale];
  }

  // Printing pages -----------------------------------------------------------------

  // Runs paint(ctx) on a leaf texture, with design units mapped onto that page.
  function inLeaf(target, side, paint) {
    const ctx = target.getContext('2d');
    ctx.save();
    ctx.setTransform(leafResolution, 0, 0, leafResolution, 0, 0);
    ctx.translate(-layout.book.x - side * layout.book.half, -layout.book.y);
    paint(ctx);
    ctx.restore();
  }

  // Prints flat into a scratch leaf, then bends that print onto this page's paper
  // band. The mesh shader projects the full design space onto its canvas, so a
  // leaf (book.half units square) is described here in W by H/book.half units.
  function printOnPaper(ctx, side, paint) {
    const b = layout.book;
    if (!printLayer || printLayer.width !== leafSize) printLayer = createCanvas(leafSize, leafSize);
    printLayer.getContext('2d').clearRect(0, 0, leafSize, leafSize);
    inLeaf(printLayer, side, paint);
    const mesh = Array.from({length: 129}, (_, i) => {
      const u = i / 128, band = pageBand(side, b.x + (side + u) * b.half);
      return {u, normal: 0, top: [u * W, band.top / b.half * H], bottom: [u * W, band.bottom / b.half * H]};
    });
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(printRenderer.render(mesh, printLayer, printLayer, leafSize, leafSize, b.h / b.half, 0, true), 0, 0);
    ctx.restore();
  }

  // A faint star trail in the lower margin: paper detail without grain or rules.
  function printStarTrail(ctx, side) {
    const offset = side * layout.book.half;
    const trail = [[344, 719, 17], [432, 743, 25], [518, 700, 48], [624, 729, 20], [688, 688, 11]];
    ctx.save();
    ctx.strokeStyle = '#c9a450';
    ctx.lineWidth = 1;
    ctx.globalAlpha = .15;
    ctx.beginPath();
    trail.forEach(([x, y], i) => i ? ctx.lineTo(x + offset, y) : ctx.moveTo(x + offset, y));
    ctx.stroke();
    trail.forEach(([x, y, size]) => star(ctx, x + offset, y, size, '#c8a246', .12));
    ctx.restore();
  }

  // Two gold rules following the real top-sheet outline, not a rectangle squeezed
  // through the page mapping. Top, sides and bottom share one inset.
  const pageBorderStyle = {inset: 27, gap: 7};
  function pageBorderPoints(side, inset = pageBorderStyle.inset) {
    const b = layout.book, points = [], right = b.half - inset - 12;
    const top = x => pageBand(0, b.x + x).top + inset, bottom = x => pageBand(0, b.x + x, paper.sheetProfile).bottom - inset;
    const leftTop = paper.outerEdge(top(100) / b.h) + inset, leftBottom = paper.outerEdge(bottom(100) / b.h) + inset;
    const push = (x, y) => points.push([side ? b.spine + b.half - x : b.x + x, b.y + y]);
    push(leftTop, top(leftTop));
    for (let i = 1; i <= 48; i++) {
      const x = leftTop + (right - leftTop) * i / 48;
      push(x, top(x));
    }
    push(right, bottom(right));
    for (let i = 47; i >= 0; i--) {
      const x = leftBottom + (right - leftBottom) * i / 48;
      push(x, bottom(x));
    }
    return points;
  }
  function printBorder(ctx, side) {
    const radius = 16;
    ctx.save();
    ctx.lineJoin = 'round';
    for (const [inset, alpha, width] of [[pageBorderStyle.inset, .55, 1.5], [pageBorderStyle.inset + pageBorderStyle.gap, .23, 1]]) {
      ctx.strokeStyle = '#c9a450';
      ctx.globalAlpha = alpha;
      ctx.lineWidth = width;
      const points = pageBorderPoints(side, inset);
      // Round every corner: cut each point short on both sides and curve through it.
      ctx.beginPath();
      points.forEach((p, i) => {
        const prev = points[(i + points.length - 1) % points.length], next = points[(i + 1) % points.length];
        const before = Math.min(radius, Math.hypot(p[0] - prev[0], p[1] - prev[1]) * .45);
        const after = Math.min(radius, Math.hypot(next[0] - p[0], next[1] - p[1]) * .45);
        const towards = (q, d) => {
          const length = Math.hypot(q[0] - p[0], q[1] - p[1]);
          return [p[0] + (q[0] - p[0]) * d / length, p[1] + (q[1] - p[1]) * d / length];
        };
        const start = towards(prev, before), end = towards(next, after);
        i ? ctx.lineTo(...start) : ctx.moveTo(...start);
        ctx.quadraticCurveTo(...p, ...end);
      });
      ctx.closePath();
      ctx.stroke();
    }
    ctx.restore();
  }

  function printPhoto(ctx) {
    const [x, y, w, h] = photoBounds(), [sx, sy, sw, sh] = assets['game-preview'].content;
    ctx.save();
    shadow(ctx, '#57451c55', 11, 1, 7); // the photo's own shadow, so it bends with the paper
    ctx.fillStyle = '#fff';
    ctx.fillRect(x - 5, y - 5, w + 10, h + 10);
    ctx.restore();
    ctx.drawImage(images['game-preview'], sx, sy, sw, sh, x, y, w, h);
    const corners = [[x - 7, y - 7, 0], [x + w + 7, y - 7, Math.PI / 2], [x + w + 7, y + h + 7, Math.PI], [x - 7, y + h + 7, -Math.PI / 2]];
    corners.forEach(([cx, cy, rot]) => photoCorner(ctx, cx, cy, rot));
  }

  // The right-hand page picture: the game photo, or a chapter illustration on a halo.
  function printIllustration(ctx, chapter) {
    if (chapter.art === 'picture') {
      printPhoto(ctx);
      return;
    }
    const x = helpColumn, y = 310;
    ctx.save();
    const halo = ctx.createRadialGradient(x, y, 20, x, y, 215);
    halo.addColorStop(0, '#fff1b6');
    halo.addColorStop(1, '#fff1b600');
    ctx.fillStyle = halo;
    ctx.beginPath();
    ctx.ellipse(x, y, 245, 163, 0, 0, Math.PI * 2);
    ctx.fill();
    if (chapter.art === 'sound' || chapter.art === 'game') {
      const id = 'chapter-' + chapter.art, [, , sw, sh] = assets[id].content, size = 250, scale = size / Math.max(sw, sh);
      art(ctx, id, x - sw * scale / 2, y - sh * scale / 2 - 12, sw * scale, sh * scale);
    } else {
      art(ctx, 'bubble-star', x - 69, y - 92, 138, 138, -.08);
      art(ctx, 'bubble-star', x - 186, y - 9, 74, 74, -.23);
      art(ctx, 'bubble-star', x + 123, y - 26, 64, 64, .22);
    }
    ctx.restore();
  }

  // Everything printed on one page. `print` chooses how much:
  //   'paper'   only the page ornaments (the native export's blank paper)
  //   'static'  plus everything that does not change with the selection
  //   'full'    plus the rows, ribbon and help text (the copies used for turning)
  function printPage(ctx, side, print) {
    printStarTrail(ctx, side);
    if (print === 'paper') return;
    const full = print === 'full';
    if (!side) {
      if (full) {
        const [x, y, w, h] = layout.ribbon;
        art(ctx, 'selection-ribbon', x, y + visibleRow() * layout.rows.pitch, w, h);
        visibleRows().forEach((row, i) => {
          const baseline = layout.rows.y + i * layout.rows.pitch, color = i === visibleRow() ? '#fff' : ink;
          ctx.save();
          if (menu.isDisabled(row)) ctx.globalAlpha = .5;
          text(ctx, row.label, layout.rows.label, baseline, 35, color, 'left', row.options ? 350 : 580);
          valueText(ctx, row, layout.rows.value, baseline, color);
          if (row.options && !menu.isDisabled(row)) {
            arrow(ctx, layout.rows.left, baseline - 14, -1);
            arrow(ctx, layout.rows.right, baseline - 14, 1);
          }
          ctx.restore();
        });
      }
      if (rows.length > visibleRowCount) {
        const page = Math.floor(state.selected / visibleRowCount) + 1, pageCount = Math.ceil(rows.length / visibleRowCount);
        text(ctx, page + ' / ' + pageCount, 470, 662, 23, ink, 'center');
        arrow(ctx, 400, 652, -1);
        arrow(ctx, 540, 652, 1);
      }
      return;
    }
    const chapter = chapters[state.chapter];
    printIllustration(ctx, chapter);
    const [, photoY, , photoHeight] = photoBounds();
    text(ctx, chapter.caption, helpColumn, photoY + photoHeight + 40, 26, ink, 'center', 590);
    if (full) helpText(ctx, rows[state.selected].help);
    ctx.strokeStyle = '#f6d000';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(909, 622);
    ctx.lineTo(1180, 622);
    ctx.moveTo(1227, 622);
    ctx.lineTo(1422, 622);
    ctx.stroke();
    art(ctx, 'bubble-star', 1189, 606, 32, 32);
    text(ctx, chapter.note, 1210, 669, 25, ink, 'center', 570);
  }

  // Book surfaces ------------------------------------------------------------------

  function ensurePaper() {
    if (paper.size === leafSize) return;
    paper = {size: leafSize, ...window.measureBookPaper({
      image: images['book-half'], content: assets['book-half'].content, book: layout.book, leafSize, leafResolution,
    })};
    paper.sheetMask = sheetMask(blankLeaf(1));
  }

  // The bare paper of one page. The right page is the same texture, mirrored.
  function blankLeaf(side) {
    const leaf = createCanvas(leafSize, leafSize), ctx = leaf.getContext('2d');
    if (side) {
      ctx.translate(leafSize, 0);
      ctx.scale(-1, 1);
    }
    ctx.drawImage(paper.normalized, 0, 0);
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    return leaf;
  }

  function mirrored(leaf) {
    const out = createCanvas(leafSize, leafSize), ctx = out.getContext('2d');
    ctx.translate(leafSize, 0);
    ctx.scale(-1, 1);
    ctx.drawImage(leaf, 0, 0);
    return out;
  }

  // Paper, border rules and print for one page.
  function printedLeaf(side, print) {
    const leaf = blankLeaf(side);
    inLeaf(leaf, side, ctx => {
      printBorder(ctx, side);
      printOnPaper(ctx, side, printCtx => printPage(printCtx, side, print));
    });
    return leaf;
  }

  // Alpha mask of the top sheet on the bare right page. The red hardcover stays
  // stationary, even where its silhouette shares the leaf's outer edge.
  function sheetMask(blank) {
    const mask = createCanvas(leafSize, leafSize);
    const pixels = blank.getContext('2d').getImageData(0, 0, leafSize, leafSize);
    for (let i = 0; i < pixels.data.length; i += 4) {
      const red = pixels.data[i], green = pixels.data[i + 1], blue = pixels.data[i + 2];
      if (green < red * .72 || blue < red * .52) pixels.data[i + 3] = 0;
      else if (pixels.data[i + 3] > 230) pixels.data[i + 3] = 255;
    }
    mask.getContext('2d').putImageData(pixels, 0, 0);
    return mask;
  }

  // Lifts the top sheet out of a printed right page: the static print itself, not
  // a redrawn approximation. Clear areas are backed with plain paper, then the face
  // is cut to the sheet's band, the paper mask and the sheet's outer edge.
  function turnFace(source) {
    const b = layout.book, face = createCanvas(leafSize, leafSize), ctx = face.getContext('2d');
    ctx.drawImage(source, 0, 0);
    ctx.globalCompositeOperation = 'destination-over';
    ctx.fillStyle = paperColor;
    ctx.fillRect(0, 0, leafSize, leafSize);
    ctx.globalCompositeOperation = 'destination-in';
    ctx.beginPath();
    for (let i = 0; i < 256; i++) {
      const x = i / 255 * b.half, band = pageBand(1, b.spine + x, paper.sheetProfile);
      i ? ctx.lineTo(x * leafResolution, band.top * leafResolution) : ctx.moveTo(x * leafResolution, band.top * leafResolution);
    }
    for (let i = 255; i >= 0; i--) {
      const x = i / 255 * b.half;
      ctx.lineTo(x * leafResolution, pageBand(1, b.spine + x, paper.sheetProfile).bottom * leafResolution);
    }
    ctx.closePath();
    ctx.fill();
    ctx.drawImage(paper.sheetMask, 0, 0);
    ctx.beginPath();
    ctx.moveTo(0, 0);
    for (let i = 0; i <= 64; i++) {
      const v = i / 64;
      ctx.lineTo((b.half - paper.outerEdge(v)) * leafResolution, v * b.h * leafResolution);
    }
    ctx.lineTo(0, b.h * leafResolution);
    ctx.closePath();
    ctx.fill();
    return face;
  }

  // Draws a flat leaf with its gutter dropping toward the spine, in 256 pixel-aligned
  // columns. Each column samples the whole image, which avoids seams between them.
  function warpLeaf(ctx, source, side) {
    const b = layout.book, total = b.half * resolution;
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    for (let i = 0; i < 256; i++) {
      const u = i / 256, v = (i + 1) / 256, gutter = side ? 1 - u : u, drop = 27 * Math.pow(gutter, 10);
      const x0 = b.x + side * b.half + u * b.half, x1 = b.x + side * b.half + v * b.half;
      const left = Math.round(Math.min(x0, x1) * resolution), right = Math.round(Math.max(x0, x1) * resolution);
      if (left === right) continue;
      ctx.save();
      ctx.beginPath();
      ctx.rect(left, 0, right - left, canvas.height);
      ctx.clip();
      const y = (b.y + drop) * resolution, h = (b.h - drop) * resolution;
      ctx.drawImage(source, 0, 0, b.half * leafResolution, b.h * leafResolution, (b.x + side * b.half) * resolution, y, total, h);
      ctx.restore();
    }
    ctx.restore();
  }

  // The open book at rest: binding, both leaves, the top sheet lying on each side,
  // and the crease at the fold.
  function composeSpread(leaves, front, back) {
    const b = layout.book, spread = createCanvas(canvas.width, canvas.height), ctx = spread.getContext('2d');
    ctx.setTransform(resolution, 0, 0, resolution, 0, 0);
    drawSpine(ctx); // under the leaves; on top, its ends showed over the paper
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    warpLeaf(ctx, leaves[0], 0);
    warpLeaf(ctx, leaves[1], 1);
    // The resting sheet uses the same mesh as a turn, so lift-off cannot pop.
    for (const progress of [0, 1]) {
      ctx.drawImage(pageRenderer.render(leafMesh(progress), front, back, canvas.width, canvas.height, leafTextureV(), progress), 0, 0);
    }
    ctx.setTransform(resolution, 0, 0, resolution, 0, 0);
    const crease = ctx.createLinearGradient(833, 0, 839, 0);
    crease.addColorStop(0, '#eee6cf');
    crease.addColorStop(.5, '#e8dcb9');
    crease.addColorStop(1, '#eee6cf');
    ctx.fillStyle = crease;
    ctx.fillRect(833, 119, 6, pagePoint(1, b.spine, b.y + b.h * b.paperHingeV)[1] - 119);
    return spread;
  }

  function createBook() {
    ensurePaper();
    const pages = [0, 1].map(side => printedLeaf(side, 'full'));
    const turnPage = turnFace(pages[1]), turnBackBase = turnFace(mirrored(pages[0]));
    const restPages = composeSpread(pages, turnPage, turnBackBase);
    const bases = [0, 1].map(side => printedLeaf(side, 'static'));
    const baseRestPages = composeSpread(bases, turnFace(bases[1]), turnFace(mirrored(bases[0])));
    return {pages, turnPage, turnBackBase, restPages, baseRestPages};
  }

  function rebuildBook() {
    book = createBook();
    interaction.key = '';
    bookStale = false;
    needsDraw = true;
  }

  // During a chapter turn the moving sheet mixes the outgoing and incoming pages.
  const turningSurfaces = () => transition ? transition.surfaces : book;

  // Interaction layer --------------------------------------------------------------

  // Row labels and values, recolored white wherever the ribbon covers them.
  function printRowInk(position, vp) {
    if (!rowInkLayer || rowInkLayer.width !== leafSize) rowInkLayer = createCanvas(leafSize, leafSize);
    rowInkLayer.getContext('2d').clearRect(0, 0, leafSize, leafSize);
    inLeaf(rowInkLayer, 0, ctx => {
      visibleRows().forEach((row, i) => {
        const baseline = layout.rows.y + i * layout.rows.pitch, active = i === visibleRow();
        ctx.save();
        if (menu.isDisabled(row)) ctx.globalAlpha = .5;
        text(ctx, row.label, layout.rows.label, baseline, 35, ink, 'left', row.options ? 350 : 580);
        ctx.save();
        ctx.translate(layout.rows.value, baseline);
        if (active && valueMotion && vp < 1) { // a changed value slides in and pops
          const pop = Math.sin(vp * Math.PI);
          ctx.translate(valueMotion.direction * 12 * (1 - outCubic(vp)), 0);
          ctx.scale(1 + .1 * pop, 1 + .1 * pop);
        }
        valueText(ctx, row, 0, 0, ink);
        ctx.restore();
        ctx.restore();
      });
      const [x, y, w, h] = layout.ribbon, [sx, sy, sw, sh] = assets['selection-ribbon'].content;
      ctx.globalCompositeOperation = 'source-atop';
      ctx.drawImage(ribbonInk, sx, sy, sw, sh, x, y + position * layout.rows.pitch, w, h);
    });
    return rowInkLayer;
  }

  // Left page: the ribbon, the row ink once, then the arrows, which kick outward
  // as their value changes.
  function printSelection(ctx, rowInk, position, vp) {
    const [x, y, w, h] = layout.ribbon;
    art(ctx, 'selection-ribbon', x, y + position * layout.rows.pitch, w, h);
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(rowInk, 0, 0);
    ctx.restore();
    visibleRows().forEach((row, i) => {
      if (!row.options || menu.isDisabled(row)) return;
      const baseline = layout.rows.y + i * layout.rows.pitch, active = i === visibleRow();
      const kick = active && valueMotion ? 7 * Math.sin(vp * Math.PI) : 0;
      arrow(ctx, layout.rows.left - (valueMotion?.direction < 0 ? kick : 0), baseline - 14, -1);
      arrow(ctx, layout.rows.right + (valueMotion?.direction > 0 ? kick : 0), baseline - 14, 1);
    });
  }

  // Right page: the old help text lifts away as the new one settles in.
  function printHelp(ctx, dp) {
    const exit = ease(clamp(dp / .5, 0, 1)), enter = ease(clamp((dp - .35) / .65, 0, 1));
    if (descriptionMotion && dp < .5) {
      ctx.save();
      ctx.globalAlpha = 1 - exit;
      ctx.translate(0, -10 * exit);
      helpText(ctx, descriptionMotion.from);
      ctx.restore();
    }
    ctx.save();
    ctx.globalAlpha = descriptionMotion ? enter : 1;
    ctx.translate(0, descriptionMotion ? 10 * (1 - enter) : 0);
    helpText(ctx, rows[state.selected].help);
    ctx.restore();
  }

  // Everything that changes with the selection, bent onto both pages as one
  // surface. Cached until the selection, a value or a motion step changes.
  function interactionLayer(t) {
    const position = rowPosition(t);
    const vp = motionProgress(valueMotion, motionTiming.value, t), dp = motionProgress(descriptionMotion, motionTiming.description, t);
    const key = [state.chapter, state.selected, position, vp, dp, leafSize, resolution].join(':');
    if (interaction.key === key) return interaction.pixels;
    interaction.key = key;
    const pixels = interaction.pixels;
    if (!pixels || pixels.width !== canvas.width || pixels.height !== canvas.height) interaction.pixels = createCanvas(canvas.width, canvas.height);

    const rowInk = printRowInk(position, vp);
    const layers = [0, 1].map(side => {
      const layer = createCanvas(leafSize, leafSize);
      inLeaf(layer, side, ctx => printOnPaper(ctx, side, printCtx => side ? printHelp(printCtx, dp) : printSelection(printCtx, rowInk, position, vp)));
      return layer;
    });
    const reverse = mirrored(layers[0]);
    const ctx = interaction.pixels.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    for (const progress of [1, 0]) {
      ctx.drawImage(pageRenderer.render(leafMesh(progress), layers[1], reverse, canvas.width, canvas.height, leafTextureV(), progress), 0, 0);
    }
    return interaction.pixels;
  }

  // Cursor and turning sheet -------------------------------------------------------

  // The cursor star, printed onto the left page's back face so it bends exactly
  // like the rest of the print: one leaf-pixel column at a time, following the
  // paper band. Stored mirrored, in the back face's texture orientation.
  function cursorFace(t, selected, position = visibleRow(selected)) {
    const key = [t, selected, position, leafSize].join(':');
    if (cursor.key === key) return cursor.face;
    if (!cursor.face || cursor.face.width !== leafSize) cursor.face = createCanvas(leafSize, leafSize);
    cursor.key = key;
    const b = layout.book, [cx, cy, cw, ch] = layout.cursor, pulse = 1 + .035 * Math.sin(t * 3);
    const tileCtx = cursor.tile.getContext('2d');
    tileCtx.clearRect(0, 0, 128, 128);
    art(tileCtx, 'cursor-star', 64 - 48 * pulse, 64 - 48 * pulse, 96 * pulse, 96 * pulse, Math.sin(t * 2) * .065);
    const hop = selectionMotion && !transition ? Math.sin(Math.PI * motionProgress(selectionMotion, motionTiming.selection, t)) : 0;
    const w = cw * 128 / 96, h = ch * 128 / 96, x = cx - (w - cw) / 2 - 7 * hop, y = cy + position * layout.rows.pitch - (h - ch) / 2;
    const ctx = cursor.face.getContext('2d');
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, leafSize, leafSize);
    const left = Math.floor((x - b.x) * leafResolution), right = Math.ceil((x + w - b.x) * leafResolution);
    for (let pixel = left; pixel < right; pixel++) {
      const sample = b.x + (pixel + .5) / leafResolution, band = pageBand(0, sample), heightScale = (band.bottom - band.top) / b.h;
      const top = band.top + (y - b.y) * heightScale;
      ctx.save();
      ctx.beginPath();
      ctx.rect(leafSize - pixel - 1, 0, 1, leafSize);
      ctx.clip();
      ctx.translate(leafSize, 0);
      ctx.scale(-1, 1);
      ctx.drawImage(cursor.tile, (x - b.x) * leafResolution, top * leafResolution, w * leafResolution, h * heightScale * leafResolution);
      ctx.restore();
    }
    return cursor.face;
  }

  function drawCursor(ctx, t, selected = state.selected) {
    const position = transition || state.turnStart !== null ? visibleRow(selected) : rowPosition(t);
    const surface = pageRenderer.render(leafMesh(1), turningSurfaces().turnPage, cursorFace(t, selected, position),
      canvas.width, canvas.height, leafTextureV(), 1);
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(surface, 0, 0);
    ctx.restore();
  }

  function drawTurningSheet(ctx, progress, t) {
    if (!transition && (progress <= 0 || progress >= 1)) return;
    const b = layout.book, mesh = leafMesh(progress), surfaces = turningSurfaces();

    // The back face is the static print plus the cursor star as it sat on that page.
    if (!turnBack || turnBack.width !== leafSize) turnBack = createCanvas(leafSize, leafSize);
    const back = turnBack.getContext('2d');
    back.setTransform(1, 0, 0, 1, 0, 0);
    back.clearRect(0, 0, leafSize, leafSize);
    back.drawImage(surfaces.turnBackBase, 0, 0);
    back.drawImage(cursorFace(t, transition ? transition.backSelected : state.selected), 0, 0);

    // A soft shadow falls onto the stationary paper of both pages. It is never
    // stored in the lifted sheet, and fades out as the sheet comes to rest.
    const amount = Math.sin(progress * Math.PI);
    ctx.save();
    ctx.beginPath();
    for (const side of [0, 1]) {
      for (let i = 0; i <= 64; i++) {
        const x = b.x + side * b.half + i / 64 * b.half, p = pagePoint(side, x, b.y + pageBand(side, x, paper.sheetProfile).top);
        i ? ctx.lineTo(...p) : ctx.moveTo(...p);
      }
      for (let i = 64; i >= 0; i--) {
        const x = b.x + side * b.half + i / 64 * b.half;
        ctx.lineTo(...pagePoint(side, x, b.y + pageBand(side, x, paper.sheetProfile).bottom));
      }
      ctx.closePath();
    }
    ctx.clip();
    ctx.beginPath();
    mesh.forEach((s, i) => {
      const band = pageBand(1, b.spine + s.u * b.half, paper.sheetProfile), p = meshPoint(s, band.top / b.h);
      i ? ctx.lineTo(p[0] + s.z * .045, p[1] + s.z * .04) : ctx.moveTo(...p);
    });
    [...mesh].reverse().forEach(s => {
      const p = meshPoint(s, pageBand(1, b.spine + s.u * b.half, paper.sheetProfile).bottom / b.h);
      ctx.lineTo(p[0] + s.z * .045, p[1] + s.z * .04);
    });
    ctx.closePath();
    ctx.filter = 'blur(' + (8 * resolution) + 'px)';
    ctx.fillStyle = 'rgba(77,48,20,' + (.16 * amount) + ')';
    ctx.fill();
    ctx.restore();

    // The sheet is one connected mesh, so no cracks open between strips.
    const surface = pageRenderer.render(mesh, surfaces.turnPage, turnBack, canvas.width, canvas.height, leafTextureV(), progress);
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(surface, 0, 0);
    ctx.restore();
  }

  // Scene --------------------------------------------------------------------------

  // Broad flat rays, separate from the sun sprite. The night mask drawn next clips
  // them to the day side, and the book hides their roots.
  function drawSunbeams(ctx, t) {
    ctx.save();
    ctx.translate(175, 114);
    ctx.rotate(t * .11);
    ctx.fillStyle = '#fff98d';
    for (let i = 0; i < 14; i++) {
      const a = i * Math.PI / 7, r = 690;
      ctx.beginPath();
      ctx.moveTo(0, 0);
      ctx.lineTo(Math.cos(a - .075) * r, Math.sin(a - .075) * r);
      ctx.lineTo(Math.cos(a + .075) * r, Math.sin(a + .075) * r);
      ctx.closePath();
      ctx.fill();
    }
    ctx.restore();
  }

  function drawParticles(ctx, t) {
    skyStars.forEach(([x, y, size, color], i) => {
      const px = x + (Math.sin(t * .55 + i) - Math.sin(i)) * 8, py = y + (Math.sin(t * .65 + i * .4) - Math.sin(i * .4)) * 10;
      const glow = .5 + .5 * Math.sin(t * (1.4 + i * .035) + i), pulse = .94 + .06 * Math.cos(t * 1.5 + i);
      ctx.save();
      ctx.globalAlpha = .48 + .52 * glow;
      shadow(ctx, color, 2 + 7 * glow);
      star(ctx, px, py, size * pulse, color, Math.sin(t * .9 + i) * .14);
      ctx.restore();
    });
    confetti.at(t).forEach(({x, y, size, color, angle, fold}) => {
      ctx.save();
      ctx.translate(x, y);
      ctx.rotate(angle);
      ctx.scale(fold, 1);
      ctx.fillStyle = color;
      ctx.fillRect(-size * .38, -size * .5, size * .76, size);
      ctx.restore();
    });
  }

  function drawBackground(ctx, t) {
    backdrop.day(ctx);
    drawSunbeams(ctx, t);
    backdrop.dayDetails(ctx, t);
    backdrop.nightDetails(ctx, t);
    drawParticles(ctx, t);
    art(ctx, 'sky-sun', 96, 35, 158, 158, t * .16);
    art(ctx, 'sky-moon', 1538, 9 + Math.sin(t * .9) * 6, 100, 108, Math.sin(t * .65) * .09);
  }

  // Chapter tabs. The incoming tab lifts briefly during a turn; hovering raises one.
  function drawTabs(ctx, t) {
    tabs.forEach(([label, x, w], i) => {
      const active = i === (transition ? transition.target : state.chapter);
      const tp = motionProgress(tabMotion, motionTiming.tab, t), lift = tabMotion?.target === i ? Math.sin(tp * Math.PI) * 11 : 0;
      ctx.save();
      ctx.translate(x + w / 2, 0);
      ctx.scale(1 + (active ? lift * .002 : 0), 1);
      ctx.translate(-x - w / 2, -lift - (hoverTab === i && !reducedMotion() ? 3 : 0));
      shadow(ctx, '#4e31076b', 8, 3, 6);
      if (!active) panel(ctx, x, 28, w, 108, 11, '#fffef6', '#f1e7c9', '#dfca96');
      else ctx.drawImage(tabTexture(w, 104), x, 17, w, 104);
      ctx.shadowColor = 'transparent';
      ctx.font = '34px Chapters';
      ctx.fillStyle = active ? '#fff' : ink;
      ctx.textAlign = 'center';
      ctx.fillText(label, x + w / 2, 71);
      ctx.restore();
    });
  }

  function drawForeground(ctx, t) {
    const drift = Math.sin(t * .5) * 3;
    cloudInstances.slice(0, 5).forEach(([id, x, y, flip]) => clouds.paint(ctx, id, x + drift, y, flip));
    art(ctx, 'brighton', -9, 607 + Math.sin(t * 1.2) * 2, 234, 250, Math.sin(t * .9) * .025);
    art(ctx, 'twila', 1468, 628 + Math.sin(t * .9) * 3, 172, 199, Math.sin(t * .8) * .028);
    art(ctx, 'cursor-star', 1405, 785, 53, 54, -.13);
    cloudInstances.slice(5).forEach(([id, x, y, flip]) => clouds.paint(ctx, id, x + drift, y, flip));
  }

  function drawFooter(ctx) {
    ctx.save();
    shadow(ctx, '#05044266', 5, 0, 3);
    panel(ctx, ...layout.footer, 28, '#fffef9', '#f7f5ec', '#0c115d');
    ctx.restore();
    button(ctx, 'A', 524, 881, 'round');
    footerText(ctx, 'Choose', 584, 914);
    button(ctx, 'B', 727, 881, 'round');
    footerText(ctx, 'Back', 786, 914);
    button(ctx, 'L', 904, 885, 'shoulder');
    text(ctx, '/', 975, 919, 35, ink, 'center');
    button(ctx, 'R', 989, 885, 'shoulder');
    footerText(ctx, 'Chapters', 1059, 914);
  }

  function draw(t = state.time) {
    const ctx = stage;
    ctx.setTransform(resolution, 0, 0, resolution, 0, 0);
    ctx.clearRect(0, 0, W, H);
    drawBackground(ctx, t);
    drawTabs(ctx, t);

    const turning = transition || state.turnStart !== null;
    ctx.save();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(turning ? turningSurfaces().restPages : book.baseRestPages, 0, 0);
    if (!turning) ctx.drawImage(interactionLayer(t), 0, 0);
    ctx.restore();
    drawCursor(ctx, t, transition ? transition.stationarySelected : state.selected);

    if (state.turnStart !== null) {
      // A brisk 640ms sweep: fixed hinge, clean lift and a short soft landing.
      state.pageProgress = clamp((t - state.turnStart) / motionTiming.chapter, 0, 1);
      drawTurningSheet(ctx, transition?.direction < 0 ? 1 - ease(state.pageProgress) : ease(state.pageProgress), t);
      if (state.pageProgress === 1) {
        if (transition) {
          finishChapterTurn();
          draw(t);
          return;
        }
        state.turnStart = null;
        restorePause();
      }
    }
    drawForeground(ctx, t);
    drawFooter(ctx);
    state.frames++;
  }

  // Menu actions -------------------------------------------------------------------

  function select(index) {
    if (transition || state.turnStart !== null) return;
    const next = clamp(index, 0, rows.length - 1);
    if (state.selected === next) return;
    const oldOffset = rowOffset(), from = rowPosition(), previousHelp = rows[state.selected].help;
    state.selected = next;
    selections[state.chapter] = next;
    valueMotion = null;
    const animate = state.playing && !reducedMotion();
    selectionMotion = animate && oldOffset === rowOffset() ? {from, start: state.time} : null;
    descriptionMotion = animate ? {from: previousHelp, start: state.time} : null;
    if (oldOffset !== rowOffset()) rebuildBook(); // the page number is static print
    else bookStale = true;
    interaction.key = '';
    needsDraw = true;
    announce();
  }

  function adjust(direction) {
    if (transition) return;
    if (!menu.step(rows[state.selected], direction)) return;
    valueMotion = state.playing && !reducedMotion() ? {direction, start: state.time} : null;
    bookStale = true;
    interaction.key = '';
    needsDraw = true;
    announce();
  }

  function activateChapter(index) {
    state.chapter = index;
    state.selected = selections[index];
    rows = chapters[index].rows;
  }

  function changeChapter(index, direction = null, animate = true) {
    if (transition) return false;
    const target = (index + chapters.length) % chapters.length;
    if (target === state.chapter) return false;
    settleInteractions();
    if (bookStale) rebuildBook();
    const from = state.chapter, outgoing = book, oldSelected = state.selected;
    const forward = direction ?? (target > from ? 1 : -1);
    activateChapter(target);
    rebuildBook();
    const destination = book, newSelected = state.selected;
    if (!animate || reducedMotion()) {
      tabMotion = null;
      state.turnStart = null;
      updateChapterLabel();
      draw();
      return true;
    }
    // Turn with the outgoing chapter still current. Turning forward lifts the
    // outgoing right page, whose back shows the incoming left page; turning back
    // is the mirror image.
    activateChapter(from);
    transition = {
      target, destination, direction: forward,
      stationarySelected: forward > 0 ? oldSelected : newSelected,
      backSelected: forward > 0 ? newSelected : oldSelected,
      surfaces: forward > 0 ? {
        restPages: composeSpread([outgoing.pages[0], destination.pages[1]], destination.turnPage, outgoing.turnBackBase),
        turnPage: outgoing.turnPage, turnBackBase: destination.turnBackBase,
      } : {
        restPages: composeSpread([destination.pages[0], outgoing.pages[1]], outgoing.turnPage, destination.turnBackBase),
        turnPage: destination.turnPage, turnBackBase: outgoing.turnBackBase,
      },
    };
    startTurnMotion();
    tabMotion = {target, start: state.time};
    state.turnStart = state.time;
    state.pageProgress = 0;
    return true;
  }

  function finishChapterTurn() {
    if (!transition) return;
    activateChapter(transition.target);
    book = transition.destination;
    interaction.key = '';
    transition = null;
    state.turnStart = null;
    state.pageProgress = 1;
    updateChapterLabel();
    restorePause();
  }

  // Replays the current page's turn.
  function turn() {
    if (transition) return;
    settleInteractions();
    if (bookStale) rebuildBook();
    startTurnMotion();
    state.turnStart = state.time;
    state.pageProgress = 0;
  }

  function play(value) {
    state.playing = value;
    lastTime = performance.now();
    updateMotionButton();
  }
  // The label alone carries the state; aria-pressed on a relabelled toggle reads
  // as a contradiction.
  function updateMotionButton() {
    motionButton.textContent = state.playing ? 'Pause animation' : 'Play animation';
  }
  // A turn needs running time, so it plays even when paused; the pause returns as
  // soon as the sheet lands rather than silently staying switched off.
  function startTurnMotion() {
    if (!state.playing) pauseAfterTurn = true;
    play(true);
  }
  function restorePause() {
    if (pauseAfterTurn) {
      pauseAfterTurn = false;
      play(false);
    }
  }
  function togglePlaying() {
    // Pausing mid-turn would freeze the sheet and block all input, so land it first.
    if (state.playing && (transition || state.turnStart !== null)) {
      finishChapterTurn();
      state.turnStart = null;
      pauseAfterTurn = false;
    }
    play(!state.playing);
  }

  function updateChapterLabel() {
    canvas.setAttribute('aria-label', 'Animated storybook design preview. ' + chapters[state.chapter].name
      + ' chapter. Up and down select. Left and right adjust. Q and E change chapters. No game settings are changed.');
    announce();
  }
  // The canvas exposes no structure, so the selected row is spoken through a live region.
  function announce() {
    const row = rows[state.selected];
    liveRegion.textContent = chapters[state.chapter].name + ' chapter. ' + row.label
      + (row.value ? ': ' + menu.displayValue(row) : '') + (menu.isDisabled(row) ? ', unavailable' : '') + '. ' + row.help;
  }

  // Input ----------------------------------------------------------------------------

  const nextChapter = () => changeChapter(state.chapter + 1, 1);
  const previousChapter = () => changeChapter(state.chapter - 1, -1);
  const firstChapter = () => changeChapter(0, -1);
  const keyCommands = {
    ArrowDown: () => select((state.selected + 1) % rows.length),
    ArrowUp: () => select((state.selected + rows.length - 1) % rows.length),
    ArrowRight: () => adjust(1),
    Enter: () => adjust(1),
    ArrowLeft: () => adjust(-1),
    e: nextChapter, PageDown: nextChapter,
    q: previousChapter, PageUp: previousChapter,
    1: () => changeChapter(0), 2: () => changeChapter(1), 3: () => changeChapter(2), 4: () => changeChapter(3),
    Escape: firstChapter,
  };
  const footerActions = {choose: () => adjust(1), back: firstChapter, previous: previousChapter, next: nextChapter};

  canvas.addEventListener('keydown', event => {
    if (event.ctrlKey || event.metaKey || event.altKey) return; // leave browser shortcuts such as Ctrl+E alone
    const command = keyCommands[event.key.length === 1 ? event.key.toLowerCase() : event.key];
    if (!command) return;
    command();
    event.preventDefault();
  });

  function toDesign(event) {
    const bounds = canvas.getBoundingClientRect();
    return [(event.clientX - bounds.left) * W / bounds.width, (event.clientY - bounds.top) * H / bounds.height];
  }
  function tabAt(x, y) {
    return y < hitZones.tabsBottom ? tabs.findIndex(([, left, width]) => x >= left && x <= left + width) : -1;
  }
  // null outside the footer; 'none' between its buttons, so that click is still consumed.
  function footerControl(x, y) {
    if (y < layout.footer[1] || y > layout.footer[1] + layout.footer[3]) return null;
    return Object.keys(hitZones.footer).find(name => x >= hitZones.footer[name][0] && x < hitZones.footer[name][1]) ?? 'none';
  }
  // -1 or 1 over the page-number arrows, otherwise 0.
  function pagerStep(x, y) {
    const zone = hitZones.pager;
    if (rows.length <= visibleRowCount || x < zone.left || x > zone.right) return 0;
    const localY = unprojectPoint(0, x, y)[1];
    return localY >= zone.top && localY <= zone.bottom ? (x < zone.split ? -1 : 1) : 0;
  }
  function rowAtPoint(x, y) {
    if (x <= hitZones.rows.left || x >= hitZones.rows.right) return -1;
    const localY = unprojectPoint(0, x, y)[1], top = layout.ribbon[1], pitch = layout.rows.pitch;
    const shown = Math.min(visibleRowCount, rows.length - rowOffset());
    return localY >= top && localY < top + shown * pitch ? rowOffset() + Math.floor((localY - top) / pitch) : -1;
  }

  canvas.addEventListener('click', event => {
    const [x, y] = toDesign(event);
    if (transition) return;
    const tab = tabAt(x, y);
    if (tab >= 0) {
      changeChapter(tab);
      return;
    }
    const control = footerControl(x, y);
    if (control) {
      footerActions[control]?.();
      return;
    }
    const pageStep = pagerStep(x, y);
    if (pageStep) {
      const count = Math.ceil(rows.length / visibleRowCount), page = (Math.floor(state.selected / visibleRowCount) + pageStep + count) % count;
      select(page * visibleRowCount);
      return;
    }
    const index = rowAtPoint(x, y);
    if (index >= 0) {
      select(index);
      if (x > hitZones.rows.valueFrom) adjust(x < hitZones.rows.decreaseUntil ? -1 : 1); // the value's arrows
    }
  });
  canvas.addEventListener('pointermove', event => {
    const [x, y] = toDesign(event), tab = tabAt(x, y);
    if (tab !== hoverTab) {
      hoverTab = tab;
      needsDraw = true;
    }
    const control = footerControl(x, y);
    const clickable = hoverTab >= 0 || rowAtPoint(x, y) >= 0 || (control && control !== 'none') || pagerStep(x, y) !== 0;
    canvas.style.cursor = clickable ? 'pointer' : 'default';
  });
  canvas.addEventListener('pointerleave', () => {
    if (hoverTab >= 0) {
      hoverTab = -1;
      needsDraw = true;
    }
  });

  document.querySelector('#assembled').onclick = () => {
    document.body.classList.remove('gallery');
    resize();
  };
  document.querySelector('#pieces').onclick = () => document.body.classList.add('gallery');
  motionButton.onclick = togglePlaying;
  document.querySelector('#turn').onclick = turn;
  document.addEventListener('visibilitychange', () => { lastTime = performance.now(); });
  window.addEventListener('resize', resize);

  // Component gallery --------------------------------------------------------------

  const gallery = document.querySelector('#gallery');
  function addGalleryCard(tile, title, note) {
    const card = document.createElement('section'), heading = document.createElement('h3'), caption = document.createElement('small');
    card.className = 'asset';
    heading.textContent = title;
    caption.textContent = note;
    card.append(tile, heading, caption);
    gallery.append(card);
  }
  for (const [id, asset] of Object.entries(assets)) {
    if (clouds.shapes[id]) continue; // the raster originals are reference-only
    const tile = createCanvas(420, 280), [sx, sy, w, h] = asset.content, s = Math.min(396 / w, 252 / h);
    tile.getContext('2d').drawImage(images[id], sx, sy, w, h, (420 - w * s) / 2, (280 - h * s) / 2, w * s, h * s);
    addGalleryCard(tile, id, asset.size.join(' × ') + (id === 'book-half' ? ' · reused by mirroring' : ''));
  }
  for (const [id, shape] of Object.entries(clouds.shapes)) {
    const tile = createCanvas(420, 280);
    clouds.paint(tile.getContext('2d'), id, (420 - shape.width) / 2, (280 - shape.height) / 2);
    addGalleryCard(tile, id, 'Native curve · 2.4-unit contour · no texture scaling');
  }

  // Sizing and the frame loop -------------------------------------------------------

  function resize() {
    if (!canvas.clientWidth) return; // hidden behind the gallery
    resolution = canvas.clientWidth / W * devicePixelRatio;
    if (transition) finishChapterTurn();
    canvas.width = Math.round(W * resolution);
    canvas.height = Math.round(H * resolution);
    leafSize = 2 ** Math.ceil(Math.log2(layout.book.half * Math.max(1.5, resolution)));
    leafResolution = leafSize / layout.book.half;
    rebuildBook();
    cursor.key = '';
  }

  function frame(now) {
    const delta = Math.min((now - lastTime) / 1000, .1);
    lastTime = now;
    if (state.playing && !document.hidden) state.time += delta;
    // A paused frame only changes through input, so redraw only when asked.
    if (!document.body.classList.contains('gallery') && (state.playing || needsDraw)) {
      needsDraw = false;
      draw();
    }
    requestAnimationFrame(frame);
  }

  resize();
  updateMotionButton();
  updateChapterLabel();
  requestAnimationFrame(frame);

  // Test and export hooks. gameWiring stays false: nothing here reaches the game.
  window.proof = {
    ready: true, gameWiring: false, manifest, layout, state, chapters, tabs, motionTiming, backdrop, confetti,
    draw, turn, select, play, changeChapter, adjust, settleInteractions, refresh: rebuildBook,
    pagePoint, surfacePoint, unprojectPoint, rowAtPoint, rowPosition, leafMesh, photoBounds,
    photoSurfacePoint: (x, y) => surfacePoint(1, x, y),
    bindingPoint: () => pagePoint(1, layout.book.spine, layout.book.y + pageBand(1, layout.book.spine, paper.sheetProfile).bottom),
    textureSize: () => leafSize,
    cloudInstances, cloudShapes: clouds.shapes, cloudStyle: clouds.style, paintCloud: clouds.paint,
    drawSunbeams: (t, ctx = stage) => drawSunbeams(ctx, t),
    tabStyle, tabTexture, pageBorderStyle, pageBorderPoints,
    // Jumps to a time, paused. With pageProgress, shows a turn at that progress.
    seek(time, pageProgress = null) {
      if (pageProgress !== null && state.turnStart === null && !transition) {
        settleInteractions();
        rebuildBook();
      }
      if (pageProgress === null && transition) finishChapterTurn(); // otherwise the turn is orphaned
      play(false);
      state.time = time;
      state.turnStart = pageProgress === null ? null : time - pageProgress * motionTiming.chapter;
      draw(time);
    },
    debugBuffers: () => ({...book, leafResolution, sheetProfile: paper.sheetProfile}),
    // Blank paper and native shapes for export-native.cjs: paper ornaments only, no menu.
    nativeExport() {
      const result = {profile: paper.profile, sheet: paper.sheetProfile, assets: {}, images: {}};
      for (const [id, asset] of Object.entries(assets)) if (!referenceOnly.has(id)) result.assets[id] = asset;
      const left = printedLeaf(0, 'paper'), right = printedLeaf(1, 'paper');
      result.images['book-left'] = left.toDataURL();
      result.images['book-right'] = right.toDataURL();
      result.images['paper-right'] = turnFace(right).toDataURL();
      // turnFace clips in right-page coordinates. Restore the left orientation
      // afterwards: mirroring the right ornament is not the left-page print.
      result.images['paper-left'] = mirrored(turnFace(mirrored(left))).toDataURL();
      for (const [id, shape] of Object.entries(clouds.shapes)) {
        const out = createCanvas(shape.width + 12, shape.height + 12);
        clouds.paint(out.getContext('2d'), id, 6, 6);
        result.images[id] = out.toDataURL();
      }
      result.leafSize = leafSize;
      result.leafResolution = leafResolution;
      return result;
    },
  };
})().catch(error => {
  console.error(error);
  document.querySelector('p').textContent = 'Preview failed to load: ' + error.message;
});
