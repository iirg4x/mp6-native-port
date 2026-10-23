// Headless (node) runs only: forward MP6_* environment variables into the
// Emscripten environment, which does not inherit process.env by default.
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  for (const [k, v] of Object.entries(process.env)) {
    if (k.startsWith('MP6_')) ENV[k] = v;
  }
});
