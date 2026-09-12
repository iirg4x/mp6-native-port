// MP6 web packager -- DOM wiring for packager.html. Everything here is
// glue: the actual logic lives in iso-source.js / folder-source.js /
// github-release.js / packager.js / directory-sink.js / zip-stream-sink.js
// / stream-download.js. This file's only job is turning clicks/drops into
// calls on those, and their results into on-screen status.

import { openIsoSource } from "./iso-source.js";
import { openFolderSource } from "./folder-source.js";
import { fetchLatestEngineRelease, downloadEngineAsset } from "./github-release.js";
import { parseZip } from "./zip-reader.js";
import { directoryPickerAvailable, DirectorySink } from "./directory-sink.js";
import { streamingDownloadAvailable, createDownloadSink } from "./stream-download.js";
import { ZipStreamSink } from "./zip-stream-sink.js";
import { runPackage, createCancelToken, STATE } from "./packager.js";

const $ = (id) => document.getElementById(id);

const el = {
    unsupportedWarning: $("unsupported-warning"),
    isoInput: $("iso-input"),
    isoDrop: $("iso-drop"),
    folderInput: $("folder-input"),
    sourceStatus: $("source-status"),
    stepSource: $("step-source"),

    engineFetchBtn: $("engine-fetch-btn"),
    engineSkipBtn: $("engine-skip-btn"),
    engineStatus: $("engine-status"),
    engineManual: $("engine-manual"),
    engineDrop: $("engine-drop"),
    engineInput: $("engine-input"),
    stepEngine: $("step-engine"),

    outputTierSub: $("output-tier-sub"),
    outputTierA: $("output-tier-a"),
    outputTierB: $("output-tier-b"),
    outputTierBMode: $("output-tier-b-mode"),
    chooseFolderBtn: $("choose-folder-btn"),
    outputFolderStatus: $("output-folder-status"),
    stepOutput: $("step-output"),

    startBtn: $("start-btn"),
    cancelBtn: $("cancel-btn"),
    progressWrap: $("progress-wrap"),
    progressBar: $("progress-bar"),
    progressBytes: $("progress-bytes"),
    progressFiles: $("progress-files"),
    progressFile: $("progress-file"),
    buildStatus: $("build-status"),
    donePanel: $("done-panel"),
    doneMessage: $("done-message"),
    stepBuild: $("step-build"),
};

// ---------------------------------------------------------------------
// State
// ---------------------------------------------------------------------

let discSource = null; // { ... } from iso-source.js / folder-source.js
let engineDecided = false; // true once the user picked "fetch"/"provide"/"skip"
let engineZip = null; // { bytes, entries } or null (content-only)
let outputTier = null; // "directory" | "zip"
let directorySink = null; // created once, tier "directory" only
let running = false;

function formatBytes(n) {
    if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GB`;
    if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MB`;
    if (n >= 1024) return `${Math.round(n / 1024)} KB`;
    return `${n} B`;
}

function setStatus(container, kind, message) {
    container.innerHTML = "";
    if (!message) return;
    const line = document.createElement("div");
    line.className = `status-line ${kind}`;
    line.textContent = message;
    container.appendChild(line);
}

function markStepState(section, state) {
    section.classList.remove("active", "done");
    if (state) section.classList.add(state);
}

function updateStartEnabled() {
    const ready = !!discSource && engineDecided && !!outputTier && (outputTier !== "directory" || !!directorySink);
    el.startBtn.disabled = !ready || running;
}

// ---------------------------------------------------------------------
// Capability check
// ---------------------------------------------------------------------

function haveRequiredApis() {
    return typeof Blob !== "undefined" && typeof Blob.prototype.slice === "function" && typeof fetch === "function";
}

if (!haveRequiredApis()) {
    el.unsupportedWarning.hidden = false;
}

// ---------------------------------------------------------------------
// Step 1: disc source
// ---------------------------------------------------------------------

function wireDrop(dropEl, onFile) {
    dropEl.addEventListener("dragover", (e) => {
        e.preventDefault();
        dropEl.classList.add("dragover");
    });
    dropEl.addEventListener("dragleave", () => dropEl.classList.remove("dragover"));
    dropEl.addEventListener("drop", (e) => {
        e.preventDefault();
        dropEl.classList.remove("dragover");
        const files = e.dataTransfer && e.dataTransfer.files;
        if (files && files.length > 0) onFile(files[0]);
    });
}

async function handleIsoFile(file) {
    el.folderInput.value = "";
    setStatus(el.sourceStatus, "info", `Reading ${file.name}…`);
    const result = await openIsoSource(file);
    if (!result.ok) {
        discSource = null;
        setStatus(el.sourceStatus, "error", result.error);
    } else {
        discSource = result.source;
        const total = formatBytes(discSource.totalWantedBytes);
        setStatus(
            el.sourceStatus,
            "ok",
            `Confirmed Mario Party 6 (USA) -- ${discSource.wantedFiles.length} files, ${total} to extract.`
        );
        markStepState(el.stepSource, "done");
    }
    updateStartEnabled();
}

async function handleFolderPick(fileList) {
    el.isoInput.value = "";
    if (!fileList || fileList.length === 0) return;
    setStatus(el.sourceStatus, "info", "Reading folder…");
    const result = await openFolderSource(fileList);
    if (!result.ok) {
        discSource = null;
        setStatus(el.sourceStatus, "error", result.error);
    } else {
        discSource = result.source;
        const total = formatBytes(discSource.totalWantedBytes);
        setStatus(
            el.sourceStatus,
            "ok",
            `Confirmed Mario Party 6 (USA) -- ${discSource.wantedFiles.length} files, ${total} to extract.`
        );
        markStepState(el.stepSource, "done");
    }
    updateStartEnabled();
}

wireDrop(el.isoDrop, (file) => handleIsoFile(file));
el.isoInput.addEventListener("change", () => {
    if (el.isoInput.files.length > 0) handleIsoFile(el.isoInput.files[0]);
});
el.folderInput.addEventListener("change", () => handleFolderPick(el.folderInput.files));

// ---------------------------------------------------------------------
// Step 2: engine
// ---------------------------------------------------------------------

async function loadEngineZipBytes(bytes, label) {
    try {
        const { entries } = parseZip(bytes);
        if (entries.length === 0) throw new Error("the zip has no files in it");
        engineZip = { bytes, entries };
        engineDecided = true;
        setStatus(el.engineStatus, "ok", `Engine ready: ${label} (${entries.length} files).`);
        el.engineManual.hidden = true;
        markStepState(el.stepEngine, "done");
    } catch (e) {
        setStatus(el.engineStatus, "error", `That doesn't look like a usable engine zip: ${e.message}`);
    }
    updateStartEnabled();
}

el.engineFetchBtn.addEventListener("click", async () => {
    el.engineFetchBtn.disabled = true;
    setStatus(el.engineStatus, "info", "Looking up the latest release…");
    el.engineManual.hidden = true;
    const release = await fetchLatestEngineRelease("win");
    if (!release.ok) {
        setStatus(el.engineStatus, "error", release.message);
        el.engineManual.hidden = false;
        el.engineFetchBtn.disabled = false;
        return;
    }
    setStatus(el.engineStatus, "info", `Downloading ${release.asset.name}…`);
    const download = await downloadEngineAsset(release.asset, (received, total) => {
        const pct = total ? ` (${Math.round((received / total) * 100)}%)` : "";
        setStatus(el.engineStatus, "info", `Downloading ${release.asset.name}${pct}…`);
    });
    el.engineFetchBtn.disabled = false;
    if (!download.ok) {
        setStatus(el.engineStatus, "error", download.message);
        el.engineManual.hidden = false;
        return;
    }
    await loadEngineZipBytes(download.bytes, `${release.asset.name} (${release.release.tag})`);
});

el.engineSkipBtn.addEventListener("click", () => {
    engineZip = null;
    engineDecided = true;
    setStatus(el.engineStatus, "ok", "Continuing without an engine -- this run will produce content/GP6E01 only.");
    el.engineManual.hidden = true;
    markStepState(el.stepEngine, "done");
    updateStartEnabled();
});

async function handleEngineFile(file) {
    setStatus(el.engineStatus, "info", `Reading ${file.name}…`);
    const bytes = new Uint8Array(await file.arrayBuffer());
    await loadEngineZipBytes(bytes, file.name);
}
wireDrop(el.engineDrop, (file) => handleEngineFile(file));
el.engineInput.addEventListener("change", () => {
    if (el.engineInput.files.length > 0) handleEngineFile(el.engineInput.files[0]);
});

// ---------------------------------------------------------------------
// Step 3: output tier
// ---------------------------------------------------------------------

async function initOutputTier() {
    if (directoryPickerAvailable()) {
        outputTier = "directory";
        el.outputTierSub.textContent = "Writes straight to a folder you choose";
        el.outputTierA.hidden = false;
    } else {
        outputTier = "zip";
        el.outputTierSub.textContent = "Downloads a single ZIP (your browser can't write to a folder directly)";
        el.outputTierB.hidden = false;
        const streaming = await streamingDownloadAvailable();
        el.outputTierBMode.textContent = streaming
            ? "This browser can stream the ZIP straight to disk as it's built."
            : "This browser will need to hold the whole ZIP in memory before the download starts -- fine for " +
              "most systems, but expect noticeably higher RAM use for a full ~400MB+ build.";
    }
    updateStartEnabled();
}
initOutputTier();

el.chooseFolderBtn.addEventListener("click", async () => {
    try {
        const handle = await window.showDirectoryPicker({ id: "mp6-output", mode: "readwrite" });
        directorySink = new DirectorySink(handle);
        setStatus(el.outputFolderStatus, "ok", `Selected folder: ${handle.name}`);
        markStepState(el.stepOutput, "done");
    } catch (e) {
        if (e && e.name === "AbortError") return; // user cancelled the picker; not an error
        setStatus(el.outputFolderStatus, "error", `Could not use that folder: ${e.message}`);
    }
    updateStartEnabled();
});

// ---------------------------------------------------------------------
// Step 4: build
// ---------------------------------------------------------------------

let cancelToken = null;

function resetProgressUi() {
    el.progressBar.value = 0;
    el.progressBytes.textContent = "";
    el.progressFiles.textContent = "";
    el.progressFile.textContent = "";
}

function onProgress(status) {
    if (status.bytesTotal > 0) {
        el.progressBar.value = status.bytesDone / status.bytesTotal;
    }
    el.progressBytes.textContent = `${formatBytes(status.bytesDone)} / ${formatBytes(status.bytesTotal)}`;
    el.progressFiles.textContent = `file ${status.filesDone} / ${status.filesTotal}`;
    el.progressFile.textContent = status.currentFile || "";
}

function setControlsEnabled(enabled) {
    el.isoInput.disabled = !enabled;
    el.folderInput.disabled = !enabled;
    el.engineFetchBtn.disabled = !enabled;
    el.engineSkipBtn.disabled = !enabled;
    el.engineInput.disabled = !enabled;
    el.chooseFolderBtn.disabled = !enabled;
}

el.startBtn.addEventListener("click", async () => {
    running = true;
    setControlsEnabled(false);
    el.startBtn.hidden = true;
    el.cancelBtn.hidden = false;
    el.cancelBtn.disabled = false;
    el.cancelBtn.textContent = "Cancel";
    el.progressWrap.hidden = false;
    el.donePanel.hidden = true;
    resetProgressUi();
    setStatus(el.buildStatus, "info", "Starting…");
    markStepState(el.stepBuild, "active");

    cancelToken = createCancelToken();

    let sink;
    if (outputTier === "directory") {
        sink = directorySink;
    } else {
        const sizeHint =
            discSource.totalWantedBytes + (engineZip ? engineZip.entries.reduce((s, e) => s + e.uncompressedSize, 0) : 0);
        const downloadSink = await createDownloadSink("mp6-native-port-output.zip", sizeHint);
        sink = new ZipStreamSink(downloadSink);
    }

    const status = await runPackage({ discSource, engineZip, sink, cancelToken, onProgress });

    running = false;
    el.cancelBtn.hidden = true;
    el.startBtn.hidden = false;
    setControlsEnabled(true);
    el.startBtn.disabled = true; // this run is done; reload to start another

    if (status.state === STATE.DONE) {
        setStatus(el.buildStatus, "ok", "Build complete." + skippedSuffix(status.skippedCount));
        markStepState(el.stepBuild, "done");
        el.progressWrap.hidden = true;
        el.donePanel.hidden = false;
        el.doneMessage.textContent = doneMessageFor(outputTier, !!engineZip);
    } else if (status.state === STATE.CANCELLED) {
        setStatus(el.buildStatus, "info", "Cancelled." + skippedSuffix(status.skippedCount));
        el.startBtn.disabled = false;
    } else {
        setStatus(el.buildStatus, "error", `Build failed: ${status.error}` + skippedSuffix(status.skippedCount));
        el.startBtn.disabled = false;
    }
});

// SECURITY: a hostile or malformed disc/folder can carry entries whose
// name would otherwise escape the output root (path-safe.js /
// directory-sink.js); those are skipped rather than aborting the run, but
// must not go unnoticed -- surface the count next to whatever status line
// is already showing, for any run outcome.
function skippedSuffix(skippedCount) {
    if (!skippedCount) return "";
    const noun = skippedCount === 1 ? "entry" : "entries";
    return ` (${skippedCount} disc ${noun} skipped as unsafe -- see console for names.)`;
}

el.cancelBtn.addEventListener("click", () => {
    if (cancelToken) cancelToken.cancelled = true;
    el.cancelBtn.disabled = true;
    el.cancelBtn.textContent = "Cancelling…";
});

function doneMessageFor(tier, hasEngine) {
    if (tier === "directory") {
        return hasEngine
            ? "Open the folder you chose and run mp6native.exe."
            : "The folder you chose now has content/GP6E01 in it. Combine it with an engine build " +
              "(mp6native.exe + its DLLs + res/) to play -- see the Packager's engine step, or build " +
              "the engine yourself per docs/BUILDING.md.";
    }
    return hasEngine
        ? "Extract the downloaded ZIP, then run mp6native.exe from inside it."
        : "Extract the downloaded ZIP -- it contains content/GP6E01. Combine it with an engine build " +
          "(mp6native.exe + its DLLs + res/) to play.";
}

updateStartEnabled();
