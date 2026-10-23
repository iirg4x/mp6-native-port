// Invoke with playwright-cli run-code --filename=docs/design/storybook-generated/verify.cjs
// This is a CLI verification function, not a production game test.
async page => {
  const result = [];
  const assert = (condition, message) => { if (!condition) throw Error(message); result.push(message); };
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const origin=new URL(page.url()).origin;
  await page.goto(origin+'/?revision=5');
  await page.waitForFunction(() => window.proof?.ready);
  const art = page.getByLabel('Animated storybook design');
  const time = () => page.evaluate(() => window.proof.state.time);
  const t0 = await time();
  const initialPixels=await art.evaluate(el=>el.toDataURL());
  await page.waitForTimeout(160);
  assert(await time() > t0, 'Animation plays on load');
  assert(await art.evaluate(el=>el.toDataURL())!==initialPixels,'Animation visibly changes rendered pixels');
  await page.getByRole('button', {name:'Pause animation',exact:true}).click();
  const paused = await time();
  const frozen = await art.evaluate(el => el.toDataURL());
  await page.waitForTimeout(160);
  assert(await time() === paused, 'Pause preserves time');
  assert(await art.evaluate(el => el.toDataURL()) === frozen, 'Pause preserves exact pixels');
  await page.getByRole('button', {name:'Play animation',exact:true}).click();
  await page.waitForTimeout(160);
  const resumed = await time();
  assert(resumed > paused && resumed - paused < 1, 'Resume continues without reset or time jump');
  await page.getByRole('button', {name:'Pause animation',exact:true}).click();
  const bounds = await art.boundingBox();
  const thirdRow=await page.evaluate(()=>window.proof.surfacePoint(0,300,window.proof.layout.ribbon[1]+2.5*window.proof.layout.rows.pitch));
  await art.click({position:{x:thirdRow[0]*bounds.width/1672,y:thirdRow[1]*bounds.height/941}});
  assert(await page.evaluate(() => window.proof.state.selected) === 2, 'Single click moves selection');
  await art.press('ArrowDown');
  assert(await page.evaluate(() => window.proof.state.selected) === 3, 'Keyboard selection works');
  await page.evaluate(() => { window.proof.select(0); window.proof.seek(0); });
  await page.getByRole('button', {name:'Replay page turn',exact:true}).click();
  await page.waitForTimeout(230);
  assert(await page.evaluate(() => window.proof.state.turnStart !== null), 'Page-turn button starts animation');
  await page.waitForTimeout(1950);
  assert(await page.evaluate(() => window.proof.state.turnStart === null), 'Page-turn animation finishes');
  const continuity=await page.evaluate(()=>{
    const p=window.proof,canvas=document.querySelector('#stage'),ctx=canvas.getContext('2d');
    p.seek(1);const before=ctx.getImageData(0,0,canvas.width,canvas.height).data;
    p.seek(1,.0001);const after=ctx.getImageData(0,0,canvas.width,canvas.height).data;
    let sum=0,changed=0;for(let i=0;i<before.length;i+=4){
      const delta=Math.max(Math.abs(before[i]-after[i]),Math.abs(before[i+1]-after[i+1]),Math.abs(before[i+2]-after[i+2]));
      sum+=delta;if(delta>12)changed++;
    }
    p.seek(1);return {mean:sum/(before.length/4),changedFraction:changed/(before.length/4)};
  });
  assert(continuity.mean<.5&&continuity.changedFraction<.002,'Turning sheet matches static page at lift-off: '+JSON.stringify(continuity));
  const landing=await page.evaluate(()=>{
    const p=window.proof,canvas=document.querySelector('#stage'),ctx=canvas.getContext('2d');
    p.seek(1,.9999);const before=ctx.getImageData(0,0,canvas.width,canvas.height).data;
    p.seek(1,1);const after=ctx.getImageData(0,0,canvas.width,canvas.height).data;
    let sum=0,changed=0;for(let i=0;i<before.length;i+=4){
      const delta=Math.max(Math.abs(before[i]-after[i]),Math.abs(before[i+1]-after[i+1]),Math.abs(before[i+2]-after[i+2]));
      sum+=delta;if(delta>24)changed++;
    }
    return {mean:sum/(before.length/4),changedFraction:changed/(before.length/4)};
  });
  assert(landing.mean<2&&landing.changedFraction<.025,'Turning sheet lands without a blank-page or layout pop');
  const components=await page.evaluate(()=>{
    const p=window.proof,ctx=document.createElement('canvas').getContext('2d'),strokes=[];
    const stroke=ctx.stroke.bind(ctx);ctx.stroke=(...args)=>{const m=ctx.getTransform();strokes.push([ctx.lineWidth,Math.abs(m.a),Math.abs(m.d)]);stroke(...args);};
    for(const id of Object.keys(p.cloudShapes))for(const flipped of [false,true])p.paintCloud(ctx,id,0,0,flipped);
    const source=p.manifest.assets['game-preview'].content,frame=p.photoBounds();
    const meshes=[0,.2,.5,.8,1].map(p.leafMesh);
    return {strokes,source,frame,hinges:meshes.map(m=>[m[0].top,m[0].bottom]),
      arcLengths:meshes.map(m=>m.slice(1).reduce((sum,s,i)=>sum+Math.hypot(s.x-m[i].x,s.z-m[i].z),0))};
  });
  assert(components.strokes.length===12&&components.strokes.every((s,i)=>Math.abs(s[0]-(i%2?2.4:9))<.00001&&s[1]===1&&s[2]===1),
    'All cloud silhouettes and mirrors use identical outlines with no local scaling');
  assert(Math.abs(components.frame[2]/components.frame[3]-components.source[2]/components.source[3])<.00001,
    'Photo frame preserves the full capture aspect ratio');
  assert(components.hinges.every(h=>JSON.stringify(h)===JSON.stringify(components.hinges[0])),
    'Page-turn hinge stays fixed from lift-off through landing');
  assert(components.arcLengths.every(length=>Math.abs(length-816)<.00001),'Curved sheet preserves its physical width instead of stretching');
  const motionDetails=await page.evaluate(()=>{
    const p=window.proof,flat=p.leafMesh(0),raised=p.leafMesh(.5),tile=document.createElement('canvas');
    tile.width=512;tile.height=256;const ctx=tile.getContext('2d');
    p.drawSunbeams(0,ctx);const first=ctx.getImageData(0,0,512,256).data;
    ctx.clearRect(0,0,512,256);p.drawSunbeams(1,ctx);const second=ctx.getImageData(0,0,512,256).data;
    let changed=0;for(let i=3;i<first.length;i+=4)if(first[i]!==second[i])changed++;
    return {beamChange:changed/(512*256),nearHeight:raised[48].bottom[1]-raised[48].top[1],farHeight:flat[48].bottom[1]-flat[48].top[1]};
  });
  assert(motionDetails.nearHeight>motionDetails.farHeight*1.1,'Lifted paper grows toward the camera instead of shrinking');
  assert(motionDetails.beamChange>.1,'Sunbeams visibly rotate independently within one second');
  const draws=await page.evaluate(()=>{
    const original=CanvasRenderingContext2D.prototype.drawImage,calls=[];
    CanvasRenderingContext2D.prototype.drawImage=function(...args){
      if(args[0] instanceof HTMLImageElement)calls.push([args[0].src,...args.slice(1)]);
      return original.apply(this,args);
    };
    try{window.proof.select(1);window.proof.select(0);window.proof.refresh();window.proof.seek(0);}finally{CanvasRenderingContext2D.prototype.drawImage=original;}
    return calls;
  });
  const photos=draws.filter(d=>d[0].endsWith('/assets/game-preview.png'));
  assert(photos.length>0&&photos.every(d=>JSON.stringify(d.slice(1,5))===JSON.stringify(components.source)),
    'Photo renderer samples the entire captured frame, not a center crop');
  assert(!draws.some(d=>/\/assets\/cloud(?:-round|-bank)?\.png$/.test(d[0])),
    'Preview never samples a scaled cloud bitmap');
  const hitPoints=await page.evaluate(()=>Array.from({length:6},(_,i)=>window.proof.surfacePoint(0,760,window.proof.layout.ribbon[1]+15+i*window.proof.layout.rows.pitch)));
  for(let i=0;i<hitPoints.length;i++){
    const box=await art.boundingBox(),[x,y]=hitPoints[i];
    await art.click({position:{x:x*box.width/1672,y:y*box.height/941}});
    assert(await page.evaluate(()=>window.proof.state.selected)===i,'Curved row '+(i+1)+' accepts one click');
  }
  await page.evaluate(()=>{window.proof.select(0);window.proof.seek(0);});
  const surface = await page.evaluate(() => ({
    gutter:window.proof.pagePoint(1,872,199),
    middle:window.proof.pagePoint(1,1220,199),
    photo:window.proof.layout.photo,
    contour:Array.from({length:65},(_,i)=>{
      const [x,y,w]=window.proof.photoBounds();
      return window.proof.photoSurfacePoint(x+i/64*w,y)[1];
    }),
    cloudIds:window.proof.cloudInstances.map(c=>c[0]),
    newAssets:['cloud-round','cloud-bank','bubble-star','button-round','tab-selected','button-shoulder']
      .every(id=>!!window.proof.manifest.assets[id]),
    singleButton:!!window.proof.manifest.assets['button-round']&&!window.proof.manifest.assets['button-green']&&!window.proof.manifest.assets['button-red'],
    inverseExact:[0,1].every(side=>[180+side*816,450+side*816,790+side*816].every(x=>[190,400,720].every(y=>{
      const screen=window.proof.surfacePoint(side,x,y),local=window.proof.unprojectPoint(side,...screen);
      return Math.abs(local[0]-x)<.001&&Math.abs(local[1]-y)<.001;
    }))),
    wired:window.proof.gameWiring,
    sizes:Object.values(window.proof.manifest.assets).map(a=>a.size),
    texture:window.proof.textureSize()
  }));
  assert(surface.gutter[1] - surface.middle[1] > 10, 'Page surface curves near binding');
  assert(surface.photo[0]>=896&&surface.photo[2]<=640, 'Photo leaves wider inner and outer page margins');
  assert(Math.max(...surface.contour)-Math.min(...surface.contour)>8, 'Photo follows the measured paper contour');
  // The photo now sits nearer the curved upper gutter. Bound its actual edge
  // to a quarter logical pixel of local curvature, rather than its empty box.
  assert(surface.contour.slice(1,-1).every((v,i)=>Math.abs(surface.contour[i]+surface.contour[i+2]-2*v)<.25),
    'Photo contour is smooth rather than following pixel defects');
  assert(new Set(surface.cloudIds).size===3&&surface.cloudIds.length>=7,'Cloud surround uses three silhouettes and seven cutouts');
  assert(surface.newAssets,'Generated component assets are present');
  assert(surface.singleButton,'A/B share one grayscale source texture');
  assert(surface.inverseExact,'Input mapping inverts the shared curved page surface');
  assert(surface.wired === false, 'Game wiring remains disabled');
  assert([...surface.sizes.flat(),surface.texture].every(v=>v>0&&(v&(v-1))===0), 'All art and page texture sizes are powers of two');
  for(const [width,height,name] of [[1672,1060,'reference'],[1280,830,'720p'],[1920,1080,'1080p'],[960,540,'small-landscape']]){
    await page.setViewportSize({width,height});
    await page.evaluate(() => window.proof.seek(0));
    const box=await art.boundingBox();
    assert(box.width<=width+.5&&box.height<=height+.5, name+' fits viewport');
    assert(Math.abs(box.width/box.height-1672/941)<.003,name+' preserves layout aspect');
    await art.screenshot({path:'output/playwright/storybook-revision-5-'+name+'.png'});
  }
  await page.setViewportSize({width:1280,height:830});
  await page.evaluate(()=>window.proof.seek(2,.12));
  await art.screenshot({path:'output/playwright/storybook-revision-5-page-turn.png'});
  for(const [progress,name] of [[.45,'mid'],[.75,'back'],[.99,'landing']]){
    await page.evaluate(progress=>window.proof.seek(2,progress),progress);
    await art.screenshot({path:'output/playwright/storybook-revision-5-turn-'+name+'.png'});
  }
  await page.evaluate(()=>window.proof.seek(0));
  const hiDpi = await page.context().browser().newContext({viewport:{width:1280,height:830},deviceScaleFactor:2});
  const highPage=await hiDpi.newPage();
  await highPage.goto(origin);
  await highPage.waitForFunction(() => window.proof?.ready);
  const ratio=await highPage.getByLabel('Animated storybook design').evaluate(el=>el.width/el.clientWidth);
  assert(Math.abs(ratio-2)<.002,'HiDPI canvas renders at native 2x resolution');
  await highPage.evaluate(()=>window.proof.seek(0));
  await highPage.getByLabel('Animated storybook design').screenshot({path:'output/playwright/storybook-revision-5-hidpi.png',scale:'device'});
  await hiDpi.close();
  await page.emulateMedia({reducedMotion:'reduce'});
  await page.reload();await page.waitForFunction(()=>window.proof?.ready);
  assert(await page.evaluate(()=>!window.proof.state.playing),'Reduced-motion preference starts paused');
  await page.getByRole('button',{name:'Play animation',exact:true}).click();
  assert(await page.evaluate(()=>window.proof.state.playing),'Explicit Play overrides reduced-motion default');
  await page.emulateMedia({reducedMotion:'no-preference'});
  await page.setViewportSize({width:1280,height:830});
  await page.reload();await page.waitForFunction(()=>window.proof?.ready);
  assert(errors.length===0,'No browser errors');
  return {passed:result.length,checks:result,continuity,landing,errors};
}
