async page=>{
 await page.goto(new URL(page.url()).origin);await page.waitForFunction(()=>window.proof?.ready);
 await page.setViewportSize({width:1280,height:830});
 await page.waitForFunction(()=>{const c=document.querySelector('#stage');return c.width===Math.round(c.clientWidth*devicePixelRatio);});
 const result=await page.evaluate(()=>{
  const p=window.proof,canvas=document.querySelector('#stage'),ctx=canvas.getContext('2d'),s=canvas.width/1672;
  p.seek(1);p.select(0);p.settleInteractions();p.draw();
  const sample=()=>{
   // Precisely the annotation: where the gold inner page rule crosses the
   // red ribbon tip. The rule belongs below the ribbon in BOTH caches.
   const [x,y]=p.surfacePoint(0,790,181);
   return ctx.getImageData(Math.round((x-6)*s),Math.round((y-7)*s),Math.round(22*s),Math.round(14*s)).data;
  };
  const baseline=sample();p.seek(1,0);const turning=sample();
  let max=0,sum=0,changed=0;
  for(let i=0;i<baseline.length;i+=4){
   const d=Math.max(...[0,1,2].map(k=>Math.abs(baseline[i+k]-turning[i+k])));
   max=Math.max(max,d);sum+=d;if(d>8)changed++;
  }
  return {mean:sum/(baseline.length/4),max,changed,pixels:baseline.length/4};
 });
 await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-page-order-lift.png'});
 await page.evaluate(()=>window.proof.seek(1,.8));
 await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-page-order-landing.png'});
 if(result.mean>1||result.max>8)throw Error('Page border moves in front of ribbon during turn: '+JSON.stringify(result));
 await page.evaluate(()=>{window.proof.seek(1);window.proof.play(true);});
 return {passed:1,checks:['Page frame remains below the ribbon at animation start'],result};
}
