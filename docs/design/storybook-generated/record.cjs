// Run after playwright-cli video-start. Records actual browser animation, not an imagegen movie.
async page => {
  await page.evaluate(()=>{window.proof.select(0);window.proof.seek(0);window.proof.play(true);});
  await page.waitForTimeout(2000);
  await page.evaluate(()=>window.proof.turn());
  await page.waitForTimeout(2200);
  await page.evaluate(()=>window.proof.select(1));
  await page.waitForTimeout(700);
  await page.evaluate(()=>window.proof.select(0));
  await page.waitForTimeout(1400);
}
