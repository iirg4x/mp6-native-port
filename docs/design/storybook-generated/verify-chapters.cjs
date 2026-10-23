async page=>{
 await page.goto(new URL(page.url()).origin);
 await page.waitForFunction(()=>window.proof?.ready);
 const checks=[],assert=(ok,message)=>{if(!ok)throw Error(message);checks.push(message);};
 const canvas=page.getByLabel('Animated storybook design');
 const names=await page.evaluate(()=>window.proof.chapters.map(c=>c.name));
 assert(names.join('|')==='Picture|Sound|Game|Extras','Only the four requested desktop chapters are present');
 assert(await page.evaluate(()=>window.proof.chapters[0].rows[0].label==='Picture Preset'),'Picture Preset remains first');
 const counts=[11,2,2,5],pictures=[];
 for(let i=0;i<names.length;i++){
  const data=await page.evaluate(i=>{
   const p=window.proof;p.changeChapter(i,null,false);p.seek(1);
   return {index:p.state.chapter,count:p.chapters[i].rows.length,image:document.querySelector('#stage').toDataURL()};
  },i);
  assert(data.index===i&&data.count===counts[i],names[i]+' contains its source-backed entries');
  pictures.push(data.image);
  await canvas.screenshot({path:'output/playwright/storybook-current-'+names[i].toLowerCase()+'.png'});
 }
 assert(new Set(pictures).size===4,'Each remaining chapter renders distinct content');
 const presets=await page.evaluate(()=>{
  const p=window.proof;p.changeChapter(0,null,false);p.select(0);p.adjust(1);
  const original=p.chapters[0].rows.slice(0,6).map(r=>r.value);
  const linked=[p.chapters[1].rows[1].value,p.chapters[3].rows[3].value];
  p.select(1);p.adjust(1);return {original,linked,custom:p.chapters[0].rows[0].value};
 });
 assert(presets.original.join('|')==='Original|Off|Off|Off|1x|Off'&&presets.linked.join('|')==='Off|Off','Original preset matches source, including sound effects and memory');
 assert(presets.custom==='Custom','Mixed enhancement values derive Custom');
 const pagination=await page.evaluate(()=>{
  const p=window.proof;p.select(6);p.seek(1);
  const xy=p.surfacePoint(0,300,p.layout.ribbon[1]+20);
  return {hit:p.rowAtPoint(...xy),selected:p.state.selected};
 });
 assert(pagination.hit===6&&pagination.selected===6,'Picture continuation uses the same six visible row slots and hit mapping');
 await canvas.screenshot({path:'output/playwright/storybook-current-picture-continuation.png'});
 const disabled=await page.evaluate(()=>{
  const p=window.proof;p.select(8);p.adjust(1);p.select(9);const before=p.chapters[0].rows[9].value;p.adjust(1);
  p.changeChapter(3,null,false);p.select(2);p.adjust(1);
  return {window:p.chapters[0].rows[9].value===before,free:p.chapters[3].rows[2].value};
 });
 assert(disabled.window&&disabled.free==='Off','Fullscreen window size and prelaunch Free Camera remain disabled');
 await page.evaluate(()=>{const p=window.proof;p.changeChapter(1,null,false);p.select(0);p.adjust(-1);p.changeChapter(0,null,false);p.changeChapter(1,null,false);});
 assert(await page.evaluate(()=>window.proof.chapters[1].rows[0].value)==='95%','Master Volume uses five-point increments and survives chapter navigation');
 const transitions=[];
 for(const direction of [1,-1]){
  const result=await page.evaluate(direction=>{
   const p=window.proof,stage=document.querySelector('#stage'),ctx=stage.getContext('2d'),scale=stage.width/1672;
   const pixels=()=>ctx.getImageData(0,0,stage.width,stage.height).data;
   const delta=(a,b,region)=>{
    let total=0,count=0;
    for(let y=Math.ceil(region[1]*scale);y<region[3]*scale;y++)for(let x=Math.ceil(region[0]*scale);x<region[2]*scale;x++){
     const i=(y*stage.width+x)*4;total+=Math.max(Math.abs(a[i]-b[i]),Math.abs(a[i+1]-b[i+1]),Math.abs(a[i+2]-b[i+2]));count++;
    }return total/count;
   };
   const from=direction>0?0:1,to=direction>0?1:0,interior=[110,140,1560,810],star=[100,145,190,230],edge=[1485,100,1610,590];
   p.changeChapter(from,null,false);p.select(0);p.seek(1);const outgoing=pixels();
   p.changeChapter(to,null,false);p.select(0);p.seek(1);const destination=pixels();
   p.changeChapter(from,null,false);p.seek(1);p.changeChapter(to,direction);p.seek(1,.000001);const lift=pixels();
   p.seek(1,.999999);const landing=pixels();p.seek(1,1);const finished=pixels();
   return {from,to,chapter:p.state.chapter,lift:delta(outgoing,lift,interior),landing:delta(destination,landing,interior),finished:delta(destination,finished,interior),
    star:delta(destination,landing,star),edge:delta(outgoing,lift,edge)};
  },direction);
  transitions.push(result);
  assert(result.chapter===result.to,'Chapter turn commits in direction '+direction);
  assert(result.lift<.5&&result.landing<.5&&result.finished===0,'Page interiors align at both ends in direction '+direction+' '+JSON.stringify(result));
  assert(result.star<.15,'Selection star lands without a size/position pop in direction '+direction+' '+result.star);
  assert(result.edge<.5,'Fore-edge starts aligned in direction '+direction+' '+result.edge);
 }
 await page.evaluate(()=>{const p=window.proof;p.changeChapter(0,null,false);p.seek(1);p.changeChapter(1,1);p.seek(1,.3);});
 await canvas.screenshot({path:'output/playwright/storybook-current-forward-turn.png'});
 await page.evaluate(()=>window.proof.seek(1,1));
 await page.evaluate(()=>{const p=window.proof;p.changeChapter(0,-1);p.seek(1,.4);});
 await canvas.screenshot({path:'output/playwright/storybook-current-backward-turn.png'});
 await page.evaluate(()=>window.proof.seek(1,1));
 const box=await canvas.boundingBox(),tab=await page.evaluate(()=>window.proof.tabs[2]);
 await canvas.click({position:{x:(tab[1]+tab[2]/2)*box.width/1672,y:51*box.height/941}});
 await page.evaluate(()=>window.proof.seek(1,1));
 assert(await page.evaluate(()=>window.proof.state.chapter)===2,'Clicking the Game tab works after Controls removal');
 await canvas.press('e');await page.evaluate(()=>window.proof.seek(1,1));
 assert(await page.evaluate(()=>window.proof.state.chapter)===3,'Next chapter reaches Extras');
 await canvas.press('e');await page.evaluate(()=>window.proof.seek(1,1));
 assert(await page.evaluate(()=>window.proof.state.chapter)===0,'Chapter navigation wraps over four chapters');
 await canvas.press('2');await page.evaluate(()=>window.proof.seek(1,1));
 assert(await page.evaluate(()=>window.proof.state.chapter)===1,'Number keys select the remaining chapters');
 await page.emulateMedia({reducedMotion:'reduce'});
 await page.evaluate(()=>window.proof.changeChapter(2));
 assert(await page.evaluate(()=>window.proof.state.chapter===2&&window.proof.state.turnStart===null),'Reduced-motion navigation commits instantly');
 await page.emulateMedia({reducedMotion:'no-preference'});
 await page.reload();await page.waitForFunction(()=>window.proof?.ready);
 return {passed:checks.length,checks,transitions};
}
