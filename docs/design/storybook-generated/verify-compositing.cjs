// Exercise the reported FPS-counter arrows in the actual Canvas/WebGL pipeline.
async page=>{
 const checks=[],assert=(ok,label)=>{if(!ok)throw Error(label);checks.push(label);},results=[];
 await page.goto(new URL(page.url()).origin);await page.waitForFunction(()=>window.proof?.ready);
 for(const width of [960,1280,1920]){
  await page.setViewportSize({width,height:Math.round(width*.66)});
  await page.waitForFunction(()=>{const c=document.querySelector('#stage');return c.width===Math.round(c.clientWidth*devicePixelRatio);});
  const result=await page.evaluate(()=>{
   const p=window.proof,canvas=document.querySelector('#stage'),ctx=canvas.getContext('2d'),scale=canvas.width/1672;
   p.seek(10);p.select(7);p.settleInteractions();p.draw();
   const countArrowPixels=()=>{
    const counts=[];
    for(let row=0;row<5;row++)for(const x of [p.layout.rows.left,p.layout.rows.right]){
     const center=p.surfacePoint(0,x,p.layout.rows.y+row*p.layout.rows.pitch-14);
     const data=ctx.getImageData(Math.floor((center[0]-22)*scale),Math.floor((center[1]-22)*scale),Math.ceil(44*scale),Math.ceil(44*scale)).data;
     let gold=0;for(let i=0;i<data.length;i+=4)if(data[i]>210&&data[i+1]>110&&data[i+1]<220&&data[i+2]<85)gold++;
     counts.push(gold);
    }return counts;
   };
   const normal=countArrowPixels(),stroke=CanvasRenderingContext2D.prototype.stroke;
   // Hide only the live interaction arrows. There must be NO cached copies
   // left underneath. Before the fix, all ten static arrows survived this.
   try{
    CanvasRenderingContext2D.prototype.stroke=function(...args){if(this.lineWidth!==7)return stroke.apply(this,args);};
    p.settleInteractions();p.draw();
    const underneath=countArrowPixels();
    return {width:canvas.width,normal,underneath};
   }finally{CanvasRenderingContext2D.prototype.stroke=stroke;p.settleInteractions();p.draw();}
  });
  assert(result.normal.every(n=>n>15),'All ten arrows render at '+width+'px');
  assert(result.underneath.every(n=>n===0),'No duplicate stationary arrows under interaction layer at '+width+'px');
  results.push(result);
 }
 await page.setViewportSize({width:1280,height:830});
 await page.waitForFunction(()=>{const c=document.querySelector('#stage');return c.width===Math.round(c.clientWidth*devicePixelRatio);});
 const motion=await page.evaluate(()=>{
  const p=window.proof;p.seek(20);p.select(7);p.play(true);p.adjust(1);p.play(false);
  const stage=document.querySelector('#stage'),ctx=stage.getContext('2d'),s=stage.width/1672,centers=[];
  for(const dt of [0,.03,.06,.09,.12,.18,.24,.3]){
   p.seek(20+dt);const [x,y]=p.surfacePoint(0,p.layout.rows.right,p.layout.rows.y+p.layout.rows.pitch-14);
   const left=Math.floor((x-20)*s),top=Math.floor((y-22)*s),w=Math.ceil(48*s),h=Math.ceil(44*s),data=ctx.getImageData(left,top,w,h).data;
   let weight=0,total=0,min=w,max=0;
   for(let i=0;i<data.length;i+=4)if(data[i]>210&&data[i+1]>110&&data[i+1]<220&&data[i+2]<85){const px=i/4%w;weight++;total+=px;min=Math.min(min,px);max=Math.max(max,px);}
   centers.push({dt,x:(left+total/weight)/s,span:(max-min+1)/s,pixels:weight});
  }return centers;
 });
 assert(motion.every(m=>m.pixels>15&&m.span<22),'Arrow kick stays a single silhouette throughout value animation');
 assert(motion[4].x-motion[0].x>5&&Math.abs(motion.at(-1).x-motion[0].x)<1,'Arrow moves out and returns cleanly to its resting position');
 await page.evaluate(()=>{const p=window.proof;p.seek(30);p.select(7);p.play(true);p.select(8);p.play(false);p.seek(30.04);});
 await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-compositing-selection.png'});
 await page.evaluate(()=>{const p=window.proof;p.settleInteractions();p.seek(32);p.select(7);p.play(true);p.adjust(1);p.play(false);p.seek(32.08);});
 await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-compositing-value.png'});
 await page.evaluate(()=>{window.proof.seek(33);window.proof.play(true);});
 return {passed:checks.length,checks,results,motion};
}
