// MP6 web packager -- service worker.
//
// This file's ONLY job is letting the fallback (non-File-System-Access-API)
// output tier stream a ZIP download to disk without buffering the whole
// archive in the page's JS heap first. It never fetches, caches, or even
// looks at game data or disc images -- those never leave js/packager.js's
// in-page pipeline. Concretely: the page (js/stream-download.js) opens a
// MessageChannel, hands this worker one port, and posts ZIP bytes down it
// as the packager produces them; this worker answers a same-origin fetch
// to a reserved /mp6-download/<id> URL with a streamed Response built from
// those messages, which the browser's normal download machinery then
// writes to disk incrementally. Standard "StreamSaver"-style technique --
// there is nothing here that touches the network or any other origin.
//
// Bounded backpressure: each download tracks how many chunks the page has
// sent but this worker hasn't yet handed to the stream (`unacked`). The
// worker acks every chunk it consumes so the page can cap how far ahead it
// gets (see stream-download.js's MAX_UNACKED) -- without this, a fast local
// disk read paired with a slow network could grow this worker's queue
// without bound.

const downloads = new Map(); // id -> DownloadState

class DownloadState {
    constructor(filename, size, port) {
        this.filename = filename;
        this.size = size; // best-effort; 0/undefined means "unknown, don't set Content-Length"
        this.port = port;
        this.queue = [];
        this.waitingPull = null; // resolver for a pull() that's blocked on more data
        this.done = false;
        this.error = null;
    }

    push(chunk) {
        this.queue.push(chunk);
        this.port.postMessage({ type: "ack" });
        if (this.waitingPull) {
            const resolve = this.waitingPull;
            this.waitingPull = null;
            resolve();
        }
    }

    end() {
        this.done = true;
        if (this.waitingPull) {
            const resolve = this.waitingPull;
            this.waitingPull = null;
            resolve();
        }
    }

    fail(message) {
        this.error = message;
        this.end();
    }
}

self.addEventListener("install", () => {
    self.skipWaiting();
});

self.addEventListener("activate", (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener("message", (event) => {
    const msg = event.data;
    if (!msg || msg.type !== "mp6-download-start") return;
    const port = event.ports[0];
    if (!port) return;
    const state = new DownloadState(msg.filename, msg.size, port);
    downloads.set(msg.id, state);

    port.onmessage = (e) => {
        const m = e.data;
        if (m.type === "chunk") state.push(m.chunk);
        else if (m.type === "end") state.end();
        else if (m.type === "error") state.fail(m.message || "unknown error");
    };
});

self.addEventListener("fetch", (event) => {
    const url = new URL(event.request.url);
    const match = url.pathname.match(/\/mp6-download\/([^/]+)$/);
    if (!match) return; // not a download URL: let the browser handle it normally
    const id = match[1];
    const state = downloads.get(id);
    if (!state) return; // unknown/expired id: not ours

    const stream = new ReadableStream({
        async pull(controller) {
            while (state.queue.length === 0 && !state.done) {
                await new Promise((resolve) => {
                    state.waitingPull = resolve;
                });
            }
            if (state.queue.length > 0) {
                controller.enqueue(state.queue.shift());
                return;
            }
            if (state.error) {
                controller.error(new Error(state.error));
            } else {
                controller.close();
            }
            downloads.delete(id);
        },
        cancel() {
            downloads.delete(id);
        },
    });

    const headers = {
        "Content-Type": "application/zip",
        "Content-Disposition": `attachment; filename="${state.filename.replace(/["\r\n]/g, "")}"`,
        "X-Content-Type-Options": "nosniff",
    };
    if (state.size) headers["Content-Length"] = String(state.size);

    event.respondWith(new Response(stream, { headers }));
});
