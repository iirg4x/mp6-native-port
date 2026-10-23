// Packs only freshly generated art. Never reads/crops the approved concept.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
let sharp;try{sharp=require('sharp');}catch{sharp=require(path.join(process.env.USERPROFILE,'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp'));}
const root=__dirname,prompts=JSON.parse(fs.readFileSync(path.join(root,'prompts.json'),'utf8'));
const sizes={'book-half':[1024,1024],brighton:[512,512],twila:[512,512],cloud:[512,256],'sky-sun':[256,256],'sky-moon':[256,256],'selection-ribbon':[1024,128],'cursor-star':[128,128]};
const only=process.argv.indexOf('--only')>=0?process.argv[process.argv.indexOf('--only')+1]:null;
async function main(){
 fs.mkdirSync(path.join(root,'assets'),{recursive:true});const manifest=only?JSON.parse(fs.readFileSync(path.join(root,'manifest.json'),'utf8')):{status:'visual-only-unapproved',artOrigin:'fresh imagegen output; not sliced from the concept',book:'one half texture, mirrored in rendering',assets:{}};
 if(only&&!prompts.some(spec=>spec.name===only))throw Error('Unknown component: '+only);
 for(const spec of prompts){
  if(only&&spec.name!==only)continue;
  const {data,info}=await sharp(spec.sourcePath).ensureAlpha().raw().toBuffer({resolveWithObject:true});
  const {width:w,height:h}=info;console.log(spec.name,w,h);
  // Neutral luminance is shared by A/B; their only color is renderer tint.
  if(spec.name==='button-round')for(let p=0;p<w*h;p++){
   const offset=p*4,y=Math.round(data[offset]*.2126+data[offset+1]*.7152+data[offset+2]*.0722);
   data[offset]=data[offset+1]=data[offset+2]=y;
  }
  if((spec.name==='book-half'&&!spec.preserveGeneratedAlpha)||spec.name==='selection-ribbon'){
   const shape=spec.name==='book-half'?'<path d="M196 100 Q700 25 1134 31 L1197 44 L1197 1200 Q620 1185 110 1194 Q43 1193 53 1123 L176 125 Q180 108 196 100Z"/>':'<path d="M99 276 H1933 L2008 383 L1933 491 H99 Q55 491 55 446 V322 Q55 276 99 276Z"/>';
   const view=spec.name==='book-half'?'0 0 1280 1280':'0 0 2048 774';
   const mask=await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="${view}"><g fill="white">${shape}</g></svg>`)).ensureAlpha().raw().toBuffer();
   for(let p=0;p<w*h;p++)data[p*4+3]=Math.min(data[p*4+3],mask[p*4+3]);
  }
  // Remove disconnected generation debris while retaining all substantial rays.
  const seen=new Uint8Array(w*h),queue=new Int32Array(w*h),minimum=Math.max(24,Math.floor(w*h*.00003));
  for(let p=0;p<w*h;p++)if(!seen[p]&&data[p*4+3]>32){
   let head=0,tail=1;queue[0]=p;seen[p]=1;
   while(head<tail){const q=queue[head++],x=q%w;for(const d of [-w,w,-1,1]){const n=q+d;if(n<0||n>=w*h||(d===-1&&x===0)||(d===1&&x===w-1)||seen[n]||data[n*4+3]<=32)continue;seen[n]=1;queue[tail++]=n;}}
   if(tail<minimum)for(let k=0;k<tail;k++)data[queue[k]*4+3]=0;
  }
  let left=w,top=h,right=0,bottom=0;
  for(let p=0;p<w*h;p++){if(data[p*4+3]<=32)data[p*4+3]=0;else{const x=p%w,y=Math.floor(p/w);left=Math.min(left,x);top=Math.min(top,y);right=Math.max(right,x+1);bottom=Math.max(bottom,y+1);}}
  const [tw,th]=spec.size||sizes[spec.name],scale=Math.min((tw-8)/(right-left),(th-8)/(bottom-top)),cw=Math.round((right-left)*scale),ch=Math.round((bottom-top)*scale);
  const sprite=await sharp(data,{raw:{width:w,height:h,channels:4}}).extract({left,top,width:right-left,height:bottom-top}).resize(cw,ch).png().toBuffer();
  const x=Math.floor((tw-cw)/2),y=Math.floor((th-ch)/2),output=path.join(root,'assets',spec.name+'.png');
  await sharp({create:{width:tw,height:th,channels:4,background:{r:0,g:0,b:0,alpha:0}}}).composite([{input:sprite,left:x,top:y}]).png().toFile(output);
  manifest.assets[spec.name]={file:'assets/'+spec.name+'.png',size:[tw,th],content:[x,y,cw,ch],sha256:crypto.createHash('sha256').update(fs.readFileSync(output)).digest('hex')};
 }
 if(only){
  fs.writeFileSync(path.join(root,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
  console.log('Prepared only',only,'at',manifest.assets[only].size.join('x'));return;
 }
 // One opaque luminance tile. Day uses 1-mask and night uses mask.
 await require('../../../tools/generate_storybook_mask.cjs')(path.join(root,'assets/zigzag-mask.png'));
 manifest.assets['zigzag-mask']={file:'assets/zigzag-mask.png',size:[64,126],content:[0,0,64,126],origin:'opaque black/white seamless luminance tile'};
 const capture=path.join(process.env.USERPROFILE,'AppData/Local/Temp/codex-clipboard-36ee2a98-803d-48a8-943b-da8efabb597f.png');
 const frame=await sharp(capture).extract({left:1,top:38,width:1120,height:480}).resize(1016,435).png().toBuffer();
 await sharp({create:{width:1024,height:512,channels:4,background:{r:0,g:0,b:0,alpha:0}}}).composite([{input:frame,left:4,top:38}]).png().toFile(path.join(root,'assets/game-preview.png'));
 manifest.assets['game-preview']={file:'assets/game-preview.png',size:[1024,512],content:[4,38,1016,435],origin:'user-provided actual in-game screenshot, not imagegen or concept crop'};
 fs.copyFileSync(path.resolve(root,'../../../res/fonts/BreeSerif-Regular.ttf'),path.join(root,'assets/BreeSerif-Regular.ttf'));
 fs.copyFileSync(path.resolve(root,'../../../res/fonts/BreeSerif-OFL.txt'),path.join(root,'assets/BreeSerif-OFL.txt'));
 if(!Object.values(manifest.assets).every(a=>a.size.every(v=>(v&(v-1))===0)))throw Error('Non-power-of-two texture');
 fs.writeFileSync(path.join(root,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
 console.log('Prepared',Object.keys(manifest.assets).length,'power-of-two textures. No game files changed.');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
