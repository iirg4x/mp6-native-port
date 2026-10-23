/* Visual-only reference decomposition. No imports from or writes to the game. */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
let sharp;
try { sharp = require('sharp'); } catch {
  sharp = require(path.join(process.env.USERPROFILE, '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp'));
}
const root = __dirname;
const reference = path.join(root, '../settings-storybook-approved.png');
const generated = path.join(process.env.USERPROFILE, '.codex/generated_images/01a05fa3-8d86-7fd3-b430-3861e1ceebb1');
const bookPath = path.join(generated, 'exec-3150537c-1ba0-49c8-ba0d-ad3f5b0d32ad.png');
const skyPath = path.join(generated, 'exec-a06025b2-5121-4991-983f-1eaf427e842c.png');
const W = 1672, H = 941, count = W * H;
const layers = [];
const rect = (x,y,w,h) => `<rect x="${x}" y="${y}" width="${w}" height="${h}"/>`;
const poly = points => `<polygon points="${points}"/>`;
const curve = d => `<path d="${d}"/>`;
function add(id, group, shape, under = null, text = null) {
  layers.push({id, group, shape, under, text});
}
const seam = '836,0 819,14 853,45 810,92 855,155 811,225 853,305 810,375 855,455 810,535 853,615 811,695 855,775 810,838 850,858 1274,858 1232,890 849,891 820,913 847,930 830,941';
add('day-backdrop', 'Backdrop', poly(`0,0 ${seam} 0,941`), 'sky');
add('night-backdrop', 'Backdrop', poly(`${seam} 1672,941 1672,0`), 'sky');
// Decorations have their own texture and pivot, independent of the book.
add('day-sun', 'Backdrop accents', curve('M 96 62 L 126 50 L 150 39 L 174 36 L 195 47 L 216 51 L 236 73 L 244 104 L 229 137 L 202 166 L 165 177 L 132 159 L 106 129 L 93 96 Z'), 'sky');
add('night-crescent', 'Backdrop accents', rect(1539,5,108,113), 'sky');
[[46,0,46,42],[4,48,51,55],[7,136,48,42],[5,249,38,40],[0,391,31,43],[189,901,43,39],[298,876,47,49]].forEach((r,i)=>add(`confetti-${i+1}`, 'Backdrop accents', rect(...r), 'sky'));
[[1250,0,30,30],[1431,4,44,48],[1487,42,30,35],[1633,92,36,35],[1628,149,44,48],[1640,236,30,27],[1635,296,36,36],[1646,455,26,30],[1363,877,36,40],[1520,891,36,43],[1623,899,37,32]].forEach((r,i)=>add(`sky-star-${i+1}`, 'Backdrop accents', rect(...r), 'sky'));
const tabPositions = [[274,14,229,106],[509,25,205,97],[727,25,220,97],[963,27,211,96],[1186,27,216,99]];
const tabNames = ['Picture','Sound','Controls','Game','Extras'];
for (let i=0;i<5;i++) {
  const [x,y,w,h]=tabPositions[i];
  add(`chapter-${tabNames[i].toLowerCase()}-plate`, 'Chapter tabs', `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="13"/>`, i===0?'red-tab':'ivory');
  add(`chapter-${tabNames[i].toLowerCase()}-label`, 'Chapter labels', rect(i===0?338:x+26,38,i===0?120:w-52,40), null, tabNames[i]);
}
add('chapter-sun-badge', 'Chapter tabs', rect(286,28,50,57));
add('chapter-favourite-star', 'Chapter tabs', rect(460,48,33,35));
const cover = 'M 63 105 Q 46 101 47 127 L 4 813 Q 0 851 28 861 L 811 862 L 837 853 L 862 863 L 1647 867 Q 1671 863 1669 832 L 1628 121 Q 1627 106 1611 108 L 1380 83 L 860 77 L 835 92 L 807 76 L 274 81 Z';
add('book-red-cover', 'Book', curve(cover), 'book');
add('book-left-page-stack', 'Book', curve('M 87 112 Q 533 59 810 91 L 836 113 L 836 843 L 813 841 Q 411 826 53 850 L 26 837 Z'), 'book');
add('book-right-page-stack', 'Book', curve('M 836 113 Q 944 58 1589 111 L 1644 835 L 1627 850 Q 1116 826 856 842 L 836 843 Z'), 'book');
add('book-left-page', 'Book', curve('M 105 106 Q 230 81 399 86 Q 744 70 803 95 Q 823 102 836 114 L 836 837 Q 816 812 761 809 Q 442 793 45 819 Z'), 'book');
add('book-right-page', 'Book', curve('M 836 114 Q 869 81 1004 87 Q 1351 76 1564 105 L 1621 824 Q 1448 807 1288 811 Q 925 788 836 837 Z'), 'book');
add('page-ornament-top-left', 'Page ornaments', rect(107,107,83,86));
add('page-ornament-bottom-left', 'Page ornaments', rect(707,717,104,86));
add('page-ornament-top-right', 'Page ornaments', rect(1495,105,84,83));
add('heading-sun-emblem', 'Headings', rect(177,112,127,130));
add('heading-picture', 'Headings', rect(327,113,312,92), null, 'Picture');
add('heading-subtitle', 'Headings', rect(296,211,373,40), null, 'Make the adventure shine.');
add('heading-your-adventure', 'Headings', rect(1068,117,312,57), null, 'Your adventure');
add('heading-moon-emblem', 'Headings', rect(1445,108,75,76));
add('selected-row-ribbon', 'Selection', poly('144,264 783,264 794,272 812,304 791,340 146,340 132,318 131,284'), 'ribbon');
add('selected-row-star', 'Selection', rect(114,273,67,66));
const rows = [
  ['widescreen','Widescreen','On',278,192,264,56],
  ['anti-aliasing','Anti-Aliasing','Smooth',352,181,289,58],
  ['ambient-occlusion','Ambient Occlusion','On',425,181,365,58],
  ['shadow-quality','Shadow Quality','4x',497,181,315,58],
  ['unlocked-fps','Unlocked FPS','On',571,181,299,58],
  ['picture-preset','Picture Preset','Custom',646,198,303,58],
];
for(let i=0;i<rows.length;i++) {
  const [id,label,value,y,x,w,h]=rows[i];
  add(`${id}-label`,'Option labels',rect(x,y,w,h),null,label);
  add(`${id}-value`,'Option values',rect(i===1?608:i===5?605:637,y, i===1||i===5?121:65,h),null,value);
  add(`${id}-previous`,'Option arrows',rect(557,y+4,30,44));
  add(`${id}-next`,'Option arrows',rect(741,y+4,32,44));
  if(i>0) add(`${id}-rule`,'Option rules',rect(165,y+60,623,3));
}
add('preview-photo', 'Preview', rect(871,194,690,377));
add('preview-corner-top-left','Photo corners',poly('862,187 923,188 922,195 870,247 862,243'));
add('preview-corner-top-right','Photo corners',poly('1509,187 1569,188 1571,249 1510,197'));
add('preview-corner-bottom-left','Photo corners',poly('861,522 870,526 920,574 920,584 862,584'));
add('preview-corner-bottom-right','Photo corners',poly('1516,574 1566,525 1578,525 1578,584 1518,584'));
add('preview-caption','Help',rect(1120,576,155,36),null,'Game preview');
add('help-heading','Help',rect(885,613,284,54),null,'Widescreen');
add('help-description','Help',rect(892,671,529,40),null,'See more of the board without stretching it.');
add('help-divider-left','Help',rect(906,736,277,11));
add('help-divider-star','Help',rect(1186,724,37,37));
add('help-divider-right','Help',rect(1225,736,200,11));
add('help-caption','Help',rect(1062,765,285,33),null,'Changes apply as you play.');
// Silhouettes follow the approved paper cutouts, not generic replacement icons.
add('cloud-left-rear','Clouds',curve('M 0 594 Q 34 591 46 630 Q 81 630 88 666 L 120 722 L 214 777 Q 248 780 252 812 Q 271 810 280 832 Q 292 864 269 870 L 0 870 Z'));
add('cloud-right-rear','Clouds',curve('M 1672 616 Q 1640 607 1616 633 Q 1582 624 1562 657 L 1465 756 Q 1436 745 1425 780 Q 1398 781 1394 812 Q 1381 828 1396 858 Q 1402 873 1459 866 L 1672 874 Z'));
add('brighton-cutout','Hosts',poly('0,709 31,699 14,670 16,651 30,646 62,660 66,639 83,641 99,612 109,603 122,607 139,658 171,636 188,633 196,644 192,661 180,684 218,699 229,712 225,729 198,751 211,780 207,794 194,799 165,783 157,821 145,832 129,818 117,851 101,858 89,844 81,823 58,832 47,818 53,797 18,803 0,790 0,760 11,744 0,734'));
add('twila-cutout','Hosts',poly('1533,625 1556,628 1584,639 1608,657 1627,679 1638,707 1640,735 1634,763 1624,785 1622,819 1608,824 1587,806 1514,807 1484,824 1471,814 1481,786 1485,778 1463,766 1450,750 1450,738 1475,724 1492,711 1505,687 1515,663 1517,652 1513,641 1519,631'));
add('twila-star-cutout','Hosts',poly('1424,774 1436,776 1444,787 1458,786 1469,797 1466,810 1456,818 1459,832 1446,840 1431,833 1415,838 1407,828 1409,815 1396,807 1398,793 1414,787'));
add('cloud-left-front','Clouds',curve('M 0 798 Q 21 796 37 816 Q 50 814 58 827 Q 81 829 90 850 Q 118 844 130 862 Q 151 866 152 890 Q 154 910 130 912 L 0 912 Z'));
add('cloud-right-front','Clouds',curve('M 1451 864 Q 1443 838 1466 825 Q 1476 807 1498 811 Q 1516 790 1541 810 Q 1563 792 1588 806 Q 1614 803 1628 833 Q 1655 834 1665 857 Q 1671 882 1648 888 L 1475 888 Q 1452 887 1451 864 Z'));
add('footer-holder','Footer',`<rect x="436" y="857" width="800" height="77" rx="38"/>`,'footer');
add('footer-a-button','Footer buttons',rect(517,864,61,62));
add('footer-choose','Footer labels',rect(580,879,88,34),null,'Choose');
add('footer-b-button','Footer buttons',rect(721,864,61,62));
add('footer-back','Footer labels',rect(785,879,65,34),null,'Back');
add('footer-l-button','Footer buttons',rect(897,870,66,51));
add('footer-slash','Footer labels',rect(963,875,21,41),null,'/');
add('footer-r-button','Footer buttons',rect(981,870,68,51));
add('footer-chapters','Footer labels',rect(1054,879,112,36),null,'Chapters');

async function main() {
  const meta=await sharp(reference).metadata();
  if(meta.width!==W||meta.height!==H) throw Error(`Unexpected reference size ${meta.width}x${meta.height}`);
  const src=await sharp(reference).ensureAlpha().raw().toBuffer();
  const book=await sharp(bookPath).resize(W,H,{fit:'fill'}).ensureAlpha().raw().toBuffer();
  const sky=await sharp(skyPath).resize(W,H,{fit:'fill'}).ensureAlpha().raw().toBuffer();
  const owners=new Int16Array(count).fill(-1);
  for(let i=0;i<layers.length;i++) {
    const l=layers[i];
    const mask=await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}"><g fill="white">${l.shape}</g></svg>`)).ensureAlpha().raw().toBuffer();
    l.mask=new Uint8Array(count);
    for(let p=0;p<count;p++) if(mask[p*4+3]>0) l.mask[p]=1;
    const gold=l.group==='Option arrows'||l.group==='Page ornaments'||l.id==='heading-sun-emblem'||l.id==='chapter-sun-badge'||l.id==='chapter-favourite-star'||l.id==='help-divider-star';
    const moon=l.id==='heading-moon-emblem';
    if(l.text||gold||moon) {
      const ink=new Uint8Array(count);
      const white=l.id==='chapter-picture-label'||l.id==='widescreen-label'||l.id==='widescreen-value';
      for(let p=0;p<count;p++) if(l.mask[p]) {
        const r=src[p*4],g=src[p*4+1],b=src[p*4+2];
        const keep=gold?r>120&&g>70&&b<r-35:moon?b>r+20&&g>r+20:white?g>80&&b>80:(b>r+8&&b>g+8)||(r<100&&g<100&&b<100);
        if(keep) ink[p]=1;
      }
      // Keep the original anti-aliased glyph boundary, not the rectangular page.
      const grown=new Uint8Array(count);
      for(let p=0;p<count;p++) if(ink[p]) for(const delta of [-W-1,-W,-W+1,-1,0,1,W-1,W,W+1]) {
        const q=p+delta;if(q>=0&&q<count&&l.mask[q])grown[q]=1;
      }
      l.mask=grown;
    }
    if(l.group==='Hosts')for(let p=0;p<count;p++)if(l.mask[p]){
      const r=src[p*4],g=src[p*4+1],b=src[p*4+2];
      // Reject blue cloud pixels between the rays, not the cream cutout border.
      if(b>r+12&&b>g-25)l.mask[p]=0;
    }
    for(let p=0;p<count;p++) if(l.mask[p]) owners[p]=i;
  }
  for(let p=0;p<count;p++) if(owners[p]<0) throw Error(`Unassigned source pixel ${p%W},${Math.floor(p/W)}`);
  const out=path.join(root,'textures');fs.mkdirSync(out,{recursive:true});
  const manifest={version:1,status:'visual-only-awaiting-approval',viewport:[W,H],referenceSHA256:crypto.createHash('sha256').update(fs.readFileSync(reference)).digest('hex'),background:{day:'#fff000',night:'#080896',mask:'textures/zigzag-mask.png',textureSize:[2048,1024],contentRect:[0,0,W,H]},particles:[],layers:[]};
  const assembled=Buffer.alloc(count*4);
  const nightMask=Buffer.alloc(2048*1024*4);
  for(let p=0;p<count;p++) {
    const x=p%W,y=Math.floor(p/W),night=layers[1].mask[p];
    const at=(y*2048+x)*4;
    if(night){nightMask[at]=nightMask[at+1]=nightMask[at+2]=nightMask[at+3]=255;}
    const color=night?[8,8,150]:[255,240,0];
    for(let c=0;c<3;c++)assembled[p*4+c]=color[c];assembled[p*4+3]=255;
  }
  await sharp(nightMask,{raw:{width:2048,height:1024,channels:4}}).png().toFile(path.join(out,'zigzag-mask.png'));
  const particleShapes={
    star:'<path d="M64 7 80 43 120 47 89 74 98 115 64 94 30 115 39 74 8 47 48 43Z" fill="#fff349"/>',
    confetti:'<rect x="13" y="8" width="38" height="48" fill="white"/>',
  };
  for(const [id,shape] of Object.entries(particleShapes)) {
    const size=id==='star'?128:64;
    await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}">${shape}</svg>`)).png().toFile(path.join(out,`particle-${id}.png`));
  }
  // Particle instances reference tiny reusable textures, never backdrop artwork.
  for(const l of layers.filter(l=>l.id.startsWith('sky-star-')||l.id.startsWith('confetti-'))) {
    let x0=W,y0=H,x1=0,y1=0;for(let p=0;p<count;p++)if(l.mask[p]){const x=p%W,y=Math.floor(p/W);x0=Math.min(x0,x);y0=Math.min(y0,y);x1=Math.max(x1,x);y1=Math.max(y1,y);}
    const star=l.id.startsWith('sky-star');
    manifest.particles.push({id:l.id,type:star?'star':'confetti',texture:`textures/particle-${star?'star':'confetti'}.png`,position:[(x0+x1)/2,(y0+y1)/2],size:star?Math.min(x1-x0,y1-y0):35,color:star?'#fff349':['#03c5ef','#ee35d7','#19d648','#7536f1'][manifest.particles.length%4],phase:manifest.particles.length*.73});
  }
  const pot=n=>2**Math.ceil(Math.log2(Math.max(1,n)));
  for(let i=0;i<layers.length;i++) {
    const l=layers[i];let x0=W,y0=H,x1=0,y1=0;
    if(l.group==='Backdrop'||l.group==='Backdrop accents')continue;
    for(let p=0;p<count;p++) if(l.mask[p]&&(owners[p]===i||l.under)) {
      const x=p%W,y=Math.floor(p/W);x0=Math.min(x0,x);y0=Math.min(y0,y);x1=Math.max(x1,x+1);y1=Math.max(y1,y+1);
    }
    if(x1<=x0||y1<=y0) continue;
    const cw=x1-x0,ch=y1-y0,tw=pot(cw+4),th=pot(ch+4),pixels=Buffer.alloc(tw*th*4);
    for(let y=y0;y<y1;y++) for(let x=x0;x<x1;x++) {
      const p=y*W+x;if(!l.mask[p]||(!(owners[p]===i)&&!l.under))continue;
      const at=((y-y0+2)*tw+x-x0+2)*4, sourceAt=p*4;
      let rgba;
      if(owners[p]===i) rgba=[src[sourceAt],src[sourceAt+1],src[sourceAt+2],255];
      else if(l.under==='book') rgba=[book[sourceAt],book[sourceAt+1],book[sourceAt+2],255];
      else if(l.under==='sky') rgba=[sky[sourceAt],sky[sourceAt+1],sky[sourceAt+2],255];
      else if(l.under==='ribbon'||l.under==='red-tab') rgba=[248,2,18,255];
      else rgba=[252,250,243,255];
      for(let c=0;c<4;c++){pixels[at+c]=rgba[c];assembled[sourceAt+c]=rgba[c];}
    }
    const name=l.id+'.png';await sharp(pixels,{raw:{width:tw,height:th,channels:4}}).png().toFile(path.join(out,name));
    manifest.layers.push({id:l.id,group:l.group,texture:`textures/${name}`,textureSize:[tw,th],contentRect:[2,2,cw,ch],position:[x0,y0],pivot:[cw/2,ch/2],text:l.text,source:'approved-reference-cutout',hiddenBacking:l.under});
  }
  // Complete sun was hidden by the book; recover only that unseen portion.
  for(const [id,bounds,pivot] of [['day-sun',[89,32,169,171],[84,84]],['night-moon',[1539,5,108,113],[54,56]]]) {
    const [x,y,cw,ch]=bounds,tw=pot(cw+4),th=pot(ch+4),pixels=Buffer.alloc(tw*th*4);
    for(let yy=0;yy<ch;yy++)for(let xx=0;xx<cw;xx++){
      const p=(y+yy)*W+x+xx,at=((yy+2)*tw+xx+2)*4;
      const data=id==='day-sun'?sky:src,r=data[p*4],g=data[p*4+1],b=data[p*4+2];
      const radius=Math.hypot(xx-84,yy-85);
      let inside=id==='day-sun'?(radius<65||(radius<85&&r>210&&g<218&&b<105)):r>100&&g>100&&r>b+25;
      if(!inside)continue;
      pixels[at]=r;pixels[at+1]=g;pixels[at+2]=b;pixels[at+3]=255;
    }
    const name=id+'.png';await sharp(pixels,{raw:{width:tw,height:th,channels:4}}).png().toFile(path.join(out,name));
    manifest.layers.unshift({id,group:'Animated sky',texture:`textures/${name}`,textureSize:[tw,th],contentRect:[2,2,cw,ch],position:[x,y],pivot,animation:id==='day-sun'?'sun-turn':'moon-rock',source:id==='day-sun'?'imagegen-recovered-hidden-sun':'approved-reference-cutout'});
  }
  let unequal=0,maximum=0;
  for(let p=0;p<count;p++){
    if(layers[owners[p]].group.startsWith('Backdrop'))continue;
    let different=false;for(let c=0;c<3;c++){const delta=Math.abs(src[p*4+c]-assembled[p*4+c]);maximum=Math.max(maximum,delta);different||=delta!==0;}if(different)unequal++;
  }
  if(unequal)throw Error(`Reconstruction mismatches ${unequal} pixels`);
  fs.writeFileSync(path.join(root,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
  await sharp(assembled,{raw:{width:W,height:H,channels:4}}).png().toFile(path.join(root,'reconstructed.png'));
  fs.writeFileSync(path.join(root,'verification.json'),JSON.stringify({viewport:[W,H],components:manifest.layers.length,particleInstances:manifest.particles.length,powerOfTwoTextures:manifest.layers.every(l=>l.textureSize.every(v=>(v&(v-1))===0)),foregroundChangedPixels:unequal,foregroundMaximumChannelDifference:maximum,intentionalChanges:['Two flat background colors','Separate zigzag mask','Stars and confetti are particles','Sun and moon animate independently'],method:'Source-derived semantic component assembly. Foreground comparison excludes the explicitly revised backdrop. This proves the frozen visual pose only, not runtime text or animation quality.',runtimeWiring:false},null,2)+'\n');
  console.log(JSON.stringify({components:manifest.layers.length,changedPixels:unequal,powerOfTwo:true}));
}
main().catch(e=>{console.error(e);process.exitCode=1;});
