async page=>{
 await page.waitForFunction(()=>window.proof?.ready);
 return await page.evaluate(async()=>{
  const p=window.proof,b=p.layout.book,image=new Image();image.src=p.manifest.assets['book-half'].file;await image.decode();
  const c=document.createElement('canvas');c.width=816;c.height=779;const ctx=c.getContext('2d'),[sx,sy,sw,sh]=p.manifest.assets['book-half'].content;
  for(let y=0;y<779;y++){const shift=-70*(1-y/779);ctx.save();ctx.beginPath();ctx.rect(0,y,816,1);ctx.clip();ctx.drawImage(image,sx,sy,sw,sh,shift,0,816-shift,779);ctx.restore();}
  const pixels=ctx.getImageData(0,0,816,779).data,rows=[];
  for(const y of [80,160,240,320,400,480,560,640]){
   const rgb=x=>Array.from(pixels.slice((y*816+x)*4,(y*816+x)*4+4));
   const changes=[];for(let x=35;x<125;x++){const a=rgb(x),z=rgb(x+1);if(z[1]-a[1]>3||z[2]-a[2]>3)changes.push([x,a,z]);}
   rows.push({y,colors:Array.from({length:16},(_,i)=>[40+i*5,rgb(40+i*5)]),changes});
  }
  return {rows,border:p.pageBorderPoints(0)};
 });
}
