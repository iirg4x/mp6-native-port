/* Visual-only confetti. Every piece's position is a pure function of time and its
 * birth, so pause, replay and screenshots agree, and a piece that falls off the
 * bottom never wraps back to the top.
 */
'use strict';
window.createConfettiEmitter = ({width, height, seed = crypto.getRandomValues(new Uint32Array(1))[0]}) => {
  const rate = 2.2, initialCount = 24, maxLifetime = (height + 150) / 70;
  const colors = ['#00c8ed', '#f33bce', '#18d442', '#7734ef'];

  // A small seeded generator per piece, so any piece can be recreated from its id.
  function randomFor(id) {
    let value = (seed ^ Math.imul(id + 1, 0x9e3779b9)) >>> 0;
    return () => {
      value += 0x6d2b79f5;
      let n = value;
      n = Math.imul(n ^ (n >>> 15), n | 1);
      n ^= n + Math.imul(n ^ (n >>> 7), n | 61);
      return ((n ^ (n >>> 14)) >>> 0) / 4294967296;
    };
  }

  // The order of the random() calls below is part of each seed's look; keep it.
  function particle(id, initial) {
    const random = randomFor(initial ? -id - 2 : id), size = 20 + random() * 17;
    const x = random() < .42 ? 15 + random() * 80 : 100 + random() * (width * .46 - 100);
    return {
      id: (initial ? 'initial-' : 'fall-') + id, initial, size, x,
      y: initial ? random() * height : -size - 12 - random() * 35,
      bornAt: initial ? 0 : (id + random() * .7) / rate,
      speed: 70 + random() * 45, drift: (random() - .5) * 8,
      phase: random() * Math.PI * 2, spin: (random() - .5) * 1.8,
      color: colors[Math.floor(random() * colors.length)],
    };
  }

  // Pieces already scattered over the sky when the page opens.
  const initial = Array.from({length: initialCount}, (_, id) => particle(id, true));

  function at(time) {
    const t = Math.max(0, time), out = [];
    const append = p => {
      const age = t - p.bornAt;
      if (age < 0) return;
      const y = p.y + p.speed * age;
      if (y - p.size > height) return;
      out.push({...p, age, y, x: p.x + p.drift * age + Math.sin(age * 1.3 + p.phase) * 8,
        angle: p.phase + p.spin * age, fold: .25 + .75 * Math.abs(Math.cos(age * 2 + p.phase))});
    };
    initial.forEach(append);
    // Only pieces born within one lifetime of t can still be on screen.
    for (let id = Math.max(0, Math.floor((t - maxLifetime) * rate)); id <= Math.floor(t * rate); id++) append(particle(id, false));
    return out;
  }

  return {at, seed, initialCount};
};
