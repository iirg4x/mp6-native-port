// MP6 web packager -- fetches the engine build from this repo's own GitHub
// Releases. Per the repo's Ship-of-Harkinian-style posture (see the README
// "Get it" section), release assets are content-free: a Windows zip
// (mp6native.exe + its 7 DLLs + res/) and a clean arm64 APK, with the game
// disc never involved. This module only ever talks to GitHub's public
// REST API and CDN -- it never sees, needs, or transmits anything about
// the user's disc.
//
// The GitHub REST API itself sends permissive CORS headers, but asset
// *downloads* redirect to a separate storage host whose CORS behavior has
// been inconsistent in the wild, and there may simply be no release yet
// (this repo's very first one gets cut after this feature lands) -- so
// every function here reports failure as a normal return value, never a
// thrown exception, and js/ui-packager.js always offers the manual
// "download it yourself, then drop the file here" path alongside auto-fetch.

const OWNER = "iirg4x";
const REPO = "mp6-native-port";
const API_LATEST = `https://api.github.com/repos/${OWNER}/${REPO}/releases/latest`;
export const RELEASES_PAGE = `https://github.com/${OWNER}/${REPO}/releases/latest`;

const ASSET_PATTERNS = {
    win: /win.*\.zip$|\.zip$/i,
    android: /\.apk$/i,
};

function pickAsset(assets, platform) {
    const pattern = ASSET_PATTERNS[platform];
    return assets.find((a) => pattern.test(a.name)) || null;
}

/**
 * Looks up the latest release's engine asset for `platform` ("win" or
 * "android"). Always resolves; never throws. On success:
 *   { ok: true, release: { tag }, asset: { name, url, size } }
 * On failure:
 *   { ok: false, reason: "network"|"no-releases"|"no-asset"|"http", message }
 */
export async function fetchLatestEngineRelease(platform = "win") {
    let response;
    try {
        response = await fetch(API_LATEST, { headers: { Accept: "application/vnd.github+json" } });
    } catch (e) {
        return {
            ok: false,
            reason: "network",
            message: `Could not reach GitHub (${e.message}). This can happen offline, from a file:// page, or in a strict privacy mode.`,
        };
    }
    if (response.status === 404) {
        return { ok: false, reason: "no-releases", message: "No engine release has been published yet." };
    }
    if (!response.ok) {
        return { ok: false, reason: "http", message: `GitHub API returned HTTP ${response.status}.` };
    }
    let release;
    try {
        release = await response.json();
    } catch (e) {
        return { ok: false, reason: "http", message: `GitHub API returned something unexpected (${e.message}).` };
    }
    const asset = pickAsset(release.assets || [], platform);
    if (!asset) {
        return {
            ok: false,
            reason: "no-asset",
            message: `The latest release (${release.tag_name}) has no ${platform === "win" ? "Windows zip" : "APK"} asset.`,
        };
    }
    return {
        ok: true,
        release: { tag: release.tag_name },
        asset: { name: asset.name, url: asset.browser_download_url, size: asset.size },
    };
}

/**
 * Downloads an asset (as returned by fetchLatestEngineRelease) fully into
 * memory -- engine builds are small (tens of MB), unlike the disc image, so
 * this doesn't need byte-source.js's chunk discipline. Reports progress via
 * `onProgress(receivedBytes, totalBytes)` if given. Never throws.
 */
export async function downloadEngineAsset(asset, onProgress) {
    let response;
    try {
        response = await fetch(asset.url);
    } catch (e) {
        return {
            ok: false,
            message: `Could not download the engine automatically (${e.message}). This is usually a CORS restriction on the release host -- use the manual download link instead.`,
        };
    }
    if (!response.ok || !response.body) {
        return { ok: false, message: `Engine download failed (HTTP ${response.status}).` };
    }
    const total = Number(response.headers.get("content-length")) || asset.size || 0;
    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;
    for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;
        if (onProgress) onProgress(received, total);
    }
    const bytes = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.length;
    }
    return { ok: true, bytes };
}
