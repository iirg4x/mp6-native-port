async page=>{
 await page.waitForFunction(()=>window.proof?.ready);
 const data=await page.evaluate(()=>{
  const p=window.proof,fold=p.bindingPoint(),canvas=document.querySelector('#stage'),ctx=canvas.getContext('2d'),scale=canvas.width/1672;
  const endpoints=[0,.15,.4,.6,.85,1].map(t=>{
   const hinge=p.leafMesh(t)[0];return [hinge.top[0],hinge.top[1]+(hinge.bottom[1]-hinge.top[1])*p.layout.book.paperHingeV];
  });
  const sample=()=>Array.from(ctx.getImageData(Math.round((fold[0]-1)*scale),Math.round(851*scale),Math.max(1,Math.round(2*scale)),Math.round(12*scale)).data);
  p.seek(1);const baseline=sample();let maxDelta=0;
  for(const t of [.15,.4,.6,.85]){
   p.seek(1,t);const frame=sample();frame.forEach((v,i)=>maxDelta=Math.max(maxDelta,Math.abs(v-baseline[i])));
  }
  return {fold,endpoints,maxDelta};
 });
 const checks=[],assert=(ok,label)=>{if(!ok)throw Error(label+' '+JSON.stringify(data));checks.push(label);};
 assert(data.fold[1]>827&&data.fold[1]<828,'Moving sheet reaches the newly generated top-sheet fold, above the dense page stack');
 assert(data.endpoints.every(p=>Math.hypot(p[0]-data.fold[0],p[1]-data.fold[1])<.0001),'Sheet endpoint stays attached to the same fold through the turn');
 // Cached and live GPU compositing can round the translucent cover fringe by
 // one 8-bit channel step. Any visible movement/shadow change exceeds this.
 assert(data.maxDelta<=1,'Binding stays stable within one 8-bit compositing roundoff step');
 for(const [progress,name] of [[0,'rest'],[.4,'front'],[.7,'back'],[1,'landed']]){
  await page.evaluate(progress=>window.proof.seek(1,progress),progress);
  await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-binding-'+name+'.png'});
 }
 await page.evaluate(()=>window.proof.play(true));
 return {passed:checks.length,checks,...data};
}
