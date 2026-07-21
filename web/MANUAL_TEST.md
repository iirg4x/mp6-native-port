# Manual test checklist

`node web/test/run.js` covers the FST parser, the wanted-set filter, CRC-32,
the ZIP STORE round-trip, folder/directory-sink logic, and the whole
`iso-source.js` pipeline against both a synthetic fixture and (when
`$MP6_ISO` is set) a real disc. A prior pass of this checklist also drove
`packager.html` live in a real Chromium-based browser pane (served via
`python -m http.server` from `web/`) and confirmed: the whole ES module
graph loads with no console errors, `js/fst.js` + `js/wanted.js` +
`js/zip-writer.js` + `js/zip-reader.js` produce identical results to the
Node test suite when run in-page, and the GitHub auto-fetch path correctly
hit the live API and fell back to the manual-upload UI (there are no
releases yet). It also caught a real bug -- `[hidden]` was losing to
`.status-line`/`.btn`'s own `display: flex` at equal CSS specificity, so
the unsupported-browser warning and the Start/Cancel toggle were silently
staying visible/invisible incorrectly -- fixed in `css/style.css` with a
`[hidden] { display: none !important }` rule.

What that pass could *not* reach (no synthetic file-upload in that
environment, and some paths need a second browser or a multi-hundred-MB
real disc): everything below. Serve `web/` locally
(`python -m http.server 8000` from inside `web/`, then open
`http://localhost:8000/`) and work through this list before a release.

## Landing / Android pages

- [ ] `index.html`: renders correctly light and dark
      (`prefers-color-scheme`), no horizontal scroll at 375px/768px/1280px
      widths, all nav links + the `#legal` anchor work.
- [ ] `android.html`: same checks; the Releases link resolves once a
      release exists.

## Packager -- disc source (Step 1)

- [ ] Pick a real Mario Party 6 (USA) `.iso`: shows the green "Confirmed
      Mario Party 6 (USA)" line with the right file count/size (527 files,
      ~399 MiB, per the feature report's measured numbers) within a couple
      of seconds, without the tab's memory ballooning (Task Manager /
      Activity Monitor should NOT show anything like the disc's 1.3GB
      size -- chunked reads mean this should stay in the tens-of-MB range
      even while later steps stream hundreds of MB through).
- [ ] Pick a wrong-region or non-MP6 GameCube image: specific, readable
      error naming the game ID found.
- [ ] Pick an RVZ/WIA/GCZ/CISO file (rename any small file with the right
      magic bytes, or export one from Dolphin): names the format and
      suggests converting in Dolphin.
- [ ] Drag-and-drop a `.iso` onto the drop zone (not just the file input):
      same result as picking it.
- [ ] Use Dolphin's "Extract Entire Disc..." to produce a folder, then pick
      it via the folder input: same green confirmation. Try both the
      extracted folder itself and a parent folder containing a `GP6E01`
      subfolder.
- [ ] Pick an unrelated folder (no `sys/fst.bin`): clear "doesn't look like
      an extracted GameCube disc" error, not a crash.

## Packager -- engine (Step 2)

- [ ] Once a release exists: "Fetch latest release automatically" downloads
      and reports the right file count; `Start` becomes available without
      also needing the manual path.
- [ ] Force a failure (e.g. block `api.github.com` in devtools' network
      conditions, or test before any release exists): manual fallback
      section appears with a working Releases link; download the zip by
      hand and drop it on the manual drop zone -- it loads the same as
      auto-fetch would.
- [ ] "Continue without an engine": Step 2 goes green immediately, final
      output is content-only (see done-message wording).
- [ ] A real engine zip's DEFLATE-compressed entries decompress correctly
      (this is the one code path the Node suite cannot cover --
      `DecompressionStream` isn't exercised there; see
      `test/zip-roundtrip.test.js`'s header comment). Verify by actually
      running the produced `mp6native.exe` afterward.

## Packager -- output + build (Steps 3-4)

- [ ] **Chromium (Chrome/Edge), directory-picker tier:** choose a folder,
      run a full build with a real disc + real engine, let it finish, then
      confirm the folder has `mp6native.exe` + the 7 DLLs + `res/` at its
      root and `content/GP6E01/{sys,files}` alongside -- and that
      `mp6native.exe` actually boots using that content.
- [ ] Same, but cancel partway through: confirm the run stops promptly, no
      dangling progress, and that `content/GP6E01/sys/fst.bin` is either
      absent or (if the run got that far) the interruption still left the
      partial folder honestly non-bootable -- see `js/packager.js`'s
      header comment on why `fst.bin` is written last.
- [ ] **Firefox or Safari, ZIP-streaming tier:** run a full build; confirm
      it downloads as one `.zip` (not buffered -- watch memory use during
      the download; it should stay bounded, not spike to ~400MB+) and that
      the resulting zip opens correctly in the OS's normal unzip tool with
      `mp6native.exe` and `content/GP6E01/...` both present.
- [ ] Force the buffered fallback (e.g. disable service workers for the
      site in browser settings) and confirm the UI's Step 3 copy correctly
      says memory will be used, and that the download still completes and
      is valid.
- [ ] Revoke folder-write permission mid-run (OS-level, if your platform
      supports it) or otherwise force a write error: build reports FAILED
      with a readable message instead of hanging.

## Cross-cutting

- [ ] Whole flow, start to finish, in: Chrome or Edge (Windows), Firefox
      (Windows), Safari (macOS) if available.
- [ ] Reload the page mid-build: nothing left over (no orphaned service
      worker download registration blocking a later run -- refreshing and
      starting again should just work).
- [ ] `.github/workflows/pages.yml` actually deploys `web/` once Pages is
      enabled on the repo (this step is inert -- do not enable Pages before
      the owner reviews; see `docs/RELEASING.md`).
