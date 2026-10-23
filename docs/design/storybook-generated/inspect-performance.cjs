async page=>{
 await page.waitForFunction(()=>window.proof?.ready);
 return await page.evaluate(()=>{
  const p=window.proof,start=performance.now();p.select(1);
  const select=performance.now()-start,drawStart=performance.now();p.draw();
  return {select,draw:performance.now()-drawStart,width:document.querySelector('#stage').width};
 });
}
