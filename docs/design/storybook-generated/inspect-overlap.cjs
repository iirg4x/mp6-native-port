async page=>{
 await page.goto(new URL(page.url()).origin);await page.waitForFunction(()=>window.proof?.ready);
 await page.setViewportSize({width:1280,height:830});
 await page.evaluate(()=>{const p=window.proof;p.seek(10);p.select(7);p.adjust(1);p.settleInteractions();p.seek(10);});
 const canvas=page.getByLabel('Animated storybook design');
 await canvas.screenshot({path:'output/playwright/overlap-rest.png'});
 await page.evaluate(()=>{const p=window.proof;p.play(true);p.adjust(1);p.play(false);p.seek(10.08);});
 await canvas.screenshot({path:'output/playwright/overlap-adjust.png'});
 await page.evaluate(()=>{const p=window.proof;p.settleInteractions();p.seek(20);p.play(true);p.select(8);p.play(false);p.seek(20.04);});
 await canvas.screenshot({path:'output/playwright/overlap-select.png'});
 for(const progress of [.2,.45,.55,.8]){
  await page.evaluate(progress=>window.proof.seek(30,progress),progress);
  await canvas.screenshot({path:'output/playwright/overlap-turn-'+progress+'.png'});
 }
 await page.evaluate(()=>window.proof.seek(30));
 return {selected:await page.evaluate(()=>window.proof.state.selected)};
}
