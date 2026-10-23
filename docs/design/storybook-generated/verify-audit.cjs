// Invoke with playwright-cli run-code --filename=docs/design/storybook-generated/verify-audit.cjs
// Regression checks for the 2026-10-02 audit fixes (see README).
async page => {
  const result = [];
  const assert = (condition, message) => { if (!condition) throw Error(message); result.push(message); };
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const origin = new URL(page.url()).origin;
  const ready = () => page.waitForFunction(() => window.proof?.ready);
  const canvas = page.locator('#stage');

  await page.setViewportSize({width: 1280, height: 800});
  await page.goto(origin + '/'); await ready();
  const size = await page.evaluate(() => ({sh: document.documentElement.scrollHeight, vh: innerHeight}));
  assert(size.sh <= size.vh, `1280x800 has no vertical scroll (${size.sh} <= ${size.vh})`);

  // Pause during a chapter turn lands the turn and keeps input working.
  await page.evaluate(() => window.proof.changeChapter(1, 1, true));
  await page.waitForTimeout(150);
  await page.getByRole('button', {name: 'Pause animation', exact: true}).click();
  await page.waitForTimeout(300);
  const paused = await page.evaluate(() => { const p = window.proof; p.select(1); return {chapter: p.state.chapter, selected: p.state.selected, playing: p.state.playing, turning: p.state.turnStart !== null}; });
  assert(paused.chapter === 1 && !paused.turning, 'Pausing mid-turn lands on the target chapter');
  assert(!paused.playing && paused.selected === 1, 'Input still works after pausing mid-turn');

  // A chapter turn while paused animates, then returns to paused.
  await page.evaluate(() => window.proof.changeChapter(2, 1, true));
  assert(await page.evaluate(() => window.proof.state.playing), 'Chapter turn plays while paused');
  await page.waitForFunction(() => window.proof.state.chapter === 2 && window.proof.state.turnStart === null);
  await page.waitForTimeout(50);
  assert(await page.evaluate(() => !window.proof.state.playing), 'Pause is restored after the turn lands');
  await page.getByRole('button', {name: 'Replay page turn', exact: true}).click();
  await page.waitForFunction(() => window.proof.state.turnStart === null);
  await page.waitForTimeout(50);
  assert(await page.evaluate(() => !window.proof.state.playing), 'Pause is restored after a replayed turn');
  // Playing stays playing.
  await page.getByRole('button', {name: 'Play animation', exact: true}).click();
  await page.evaluate(() => window.proof.changeChapter(3, 1, true));
  await page.waitForFunction(() => window.proof.state.chapter === 3 && window.proof.state.turnStart === null);
  assert(await page.evaluate(() => window.proof.state.playing), 'Playing animation keeps playing after a turn');

  // seek() without progress lands an in-flight turn instead of orphaning it.
  await page.evaluate(() => { const p = window.proof; p.changeChapter(0, -1, true); p.seek(5); });
  const sought = await page.evaluate(() => { const p = window.proof; const ok = p.changeChapter(1, null, false); return {ok, chapter: p.state.chapter}; });
  assert(sought.ok && sought.chapter === 1, 'seek() out of a turn leaves navigation usable');

  // Volume clamps at both ends; other rows still cycle.
  const volume = await page.evaluate(() => { const p = window.proof; p.select(0);
    const row = p.chapters[1].rows[0]; row.value = '100%'; p.adjust(1); const top = row.value;
    row.value = '0%'; p.adjust(-1); const bottom = row.value;
    p.select(1); const sfx = p.chapters[1].rows[1]; sfx.value = 'On'; p.adjust(1); return {top, bottom, sfx: sfx.value}; });
  assert(volume.top === '100%' && volume.bottom === '0%', 'Master Volume stops at 0% and 100%');
  assert(volume.sfx === 'Off', 'On/Off rows still cycle');

  // Modifier shortcuts are left to the browser.
  await canvas.focus();
  const before = await page.evaluate(() => window.proof.state.chapter);
  await canvas.press('Control+e');
  assert(await page.evaluate(() => window.proof.state.chapter) === before, 'Ctrl+E does not change chapter');
  await canvas.press('e'); await page.evaluate(() => window.proof.seek(1, 1));
  assert(await page.evaluate(() => window.proof.state.chapter) === (before + 1) % 4, 'Plain E still changes chapter');

  // Screen-reader exposure.
  await page.evaluate(() => { const p = window.proof; p.changeChapter(0, null, false); p.select(1); });
  const status = await page.locator('#status').textContent();
  assert(/Picture chapter\. Widescreen: (On|Off)\./.test(status), 'Live region announces chapter, row and value: ' + status);
  assert(await canvas.getAttribute('role') === 'application', 'Canvas has an application role');
  assert(await page.locator('#motion').getAttribute('aria-pressed') === null, 'Relabelled toggle drops aria-pressed');

  // Value changes no longer rebuild the whole book, but a following turn still prints them.
  const fast = await page.evaluate(() => { const p = window.proof; p.changeChapter(0, null, false); p.seek(5); p.select(1);
    const start = performance.now(); p.adjust(1); p.adjust(-1); p.adjust(1); return (performance.now() - start) / 3; });
  assert(fast < 30, `Value change costs ${fast.toFixed(1)} ms (was ~100-180 ms)`);
  const continuity = await page.evaluate(() => {
    const p = window.proof, stage = document.querySelector('#stage'), ctx = stage.getContext('2d'), s = stage.width / 1672;
    const region = () => ctx.getImageData(Math.round(590 * s), Math.round(220 * s), Math.round(150 * s), Math.round(70 * s)).data;
    p.seek(6); const rest = region();
    p.turn(); p.draw(p.state.time); const start = region();
    let changed = 0; for (let i = 0; i < rest.length; i += 4) if (Math.abs(rest[i] - start[i]) + Math.abs(rest[i + 1] - start[i + 1]) + Math.abs(rest[i + 2] - start[i + 2]) > 60) changed++;
    p.seek(7); return changed / (rest.length / 4);
  });
  assert(continuity < 0.01, `Turn start prints the adjusted value, not a stale page (${(continuity * 100).toFixed(2)}% changed)`);

  // Paused and idle: no redraws until something changes.
  const idle = await page.evaluate(async () => { const p = window.proof; p.seek(8); const before = p.state.frames;
    await new Promise(r => setTimeout(r, 400)); const idleFrames = p.state.frames - before;
    p.select(2); await new Promise(r => setTimeout(r, 200)); return {idleFrames, afterInput: p.state.frames - before - idleFrames}; });
  assert(idle.idleFrames === 0, 'Paused page does not redraw while idle');
  assert(idle.afterInput >= 1 && idle.afterInput <= 2, `Input while paused redraws once (${idle.afterInput})`);

  // Native "Auto (...)" labels reach the page and the live region.
  await page.evaluate(() => { const p = window.proof; p.select(9); });
  assert(/Window Size: Auto \(keep current\)/.test(await page.locator('#status').textContent()), 'Window Size shows the native "Auto (keep current)" label');

  // The selected tab's nine-slice has no see-through seams (they read as a 3x3 grid).
  const seams = await page.evaluate(() => {
    const tile = window.proof.tabTexture(245, 104), d = tile.getContext('2d').getImageData(0, 0, tile.width, tile.height).data;
    let minAlpha = 255;
    for (let x = 4; x < tile.width - 4; x++) minAlpha = Math.min(minAlpha, d[(128 * tile.width + x) * 4 + 3]);
    for (let y = 4; y < tile.height - 4; y++) minAlpha = Math.min(minAlpha, d[(y * tile.width + 256) * 4 + 3]);
    return minAlpha;
  });
  // The artwork's own fill is alpha 251-255; the seams were 194-207.
  assert(seams >= 250, `Selected tab stays solid across its slice boundaries (min alpha ${seams})`);

  // The wider value column prints formerly squeezed values at full size.
  const valueFont = await page.evaluate(() => {
    const p = window.proof, fonts = {}, original = CanvasRenderingContext2D.prototype.fillText;
    p.changeChapter(0, null, false); p.select(0);
    p.chapters[0].rows[0].value = 'Classic+'; p.refresh();
    CanvasRenderingContext2D.prototype.fillText = function(value, ...rest) { fonts[value] = this.font; return original.call(this, value, ...rest); };
    try { p.seek(9); } finally { CanvasRenderingContext2D.prototype.fillText = original; }
    return fonts['Classic+'];
  });
  assert(valueFont === '32px Chapters', `"Classic+" prints at full size (${valueFont})`);

  // Footer gaps no longer show a pointer cursor.
  const box = await canvas.boundingBox(), toPage = (x, y) => [box.x + x * box.width / 1672, box.y + y * box.height / 941];
  await page.mouse.move(...toPage(1300, 905));
  assert(await canvas.evaluate(el => el.style.cursor) === 'default', 'Footer area outside the buttons keeps the default cursor');
  await page.mouse.move(...toPage(600, 905));
  assert(await canvas.evaluate(el => el.style.cursor) === 'pointer', 'Footer Choose button shows a pointer');

  // Screenshot modes pin the confetti seed; ?seed overrides.
  await page.goto(origin + '/?still'); await ready();
  assert(await page.evaluate(() => window.proof.confetti.seed) === 1, '?still uses a fixed confetti seed');
  await page.goto(origin + '/?seed=42'); await ready();
  assert(await page.evaluate(() => window.proof.confetti.seed) === 42, '?seed=42 sets the confetti seed');

  // Short landscape phones use the full height; portrait phones get a rotate hint.
  await page.setViewportSize({width: 812, height: 375});
  await page.waitForFunction(() => Math.abs(document.querySelector('#stage').clientHeight - 375) <= 1);
  assert(await page.locator('#rotate').isHidden(), 'Rotate hint hidden in landscape');
  await page.setViewportSize({width: 375, height: 812});
  assert(await page.locator('#rotate').isVisible(), 'Rotate hint shown in portrait on a phone');

  // Gallery adapts on phones.
  await page.getByRole('button', {name: 'Component gallery', exact: true}).click();
  const columns = await page.evaluate(() => getComputedStyle(document.querySelector('#gallery')).gridTemplateColumns.split(' ').length);
  assert(columns === 1, 'Gallery uses one column at phone width');
  assert(errors.length === 0, 'No browser errors');
  return {passed: result.length, checks: result, errors};
}
