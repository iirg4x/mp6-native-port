async page=>{
 await page.goto(new URL(page.url()).origin);await page.waitForFunction(()=>window.proof?.ready);
 await page.setViewportSize({width:1280,height:830});
 await page.waitForFunction(()=>{const c=document.querySelector('#stage');return c.width===Math.round(c.clientWidth*devicePixelRatio);});
 const data=await page.evaluate(()=>{
  const p=window.proof;p.seek(1);const {turnBackBase:b,turnPage:f,leafResolution:s}=p.debugBuffers();
  const ctx=b.getContext('2d'),front=f.getContext('2d'),rows=[];
  for(const y of [80,100,125,150,175,200,250,300,400,600]){
   rows.push({y,alpha:Array.from({length:16},(_,i)=>ctx.getImageData(Math.round(i*5*s),Math.round(y*s),1,1).data[3]),
    front:Array.from({length:16},(_,i)=>front.getImageData(Math.round(i*5*s),Math.round(y*s),1,1).data[3])});
  }
  return rows;
 });
 for(const progress of [.001,.05,.2,.5,.7,.8,.9,.95,.98,.999]){
  await page.evaluate(t=>window.proof.seek(1,t),progress);
  await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/occlusion-'+progress+'.png'});
 }
 return data;
}
