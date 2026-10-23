// Mechanical deployment of the generated icon; preserve its artwork and alpha.
const fs=require('node:fs/promises'),path=require('node:path');
let sharp;try{sharp=require('sharp');}catch{sharp=require(path.join(process.env.USERPROFILE,'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp'));}
(async()=>{
 const root=path.resolve(__dirname,'..'),preview=path.join(root,'docs/design/storybook-generated');
 const source=path.join(preview,'sources/chapter-picture.png');
 const png=await sharp(source).resize(512,512,{fit:'inside'}).png().toBuffer();
 const {data,info}=await sharp(png).ensureAlpha().raw().toBuffer({resolveWithObject:true});
 let left=info.width,top=info.height,right=0,bottom=0,clear=false,opaque=false;
 for(let y=0;y<info.height;y++)for(let x=0;x<info.width;x++){
  const a=data[(y*info.width+x)*4+3];clear||=a===0;opaque||=a===255;
  if(a>8){left=Math.min(left,x);top=Math.min(top,y);right=Math.max(right,x+1);bottom=Math.max(bottom,y+1);}
 }
 if(!clear||!opaque)throw Error('Expected genuine transparent generated artwork');
 for(const output of [path.join(preview,'assets/chapter-picture.png'),path.join(root,'res/menu/storybook/chapter-picture.png')])await fs.writeFile(output,png);
 const file=path.join(preview,'manifest.json'),manifest=JSON.parse(await fs.readFile(file,'utf8'));
 manifest.assets['chapter-picture']={file:'assets/chapter-picture.png',size:[info.width,info.height],content:[left,top,right-left,bottom-top],origin:'built-in imagegen; generated alpha preserved'};
 await fs.writeFile(file,JSON.stringify(manifest,null,2)+'\n');
 console.log(JSON.stringify(manifest.assets['chapter-picture']));
})().catch(error=>{console.error(error);process.exitCode=1;});
