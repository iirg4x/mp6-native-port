// MP6 browser build: the async game-content provider behind
// src/web/web_content.c. Linked as --pre-js.
//
// The page chooses a source before the game starts by setting
// Module.mp6ContentSource:
//   { kind: 'files', files: Map<path, Blob> } -- files the user picked or
//       stored in OPFS; paths relative to the disc root ("sys/fst.bin").
// Paths from C arrive absolute under DISC_ROOT and are matched relative to it.
(() => {
  const DISC_ROOT = '/mp6/GP6E01/';
  const BLOCK = 256 * 1024;
  const CACHE_LIMIT = 96 * 1024 * 1024;

  function relative(path) {
    return path.startsWith(DISC_ROOT) ? path.slice(DISC_ROOT.length) : null;
  }

  // Archives (up to this size) are read whole, front to back, in the game's
  // 128 KB blocks; a bulk first read of one fetches all of it at once.
  const WHOLE_FILE = 8 * 1024 * 1024;
  const BULK_READ = 64 * 1024;
  // A read that continues where an earlier read of that file ended fetches
  // this many blocks past itself (music streams, large files), any other
  // read one block. A stereo track is two such streams in one file, so a
  // few recent ends are kept.
  const READ_AHEAD_BLOCKS = 4;
  const STREAMS_PER_FILE = 4;

  // Blocks fetched ahead of the game (read-ahead and scene prefetch) at once;
  // the rest wait their turn so a block the game is waiting for never queues
  // behind them.
  const AHEAD_IN_FLIGHT = 6;

  // LRU of fixed-size blocks. Every read is latency-bound (a Blob slice or a
  // Range fetch costs milliseconds whatever its size), so blocks are fetched
  // concurrently and ahead of the game: a read starts the blocks it needs at
  // once and queues its read-ahead behind them, and a block already here,
  // in flight or queued is never fetched twice (a queued one the game now
  // needs starts at once).
  class BlockCache {
    constructor(fetchRange) {
      this.fetchRange = fetchRange;
      this.blocks = new Map();  // key -> Uint8Array, oldest first
      this.pending = new Map(); // key -> { promise, start (null once started) }
      this.queue = [];          // keys of fetches-ahead not started yet, oldest first
      this.aheadInFlight = 0;
      this.bytes = 0;
      this.ends = new Map(); // rel -> ends of the latest reads of that file
    }
    // The block's bytes if they are here, else the promise of them. `demand`:
    // the game is waiting for this block, so it does not queue.
    block(rel, index, size, demand) {
      const key = rel + '#' + index;
      const entry = this.blocks.get(key);
      if (entry) {
        this.blocks.delete(key);
        this.blocks.set(key, entry);
        return entry;
      }
      let job = this.pending.get(key);
      if (!job) {
        let begin;
        const promise = new Promise((resolve) => { begin = resolve; }).then((ahead) => {
          const start = index * BLOCK;
          return this.fetchRange(rel, start, Math.min(size, start + BLOCK)).finally(() => {
            if (ahead) {
              this.aheadInFlight--;
              this.pump();
            }
          });
        }).then((buf) => {
          const bytes = new Uint8Array(buf);
          this.pending.delete(key);
          this.blocks.set(key, bytes);
          this.bytes += bytes.length;
          while (this.bytes > CACHE_LIMIT && this.blocks.size > 1) {
            const [oldKey, old] = this.blocks.entries().next().value;
            this.blocks.delete(oldKey);
            this.bytes -= old.length;
          }
          return bytes;
        }, (err) => {
          this.pending.delete(key);
          throw err;
        });
        job = { promise, start: begin };
        this.pending.set(key, job);
        this.queue.push(key);
      }
      if (demand && job.start) {
        const i = this.queue.indexOf(key);
        if (i >= 0) this.queue.splice(i, 1);
        job.start(false);
        job.start = null;
      }
      this.pump();
      return job.promise;
    }
    pump() {
      while (this.aheadInFlight < AHEAD_IN_FLIGHT && this.queue.length) {
        const job = this.pending.get(this.queue.shift());
        if (!job || !job.start) continue;
        this.aheadInFlight++;
        job.start(true);
        job.start = null;
      }
    }
    // Requests every block of [offset, end) for the game, and queues the
    // read-ahead after them when this is the read's first request
    // (readCached, then read on a miss); returns the blocks of [offset, end)
    // (bytes or promises) in order.
    request(rel, size, offset, end, track) {
      let last = end;
      if (track) {
        let ends = this.ends.get(rel);
        if (!ends) this.ends.set(rel, ends = []);
        const stream = ends.indexOf(offset);
        if (offset === 0 && end - offset >= BULK_READ && size <= WHOLE_FILE) last = size;
        else last = Math.min(size, end + (stream >= 0 ? READ_AHEAD_BLOCKS : 1) * BLOCK);
        if (stream >= 0) ends.splice(stream, 1);
        ends.push(end);
        if (ends.length > STREAMS_PER_FILE) ends.shift();
      }
      const first = Math.floor(offset / BLOCK);
      const wanted = [];
      for (let i = first; i * BLOCK < end; i++) wanted.push(this.block(rel, i, size, true));
      for (let i = Math.max(first, Math.ceil(end / BLOCK)); i * BLOCK < last; i++) {
        const blk = this.block(rel, i, size, false);
        if (!(blk instanceof Uint8Array)) blk.catch(() => {});
      }
      return wanted;
    }
    static gather(blocks, offset, length) {
      const out = new Uint8Array(length);
      let done = 0;
      let within = offset % BLOCK;
      for (const blk of blocks) {
        const n = Math.min(blk.length - within, length - done);
        out.set(blk.subarray(within, within + n), done);
        done += n;
        within = 0;
      }
      return out;
    }
    // The bytes when every block is already here (no await), else null.
    readCached(rel, size, offset, len) {
      const length = Math.max(0, Math.min(len, size - offset));
      const blocks = this.request(rel, size, offset, offset + length, true);
      if (blocks.some((b) => !(b instanceof Uint8Array))) return null;
      return BlockCache.gather(blocks, offset, length);
    }
    // Queues [offset, offset+len) to be fetched ahead, without waiting for it.
    prefetch(rel, size, offset, len) {
      const end = Math.min(size, offset + len);
      for (let i = Math.floor(offset / BLOCK); i * BLOCK < end; i++) {
        const blk = this.block(rel, i, size, false);
        if (!(blk instanceof Uint8Array)) blk.catch(() => {});
      }
    }
    // The same read, awaiting what is not here yet; call after readCached.
    async read(rel, size, offset, len) {
      const length = Math.max(0, Math.min(len, size - offset));
      const blocks = await Promise.all(this.request(rel, size, offset, offset + length, false));
      return BlockCache.gather(blocks, offset, length);
    }
  }


  function filesProvider(files) {
    const cache = new BlockCache(async (rel, start, end) => files.get(rel).slice(start, end).arrayBuffer());
    return {
      handles: (path) => relative(path) !== null,
      size: (path) => {
        const blob = files.get(relative(path));
        return blob ? blob.size : -1;
      },
      read: (path, offset, len) => {
        const rel = relative(path);
        return cache.read(rel, files.get(rel).size, offset, len);
      },
      readCached: (path, offset, len) => {
        const rel = relative(path);
        return cache.readCached(rel, files.get(rel).size, offset, len);
      },
      prefetch: (path, offset, len) => {
        const rel = relative(path);
        if (files.has(rel)) cache.prefetch(rel, files.get(rel).size, offset, len);
      },
    };
  }

  // A plain GameCube disc image (GCM .iso): no extraction. sys/boot.bin and
  // sys/fst.bin are cut from the header, and every files/ path maps to its
  // byte range in the image through the disc's own file table.
  async function isoProvider(blob) {
    const bytes = async (off, len) => new Uint8Array(await blob.slice(off, off + len).arrayBuffer());
    const be32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
    const boot = await bytes(0, 0x440);
    const fstOff = be32(boot, 0x424), fstSize = be32(boot, 0x428);
    const fst = await bytes(fstOff, fstSize);
    const count = be32(fst, 8);
    const strings = count * 12;
    const nameAt = (o) => {
      let end = strings + o;
      while (fst[end]) end++;
      return new TextDecoder('latin1').decode(fst.subarray(strings + o, end));
    };
    const ranges = new Map([['sys/boot.bin', [0, 0x440]], ['sys/fst.bin', [fstOff, fstSize]]]);
    const walk = (first, end, prefix) => {
      let i = first;
      while (i < end) {
        const e = i * 12;
        const name = nameAt(be32(fst, e) & 0xFFFFFF);
        if (fst[e]) { // directory: next field is the index after its subtree
          const next = be32(fst, e + 8);
          walk(i + 1, next, prefix + name + '/');
          i = next;
        } else {
          ranges.set(prefix + name, [be32(fst, e + 4), be32(fst, e + 8)]);
          i++;
        }
      }
    };
    walk(1, count, 'files/');
    const find = (rel) => ranges.get(rel) ||
      ranges.get([...ranges.keys()].find((k) => k.toLowerCase() === rel.toLowerCase()));
    const cache = new BlockCache(async (rel, start, end) => {
      const [base] = find(rel);
      return blob.slice(base + start, base + end).arrayBuffer();
    });
    return {
      gameId: new TextDecoder('latin1').decode(boot.subarray(0, 6)),
      handles: (path) => relative(path) !== null,
      size: (path) => {
        const r = find(relative(path));
        return r ? r[1] : -1;
      },
      read: (path, offset, len) => {
        const rel = relative(path);
        return cache.read(rel, find(rel)[1], offset, len);
      },
      readCached: (path, offset, len) => {
        const rel = relative(path);
        return cache.readCached(rel, find(rel)[1], offset, len);
      },
      prefetch: (path, offset, len) => {
        const rel = relative(path);
        const r = find(rel);
        if (r) cache.prefetch(rel, r[1], offset, len);
      },
    };
  }


  // Saves (memory-card GCIs) and launcher settings live under /mp6, kept in
  // IndexedDB across launches. A first launch gets phone-friendly settings:
  // no Unlocked FPS (every extra present re-renders the whole frame) and
  // retail shadows and anti-aliasing.
  const HOST_BASE = '/mp6';
  const WEB_DEFAULT_CONFIG = {
    'video.show_fps': true,
    'enhancements.preset': 'Custom',
    'enhancements.widescreen': true,
    'enhancements.unlocked_fps': false,
    'enhancements.shadow_quality': 1,
    'enhancements.aa': 0,
    'enhancements.sfx_voices': 16,
    'enhancements.heap_scale': 1,
    'enhancements.ambient_occlusion': 0,
    'touch.enabled': true,
  };
  function persistSaves() {
    if (typeof IDBFS === 'undefined' || typeof ENVIRONMENT_IS_NODE !== 'undefined' && ENVIRONMENT_IS_NODE) return;
    ENV.MP6_HOST_BASE = HOST_BASE;
    FS.mkdirTree(HOST_BASE);
    FS.mount(IDBFS, { autoPersist: false }, HOST_BASE);
    addRunDependency('mp6-saves');
    // Launch timing (index.html's launch stages): reading the saves and the
    // shader cache back from IndexedDB is one of the steps before the game
    // starts, and its length varies a lot between launches.
    const mark = (name) => { if (Module.mp6Mark) Module.mp6Mark(name); };
    mark('saves: reading');
    FS.syncfs(true, (err) => {
      if (err) console.warn('mp6: could not load saved data', err);
      let files = 0, bytes = 0;
      const walk = (dir) => {
        for (const name of FS.readdir(dir)) {
          if (name === '.' || name === '..') continue;
          const path = dir + '/' + name, st = FS.stat(path);
          if (FS.isDir(st.mode)) walk(path); else { files++; bytes += st.size; }
        }
      };
      try { walk(HOST_BASE); } catch (e) {}
      mark(`saves: read ${files} files, ${Math.round(bytes / 1048576)} MB`);
      const config = HOST_BASE + '/mp6_config.json';
      if (!FS.analyzePath(config).exists) {
        FS.writeFile(config, JSON.stringify(WEB_DEFAULT_CONFIG, null, 2));
      }
      if (!Module.mp6ConfigOverride) removeRunDependency('mp6-saves');
    });
    let syncing = false;
    const flush = () => {
      if (syncing) return;
      syncing = true;
      FS.syncfs(false, () => { syncing = false; });
    };
    setInterval(flush, 5000);
    addEventListener('pagehide', flush);
    document.addEventListener('visibilitychange', () => { if (document.hidden) flush(); });
  }

  Module.preRun = Module.preRun || [];
  Module.preRun.push(persistSaves);
  Module.preRun.push(() => {
    const source = Module.mp6ContentSource;
    if (!source) return;
    if (source.kind === 'files') {
      Module.mp6Content = filesProvider(source.files);
      return;
    }
    if (source.kind === 'iso') {
      addRunDependency('mp6-content');
      if (Module.mp6Mark) Module.mp6Mark('disc: opening');
      isoProvider(source.blob)
        .then((p) => {
          if (Module.mp6Mark) Module.mp6Mark('disc: open');
          Module.mp6Content = p;
          removeRunDependency('mp6-content');
        })
        .catch((e) => {
          console.error(e);
          if (Module.setStatus) Module.setStatus('Could not read the disc image: ' + e.message);
        });
      return;
    }
  });
})();
