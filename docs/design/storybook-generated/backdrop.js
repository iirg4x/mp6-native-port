/* Warm party-day / deep starlit-night scenery, independent from the printed pages.
 * Native shapes stay crisp, with subdued distant details behind the book.
 */
window.createStoryBackdrop = ({width: W, height: H, mask}) => {
  const make = () => {
    const canvas = document.createElement('canvas');
    canvas.width = W;
    canvas.height = H;
    return canvas;
  };
  const night = make(), nightCtx = night.getContext('2d'), frame = make(), frameCtx = frame.getContext('2d');
  // Canvas destination-in consumes alpha, so derive coverage once from the
  // single opaque black/white source. Never stretch the tile to the viewport.
  const tile = document.createElement('canvas');tile.width=mask.width;tile.height=mask.height;
  const tileCtx=tile.getContext('2d');tileCtx.drawImage(mask,0,0);
  const pixels=tileCtx.getImageData(0,0,tile.width,tile.height);
  for(let i=0;i<pixels.data.length;i+=4){pixels.data[i+3]=pixels.data[i];pixels.data[i]=pixels.data[i+1]=pixels.data[i+2]=255;}
  tileCtx.putImageData(pixels,0,0);
  const coverage=make(),coverageCtx=coverage.getContext('2d'),unit=W/2048;
  const tileWidth=tile.width*unit,tileHeight=tile.height*unit,left=W/2-tileWidth/2;
  coverageCtx.fillStyle='#fff';coverageCtx.fillRect(left+tileWidth,0,W,H);
  for(let y=0;y<H;y+=tileHeight)coverageCtx.drawImage(tile,left,y,tileWidth,tileHeight);
  const palette = {dayTop: '#fff48a', dayBottom: '#d79016', nightTop: '#343ab2', nightBottom: '#0b062d'};

  let seed = 606; // fixed, so the star field is the same on every load
  const random = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
  };
  const distantStars = Array.from({length: 240}, () => ({x: W * .5 + random() * W * .5, y: random() * H, r: .55 + random() * 1.1, phase: random() * 6.28}));
  const balloons = [
    {x: 39, y: 260, w: 44, color: '#f35e7d', phase: .3, speed: 13},
    {x: 34, y: 480, w: 30, color: '#28cbbd', phase: 2, speed: 9},
    {x: 375, y: 898, w: 29, color: '#f07629', phase: 4, speed: 11},
    {x: 278, y: 38, w: 28, color: '#76acde', phase: 1, speed: 8},
    // Full day-side field, not just the visible border around the book.
    {x: 165, y: 210, w: 36, color: '#f35e7d', phase: 5, speed: 10},
    {x: 410, y: 220, w: 46, color: '#28cbbd', phase: 1.7, speed: 12},
    {x: 685, y: 195, w: 32, color: '#f07629', phase: 3.1, speed: 8.5},
    {x: 265, y: 430, w: 29, color: '#76acde', phase: 4.4, speed: 9.5},
    {x: 575, y: 410, w: 40, color: '#f35e7d', phase: 2.8, speed: 11},
    {x: 145, y: 665, w: 35, color: '#28cbbd', phase: .9, speed: 12.5},
    {x: 420, y: 685, w: 30, color: '#f07629', phase: 5.5, speed: 8.5},
    {x: 715, y: 740, w: 44, color: '#76acde', phase: 3.8, speed: 10.5},
  ];

  // The night sky is painted once and cut to the zigzag mask.
  const nightSky = nightCtx.createLinearGradient(0, 0, 0, H);
  nightSky.addColorStop(0, palette.nightTop);
  nightSky.addColorStop(.48, '#1c175f');
  nightSky.addColorStop(1, palette.nightBottom);
  nightCtx.fillStyle = nightSky;
  nightCtx.fillRect(0, 0, W, H);
  nightCtx.globalCompositeOperation = 'destination-in';
  nightCtx.drawImage(coverage, 0, 0);
  nightCtx.globalCompositeOperation = 'source-over';

  function day(ctx) {
    const sky = ctx.createLinearGradient(0, 0, 0, H);
    sky.addColorStop(0, palette.dayTop);
    sky.addColorStop(.48, '#f8cc35');
    sky.addColorStop(1, palette.dayBottom);
    ctx.fillStyle = sky;
    ctx.fillRect(0, 0, W, H);
  }

  // Balloons start scattered and rise. A new pass begins below the view only after
  // the whole balloon has cleared the top, so none ever jumps on screen.
  function balloonPositions(t) {
    return balloons.map(b => ({...b, x: b.x + Math.sin(t * .28 + b.phase) * 7,
      y: H + 80 - ((H + 80 - b.y + t * b.speed) % (H + 160))}));
  }

  function drawBalloon(ctx, b, t) {
    const {x, y, w} = b, h = w * 1.3;
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(Math.sin(t * .28 + b.phase) * .045);
    const shape = new Path2D();
    shape.moveTo(0, -h * .53);
    shape.bezierCurveTo(w * .69, -h * .53, w * .63, h * .02, w * .16, h * .32);
    shape.lineTo(-w * .16, h * .32);
    shape.bezierCurveTo(-w * .63, h * .02, -w * .69, -h * .53, 0, -h * .53);
    ctx.fillStyle = b.color;
    ctx.fill(shape);
    // Pale center stripe.
    ctx.save();
    ctx.clip(shape);
    ctx.fillStyle = '#fff3b6';
    ctx.beginPath();
    ctx.ellipse(0, -h * .2, w * .16, h * .57, 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
    ctx.strokeStyle = '#fff3c8';
    ctx.lineWidth = 1.5;
    ctx.stroke(shape);
    // Ropes and basket.
    ctx.strokeStyle = '#9b6729';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(-w * .14, h * .31);
    ctx.lineTo(-w * .1, h * .49);
    ctx.moveTo(w * .14, h * .31);
    ctx.lineTo(w * .1, h * .49);
    ctx.stroke();
    ctx.fillStyle = '#b47738';
    ctx.beginPath();
    ctx.roundRect(-w * .13, h * .46, w * .26, h * .15, 2);
    ctx.fill();
    ctx.restore();
  }

  function dayDetails(ctx, t) {
    for (const b of balloonPositions(t)) drawBalloon(ctx, b, t);
  }

  // Twinkling distant stars over the night sky, cut to the same zigzag mask.
  function nightDetails(ctx, t) {
    frameCtx.clearRect(0, 0, W, H);
    frameCtx.drawImage(night, 0, 0);
    for (const s of distantStars) {
      const light = .5 + .5 * Math.sin(t * (1.3 + s.r * .3) + s.phase), a = .14 + .8 * light;
      frameCtx.fillStyle = 'rgba(227,239,255,' + a + ')';
      frameCtx.shadowColor = 'rgba(173,203,255,' + (.25 + .6 * light) + ')';
      frameCtx.shadowBlur = s.r > 1.1 ? 3 + 6 * light : 0; // only the larger stars glow
      frameCtx.beginPath();
      frameCtx.arc(s.x, s.y, s.r, 0, Math.PI * 2);
      frameCtx.fill();
    }
    frameCtx.shadowBlur = 0;
    frameCtx.globalCompositeOperation = 'destination-in';
    frameCtx.drawImage(coverage, 0, 0);
    frameCtx.globalCompositeOperation = 'source-over';
    ctx.drawImage(frame, 0, 0);
  }

  return {day, dayDetails, nightDetails, palette, balloons, balloonPositions, distantStars};
};
