async page=>{
 await page.waitForFunction(()=>window.proof?.ready);
 await page.setViewportSize({width:1280,height:830});
 for(const progress of [0,.04,.08,.15,.3,.7,.95]){
  await page.evaluate(p=>window.proof.seek(1,p),progress);
  await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/edge-turn-'+progress+'.png'});
 }
 await page.evaluate(()=>window.proof.seek(1));
 return {chapter:await page.evaluate(()=>window.proof.state.chapter)};
}
