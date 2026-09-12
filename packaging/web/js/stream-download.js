// MP6 web packager -- page-side half of the streaming download fallback
// (sw.js is the other half). Browsers without the File System Access API
// (Firefox, Safari, as of this writing) have no way for a page to stream
// bytes straight to a user-chosen folder, so instead this streams a single
// ZIP to the browser's normal downloads flow via a service worker, which
// keeps this page's own memory use bounded to a small, credit-limited
// queue (MAX_UNACKED chunks) rather than the whole ~400MB+ archive.
//
// If service workers aren't available/registerable at all (very old
// browser, or a first-ever page load that hasn't been claimed by the
// worker yet -- see comment below), createDownloadSink() falls back to
// fully buffering the ZIP in memory and handing it to the browser as one
// Blob. That fallback is honest about the memory cost; js/ui-packager.js
// surfaces which mode is active before the user commits to it.

const MAX_UNACKED = 8; // bounded backpressure: at most this many chunks sit unread in the worker

let swReadyPromise = null;

/** Registers sw.js (idempotent) and waits for this page to be controlled. */
function ensureServiceWorker() {
    if (swReadyPromise) return swReadyPromise;
    swReadyPromise = (async () => {
        if (!("serviceWorker" in navigator)) return null;
        try {
            const reg = await navigator.serviceWorker.register("./sw.js");
            await navigator.serviceWorker.ready;
            if (!navigator.serviceWorker.controller) {
                // First-ever visit: this page loaded before any worker existed to
                // control it. clients.claim() in sw.js's activate handler usually
                // resolves this almost immediately; give it a short window.
                await new Promise((resolve) => {
                    const timer = setTimeout(resolve, 2000);
                    navigator.serviceWorker.addEventListener(
                        "controllerchange",
                        () => {
                            clearTimeout(timer);
                            resolve();
                        },
                        { once: true }
                    );
                });
            }
            return navigator.serviceWorker.controller ? reg : null;
        } catch {
            return null;
        }
    })();
    return swReadyPromise;
}

/** Best-effort capability probe, safe to call to decide which UI copy to show ahead of time. */
export async function streamingDownloadAvailable() {
    return (await ensureServiceWorker()) !== null;
}

function randomId() {
    return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

async function createStreamingSink(filename, sizeHint) {
    const reg = await ensureServiceWorker();
    if (!reg) return null;

    const id = randomId();
    const channel = new MessageChannel();
    let unacked = 0;
    let waitingForAck = null;

    channel.port1.onmessage = (event) => {
        if (event.data && event.data.type === "ack") {
            unacked--;
            if (waitingForAck && unacked < MAX_UNACKED) {
                const resolve = waitingForAck;
                waitingForAck = null;
                resolve();
            }
        }
    };

    navigator.serviceWorker.controller.postMessage(
        { type: "mp6-download-start", id, filename, size: sizeHint },
        [channel.port2]
    );

    const downloadUrl = new URL(`mp6-download/${id}`, reg.scope).href;
    const link = document.createElement("a");
    link.href = downloadUrl;
    link.style.display = "none";
    document.body.appendChild(link);
    link.click();
    link.remove();

    return {
        mode: "stream",
        async write(chunk) {
            while (unacked >= MAX_UNACKED) {
                await new Promise((resolve) => {
                    waitingForAck = resolve;
                });
            }
            unacked++;
            // Zero-copy transfer when `chunk` owns its whole underlying buffer;
            // otherwise (a view into a larger shared buffer -- e.g. re-streaming
            // an entry out of an in-memory engine zip) copy the exact bytes
            // first so we don't neuter a buffer something else still needs.
            const ownsWholeBuffer = chunk.byteOffset === 0 && chunk.byteLength === chunk.buffer.byteLength;
            const payload = ownsWholeBuffer ? chunk : chunk.slice();
            channel.port1.postMessage({ type: "chunk", chunk: payload }, [payload.buffer]);
        },
        async close() {
            channel.port1.postMessage({ type: "end" });
        },
        async abort(message) {
            channel.port1.postMessage({ type: "error", message: String(message) });
        },
    };
}

/** Whole-archive-in-memory fallback for browsers with no usable service worker. */
function createBufferedSink(filename) {
    const parts = [];
    return {
        mode: "buffered",
        async write(chunk) {
            // Copy out of any shared/transferable buffer before it can be reused.
            parts.push(chunk.slice());
        },
        async close() {
            const blob = new Blob(parts, { type: "application/zip" });
            const url = URL.createObjectURL(blob);
            const link = document.createElement("a");
            link.href = url;
            link.download = filename;
            link.style.display = "none";
            document.body.appendChild(link);
            link.click();
            link.remove();
            setTimeout(() => URL.revokeObjectURL(url), 60000);
        },
        async abort() {
            parts.length = 0;
        },
    };
}

/**
 * Returns a sink `{ mode: "stream" | "buffered", write(chunk), close(), abort(message) }`
 * for downloading `filename` as it's produced. Prefers the service-worker
 * streaming path; falls back to full in-memory buffering when that isn't
 * available. `sizeHint` (bytes, optional) is only used to set
 * Content-Length on the streamed response.
 */
export async function createDownloadSink(filename, sizeHint = 0) {
    const streaming = await createStreamingSink(filename, sizeHint);
    return streaming || createBufferedSink(filename);
}
