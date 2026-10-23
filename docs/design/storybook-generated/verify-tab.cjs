// Focused rendered checks for the selected chapter inset and text colors.
async page => {
  await page.waitForFunction(()=>window.proof?.ready);
  const details=await page.evaluate(()=>{
    const p=window.proof,tile=p.tabTexture(227,104),ctx=tile.getContext('2d');
    const pixels=ctx.getImageData(0,0,tile.width,tile.height).data;
    const inset=(x,y,dx,dy,scale)=>{
      for(let i=0;i<100;i++){
        const index=((y+i*dy)*tile.width+x+i*dx)*4;
        if(pixels[index]>140&&pixels[index+1]<110&&pixels[index+2]<110&&pixels[index+3]>230)return i/scale;
      }
      return null;
    };
    const padding={left:inset(0,128,1,0,512/227),right:inset(511,128,-1,0,512/227),top:inset(256,0,0,1,256/104)};
    const texts=[],original=CanvasRenderingContext2D.prototype.fillText;
    CanvasRenderingContext2D.prototype.fillText=function(...args){texts.push([args[0],this.fillStyle,this.font,args[1],this.textAlign]);return original.apply(this,args);};
    try{p.select(1);p.select(0);p.refresh();p.seek(0);}finally{CanvasRenderingContext2D.prototype.fillText=original;}
    const emitter=window.createConfettiEmitter({width:1672,height:941,seed:12345});
    const initial=emitter.at(0),later=emitter.at(1),late=emitter.at(30),veryLate=emitter.at(10000);
    const different=window.createConfettiEmitter({width:1672,height:941,seed:54321}).at(0);
    const all=[initial,later,late,veryLate];
    const trajectories=Array.from({length:120},(_,i)=>emitter.at(i*.25));
    let downward=true,birthsAbove=true,retiredNeverReturn=true;const prior=new Map(),retired=new Set();
    for(const particles of trajectories){
      const current=new Set(particles.map(p=>p.id));
      for(const id of prior.keys())if(!current.has(id))retired.add(id);
      for(const particle of particles){
        if(retired.has(particle.id))retiredNeverReturn=false;
        if(prior.has(particle.id)&&particle.y<=prior.get(particle.id))downward=false;
        if(!particle.initial&&particle.y-particle.speed*particle.age>=-particle.size)birthsAbove=false;
        prior.set(particle.id,particle.y);
      }
    }
    const [cx,cy,cw,ch]=p.layout.cursor,lastY=cy+5*p.layout.rows.pitch+ch+4;
    const starBottom=Math.max(...[cx,cx+cw].map(x=>p.surfacePoint(0,x,lastY)[1]));
    const contour=Array.from({length:65},(_,i)=>{const [x,y,w]=p.photoBounds();return p.photoSurfacePoint(x+i/64*w,y)[1];});
    return {padding,texts:[...new Map(texts.map(t=>[t[0],t])).values()],
      confetti:{initialCount:initial.length,spread:Math.max(...initial.map(p=>p.y))-Math.min(...initial.map(p=>p.y)),
        randomized:JSON.stringify(initial)!==JSON.stringify(different),repeatable:JSON.stringify(initial)===JSON.stringify(emitter.at(0)),
        oldGone:late.every(p=>!p.initial),bounded:all.every(p=>p.length<70),downward,birthsAbove,retiredNeverReturn},
      starBottom,maxPhotoBend:Math.max(...contour.slice(1,-1).map((v,i)=>Math.abs(contour[i]+contour[i+2]-2*v)))};
  });
  const checks=[],assert=(ok,message)=>{if(!ok)throw Error(message+' '+JSON.stringify(details));checks.push(message);};
  const padding=Object.values(details.padding);
  assert(padding.every(p=>p>=8),'Selected tab has increased edge padding');
  assert(Math.max(...padding)-Math.min(...padding)<1,'Top and side insets match within one logical pixel');
  const text=new Map(details.texts.map(t=>[t[0],t]));
  assert(!text.has('Make the adventure shine.')&&!text.has('Your adventure'),'Both page headings removed');
  assert(details.texts.every(t=>t[2].includes('Chapters')),'All menu lettering uses the bubbly face');
  const labels=['Widescreen','Anti-Aliasing','Ambient Occlusion','Shadow Quality','Unlocked FPS','Picture Preset'].map(label=>text.get(label));
  assert(labels.every(label=>label[3]===183&&label[4]==='left'),'All six setting labels share one left-aligned anchor');
  assert(text.get('Preview only · game settings are unchanged.')[1]==='#000000'&&text.get('Game preview')[1]==='#000000','Notes and captions are black');
  const descriptions='Choose a starting point, then adjust settings to taste. Fill wide screens without stretching the picture.';
  assert(details.texts.filter(t=>t[1]==='#0900ed').every(t=>descriptions.includes(t[0])),'Only descriptions use blue');
  assert(details.starBottom<599,'Bottom-row selection star clears the animated sun');
  const p=details.confetti;
  assert(p.initialCount===24&&p.spread>650&&p.randomized,'Initial confetti is randomized and scattered across screen height');
  assert(p.birthsAbove&&p.downward,'New confetti is born above the screen and falls monotonically downward');
  assert(p.oldGone&&p.retiredNeverReturn,'Retired confetti never wraps or reappears');
  assert(p.bounded&&p.repeatable,'Confetti stays bounded and deterministic for pause/seek');
  await page.evaluate(()=>{window.proof.select(5);window.proof.seek(0);});
  await page.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-revision-5-bottom-selection.png'});
  await page.evaluate(()=>{window.proof.select(0);window.proof.play(true);});
  return {passed:checks.length,checks,...details};
}
