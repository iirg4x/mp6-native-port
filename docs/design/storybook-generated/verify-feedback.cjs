// Focused browser proof for interaction motion, backdrop, icons and paper rule.
async page=>{
 const checks=[],assert=(ok,label)=>{if(!ok)throw Error(label);checks.push(label);},errors=[];
 page.on('pageerror',error=>errors.push(error.message));
 await page.goto(new URL(page.url()).origin);await page.waitForFunction(()=>window.proof?.ready);
 const canvas=page.getByLabel('Animated storybook design');
 const motion=await page.evaluate(()=>{
  const p=window.proof;p.seek(10);p.select(0);p.play(true);p.select(1);p.play(false);
  const start=p.rowPosition();p.seek(10.07);const middle=p.rowPosition();
  p.play(true);p.select(2);p.play(false);const retarget=p.rowPosition();
  p.seek(10.5);const end=p.rowPosition();
  return {start,middle,retarget,end,duration:p.motionTiming.chapter};
 });
 assert(motion.duration===.64,'Page turn shortened from 1.8 seconds to 640ms');
 assert(motion.start===0&&motion.middle>0&&motion.middle<1,'Selection starts at old row and travels through intermediate positions');
 assert(Math.abs(motion.retarget-motion.middle)<.00001,'Rapid selection retargets from the visible ribbon position without jumping');
 assert(motion.end===2,'Selection settles exactly on the requested row');
 await page.evaluate(()=>{const p=window.proof;p.seek(20);p.select(0);p.play(true);p.select(3);p.play(false);p.seek(20.07);});
 await canvas.screenshot({path:'output/playwright/storybook-motion-selection.png'});
 const value=await page.evaluate(()=>{
  const p=window.proof,stage=document.querySelector('#stage'),ctx=stage.getContext('2d'),s=stage.width/1672;
  p.settleInteractions();p.seek(25);p.select(1);p.play(true);p.adjust(1);p.play(false);p.seek(25.08);
  const before=ctx.getImageData(620*s,170*s,100*s,80*s).data,current=p.chapters[0].rows[1].value;
  p.seek(25.5);const after=ctx.getImageData(620*s,170*s,100*s,80*s).data;
  let changed=0;for(let i=0;i<before.length;i++)if(before[i]!==after[i])changed++;
  return {changed,current,final:p.chapters[0].rows[1].value};
 });
 assert(value.changed>100&&value.current===value.final,'Changed values animate their rendered shape without changing the chosen value again');
 await page.evaluate(()=>{const p=window.proof;p.settleInteractions();p.seek(30);p.select(0);});
 const started=Date.now();await canvas.press('e');
 await page.waitForFunction(()=>window.proof.state.turnStart===null);
 const elapsed=Date.now()-started;
 assert(elapsed<1400,'Real chapter input completes promptly ('+elapsed+'ms including setup)');
 assert(await page.evaluate(()=>window.proof.state.chapter===1),'Fast chapter turn commits Sound');
 await page.evaluate(()=>window.proof.seek(32));
 await canvas.screenshot({path:'output/playwright/storybook-latest-sound.png'});
 await page.evaluate(()=>{const p=window.proof;p.changeChapter(2,null,false);p.seek(32);});
 await canvas.screenshot({path:'output/playwright/storybook-latest-game.png'});
 const visual=await page.evaluate(()=>{
  const p=window.proof,l=p.pageBorderPoints(0),r=p.pageBorderPoints(1),color=p.backdrop.palette;
  const luminance=hex=>{const values=hex.slice(1).match(/../g).map(v=>parseInt(v,16));return values[0]*.2126+values[1]*.7152+values[2]*.0722;};
  return {mirrored:l.every((v,i)=>Math.abs(v[0]+r[i][0]-1672)<.001&&v[1]===r[i][1]),
   finite:[...l,...r].every(p=>p.every(Number.isFinite)),inside:l.every(v=>v[0]>70&&v[0]<820&&v[1]>85&&v[1]<830),
   day:luminance(color.dayTop)>luminance(color.dayBottom),night:luminance(color.nightTop)>luminance(color.nightBottom),
   balloons:p.backdrop.balloons.length,stars:p.backdrop.distantStars.length,
   icons:['chapter-sound','chapter-game'].every(id=>p.manifest.assets[id]?.size.join('x')==='512x512')};
 });
 assert(visual.mirrored&&visual.finite&&visual.inside,'Soft paper frames are mirrored, bounded and remain inset from the fold');
 assert(visual.day&&visual.night,'Both sky gradients are darker below and lighter above');
 assert(visual.balloons>=3&&visual.stars>=200,'Day balloons and a dense distant night-sky star field are present');
 const skyMotion=await page.evaluate(()=>{
  const sky=window.proof.backdrop,a=sky.balloonPositions(0),b=sky.balloonPositions(1),c=document.createElement('canvas');c.width=1672;c.height=941;
  const ctx=c.getContext('2d');sky.nightDetails(ctx,0);const first=ctx.getImageData(0,0,c.width,c.height).data;
  ctx.clearRect(0,0,c.width,c.height);sky.nightDetails(ctx,1);const second=ctx.getImageData(0,0,c.width,c.height).data;
  let changed=0;for(let i=0;i<first.length;i+=4)if(Math.abs(first[i]-second[i])>8)changed++;
  return {rising:a.every((p,i)=>b[i].y<p.y-7),swaying:a.some((p,i)=>Math.abs(b[i].x-p.x)>.5),starPixels:changed};
 });
 assert(skyMotion.rising&&skyMotion.swaying,'Balloons visibly rise and sway instead of staying in place');
 assert(skyMotion.starPixels>300,'Night stars visibly animate brightness and glow');
 assert(visual.icons,'Both generated chapter icons are packed as project-local 512px assets');
 const padding=await page.evaluate(async()=>{
  const p=window.proof,b=p.layout.book,image=new Image();image.src=p.manifest.assets['book-half'].file;await image.decode();
  const c=document.createElement('canvas');c.width=b.half;c.height=b.h;const ctx=c.getContext('2d'),[sx,sy,sw,sh]=p.manifest.assets['book-half'].content;
  for(let y=0;y<b.h;y++){
   const shift=-70*(1-y/b.h);ctx.save();ctx.beginPath();ctx.rect(0,y,b.half,1);ctx.clip();
   ctx.drawImage(image,sx,sy,sw,sh,shift,0,b.half-shift,b.h);ctx.restore();
  }
  const data=ctx.getImageData(0,0,c.width,c.height).data,frame=p.pageBorderPoints(0),a=frame[0],z=frame.at(-1),gaps=[];
  for(const y of [160,240,320,400,480,560,640]){
   // Independent verification of the actual material transition: the fine
   // lower leaves contain gold lines, while the top paper stays pale inside.
   let edge=-1;
   for(let x=40;x<140;x++){
    let pale=true;for(let dx=0;dx<16;dx++){const i=(y*c.width+x+dx)*4;if(data[i+1]<235||data[i+2]<232){pale=false;break;}}
    if(pale){edge=x;break;}
   }
   const borderX=a[0]+(z[0]-a[0])*(b.y+y-a[1])/(z[1]-a[1]);
   gaps.push(borderX-b.x-edge);
  }
  return {gaps,spread:Math.max(...gaps)-Math.min(...gaps)};
 });
 assert(padding.gaps.every(v=>v>24&&v<30)&&padding.spread<3,'Actual top-paper edge keeps a uniform 27-unit side inset: '+JSON.stringify(padding));
 await page.emulateMedia({reducedMotion:'reduce'});
 await page.evaluate(()=>{const p=window.proof;p.changeChapter(0);p.select(4);});
 assert(await page.evaluate(()=>window.proof.state.turnStart===null&&window.proof.rowPosition()===4),'Reduced motion makes chapter and row changes immediate');
 await page.emulateMedia({reducedMotion:'no-preference'});
 await page.evaluate(()=>{const p=window.proof;p.select(0);p.seek(32);});
 await canvas.screenshot({path:'output/playwright/storybook-latest-picture.png'});
 assert(errors.length===0,'No browser errors');
 await page.evaluate(()=>window.proof.play(true));
 return {passed:checks.length,checks,motion,value,visual,skyMotion,padding,elapsed,errors};
}
