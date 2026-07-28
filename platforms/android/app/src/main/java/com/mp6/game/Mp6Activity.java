/* MP6 native port -- the one activity.
 *
 * Follows the shape the U1 design documents for the MP4 port's shell (stock
 * SDL3 Java glue + a single subclass), written for this port's own loader
 * chain (see platform/android/mp6shell.c):
 *
 *   System.loadLibrary("main")           -- libmain.so JNI_OnLoad reserves
 *                                           the low region, android_dlopen_ext
 *                                           loads libmp6game.so INTO it, and
 *                                           chains SDL3-static's JNI_OnLoad
 *                                           (RegisterNatives).
 *   nativeRunMain(getMainSharedObject(), -- dlopens libmp6game.so AGAIN
 *                 getMainFunction(), ...)   (bionic dedupes to the low
 *                                           module) and runs
 *                                           mp6_android_main on the SDL
 *                                           thread.
 *
 * The "args" intent extra is the debug lever the Windows exe gets from its
 * command line: space-separated tokens; MP6_NAME=VALUE tokens become env
 * vars inside mp6_android_main, everything else feeds aurora_main's argv
 * scanner (tick budgets, --input-script).
 *   adb shell am start -n com.mp6.game/.Mp6Activity --es args "MP6_TICK_RATE_LOG=1"
 *
 * Additions for the RmlUi launcher UI:
 *
 *   - straight_boot extra: `-e straight_boot 1` prepends MP6_LAUNCHER=0,
 *     the documented automation-skip lever -- the RmlUi launcher menu is
 *     bypassed entirely and the boot is byte-compatible with the pre-A4
 *     U-A2/U-A3 flow (assumes game content is already staged).
 *
 *   - SAF folder onboarding (the mp6* statics below): the launcher's
 *     "Select Folder" action fires ACTION_OPEN_DOCUMENT_TREE here (SAF
 *     trees have no filesystem path, so both the pick AND the copy-import
 *     live on this side; the disc-image path never touches this -- SDL3's
 *     own ACTION_OPEN_DOCUMENT dialog + content:// fd bridge serve the
 *     native nod importer). platform/android/saf_bridge.c polls these
 *     statics from the SDL thread once per launcher frame. The import runs
 *     on a plain Java thread; progress is published through volatiles.
 *     Contract mirror of platform/content/content_import.cpp: same wanted
 *     file set, same GP6E01 validation, sys/fst.bin written LAST so a torn
 *     import never presents as bootable content.
 */
package com.mp6.game;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.View;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

public class Mp6Activity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        // Only the bootstrap. libmp6game.so must NOT be listed here: a
        // System.loadLibrary would place it at a loader-chosen HIGH address,
        // violating the low-image invariant the bootstrap exists to enforce.
        return new String[] { "main" };
    }

    @Override
    protected String getMainSharedObject() {
        // SDL_RunApp + SDL3 itself live inside the game image; nativeRunMain's
        // dlopen of this exact path dedupes onto the already-low-loaded module.
        return getContext().getApplicationInfo().nativeLibraryDir + "/libmp6game.so";
    }

    @Override
    protected String getMainFunction() {
        return "mp6_android_main"; // platform/main_native.c's android entry
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        ArrayList<String> out = new ArrayList<>();
        if (intent != null) {
            // A4: the on-device straight-boot lever for automation/testing:
            //   adb shell am start -n com.mp6.game/.Mp6Activity -e straight_boot 1
            // maps onto MP6_LAUNCHER=0 (the L1 automation-skip contract) so
            // the launcher menu never opens and the boot log stays
            // byte-compatible with the pre-launcher U-A2/U-A3 flow.
            String sb = intent.getStringExtra("straight_boot");
            boolean straight = (sb != null && (sb.equals("1") || sb.equalsIgnoreCase("true")))
                    || intent.getBooleanExtra("straight_boot", false);
            if (straight) {
                out.add("MP6_LAUNCHER=0");
            }
            String args = intent.getStringExtra("args");
            if (args != null && !args.isEmpty()) {
                out.addAll(Arrays.asList(args.split(" ")));
            }
        }
        return out.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    private void hideSystemBars() {
        // Immersive-sticky fullscreen (pre-androidx API so the app needs no
        // library dependencies at all).
        final View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    /* ===================================================================
     * A4: SAF folder onboarding (see file header). Everything below is the
     * Java half of platform/android/saf_bridge.c.
     * =================================================================== */

    private static final int MP6_REQUEST_FOLDER = 0x4D36; // 'M''6'; clear of SDL's dialog codes

    // Import state values -- MUST mirror platform/content/content_import.h.
    private static final int IMPORT_IDLE = 0;
    private static final int IMPORT_RUNNING = 1;
    private static final int IMPORT_DONE = 2;
    private static final int IMPORT_FAILED = 3;
    private static final int IMPORT_CANCELLED = 4;
    private static final int MAX_FST_ENTRIES = 1 << 16;
    private static final int MAX_FST_BYTES = 8 * 1024 * 1024;
    private static final int BOOT_BYTES = 0x440;
    private static final long MAX_DISC_BYTES = 1459978240L;
    private static final long MAX_WANTED_BYTES = 1610612736L;

    private static final Object sSafLock = new Object();
    private static String sPickedTree = null;      // tree URI, "" pending, "!cancelled"
    private static Thread sImportThread = null;
    private static volatile int sImportState = IMPORT_IDLE;
    private static volatile long sImportBytesDone = 0;
    private static volatile long sImportBytesTotal = 0;
    private static volatile int sImportFilesDone = 0;
    private static volatile int sImportFilesTotal = 0;
    private static volatile String sImportCurrent = "";
    private static volatile String sImportError = "";
    private static volatile boolean sImportCancel = false;

    /** Native (saf_bridge.c): fire the ACTION_OPEN_DOCUMENT_TREE picker. */
    public static void mp6OpenFolderPicker() {
        final Mp6Activity activity = (Mp6Activity) mSingleton;
        if (activity == null) {
            return;
        }
        synchronized (sSafLock) {
            sPickedTree = null;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    activity.startActivityForResult(intent, MP6_REQUEST_FOLDER);
                } catch (Exception e) {
                    synchronized (sSafLock) {
                        sPickedTree = "!cancelled";
                    }
                }
            }
        });
    }

    /** Native: one-shot poll of the picker result ("" = still pending). */
    public static String mp6PollFolderPick() {
        synchronized (sSafLock) {
            if (sPickedTree == null) {
                return "";
            }
            String result = sPickedTree;
            sPickedTree = null;
            return result;
        }
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == MP6_REQUEST_FOLDER) {
            synchronized (sSafLock) {
                if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                    sPickedTree = data.getData().toString();
                } else {
                    sPickedTree = "!cancelled";
                }
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data); // SDL's own file dialog
    }

    /** Native: start the tree import worker. */
    public static boolean mp6TreeImportStart(final String treeUri, final String destRoot) {
        final Mp6Activity activity = (Mp6Activity) mSingleton;
        if (activity == null) return false;
        synchronized (sSafLock) {
            if (sImportState == IMPORT_RUNNING) return false;
            sImportState = IMPORT_RUNNING;
            sImportBytesDone = 0;
            sImportBytesTotal = 0;
            sImportFilesDone = 0;
            sImportFilesTotal = 0;
            sImportCurrent = "";
            sImportError = "";
            sImportCancel = false;
            sImportThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    activity.runTreeImport(Uri.parse(treeUri), destRoot);
                }
            }, "mp6TreeImport");
            try {
                sImportThread.start();
            } catch (RuntimeException e) {
                sImportThread = null;
                sImportState = IMPORT_IDLE;
                sImportError = "could not start folder import worker: " + e;
                return false;
            }
            return true;
        }
    }

    /** Native: progress snapshot [state, bytesDone, bytesTotal, filesDone, filesTotal]. */
    public static long[] mp6TreeImportPoll() {
        return new long[] { sImportState, sImportBytesDone, sImportBytesTotal,
                sImportFilesDone, sImportFilesTotal };
    }

    public static String mp6TreeImportCurrent() {
        return sImportCurrent;
    }

    public static String mp6TreeImportError() {
        return sImportError;
    }

    public static void mp6TreeImportCancel() {
        sImportCancel = true;
    }

    /* ------------------------------------------------------------------
     * Tree import worker (DocumentsContract; no androidx dependency).
     * ------------------------------------------------------------------ */

    private static class DocEntry {
        final String documentId;
        final String name;
        final String mime;
        final long size;
        final boolean sizeKnown;

        DocEntry(String documentId, String name, String mime, long size, boolean sizeKnown) {
            this.documentId = documentId;
            this.name = name;
            this.mime = mime;
            this.size = size;
            this.sizeKnown = sizeKnown;
        }

        boolean isDirectory() {
            return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
        }
    }

    private ArrayList<DocEntry> listChildren(Uri treeUri, String parentDocId) throws IOException {
        if (parentDocId == null) throw new IOException("document provider returned a null document ID");
        ArrayList<DocEntry> out = new ArrayList<>();
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        Cursor c = getContentResolver().query(childrenUri, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE }, null, null, null);
        if (c == null) throw new IOException("document provider returned no child listing");
        try {
            while (c.moveToNext()) {
                String documentId = c.getString(0);
                String displayName = c.getString(1);
                String mime = c.getString(2);
                if (documentId == null || displayName == null || mime == null) {
                    throw new IOException("document provider returned an entry with null identity fields");
                }
                long size = c.isNull(3) ? 0 : c.getLong(3);
                boolean sizeKnown = !c.isNull(3) && size >= 0;
                out.add(new DocEntry(documentId, displayName, mime,
                        sizeKnown ? size : 0, sizeKnown));
                if (out.size() > MAX_FST_ENTRIES) {
                    throw new IOException("document provider directory exceeds 65,536 entries");
                }
            }
        } finally {
            c.close();
        }
        return out;
    }

    private DocEntry findChild(Uri treeUri, String parentDocId, String name) throws IOException {
        DocEntry match = null;
        for (DocEntry e : listChildren(treeUri, parentDocId)) {
            if (e.name.equalsIgnoreCase(name)) {
                if (!safeDisplayName(e.name)) {
                    throw new IOException("unsafe document provider display name: " + e.name);
                }
                if (match != null) {
                    throw new IOException("ambiguous case-colliding provider entries: "
                            + match.name + " and " + e.name);
                }
                match = e;
            }
        }
        return match; // unique case drift is tolerated for hand-copied roots
    }

    // The wanted file set -- MUST mirror content_import.cpp's wanted_file().
    private static boolean wantedFile(String rel) {
        return rel.equals("opening.bnr")
                || rel.equals("sound/MP6_SND.msm")
                || rel.equals("sound/MP6_Str.pdt")
                || rel.equals("data") || rel.startsWith("data/")
                || rel.equals("mess") || rel.startsWith("mess/")
                || rel.equals("mic") || rel.startsWith("mic/");
    }

    private static boolean safeRelPath(String rel) {
        if (rel == null || rel.isEmpty() || rel.startsWith("/")) return false;
        final String invalid = "<>\"|?*";
        final String[] reserved = {
                "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"
        };
        String[] parts = rel.split("/", -1);
        for (String part : parts) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")
                    || part.endsWith(".") || part.endsWith(" ")
                    || part.indexOf('\\') >= 0 || part.indexOf(':') >= 0) return false;
            for (int i = 0; i < part.length(); ++i) {
                char ch = part.charAt(i);
                if (ch < 0x20 || ch > 0x7e || invalid.indexOf(ch) >= 0) return false;
            }
            String base = part.split("\\.", 2)[0].toUpperCase(Locale.ROOT);
            for (String name : reserved) if (base.equals(name)) return false;
            if (base.length() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))
                    && base.charAt(3) >= '1' && base.charAt(3) <= '9') return false;
        }
        return true;
    }

    private static boolean safeDisplayName(String name) {
        return name != null && name.indexOf('/') < 0 && safeRelPath(name);
    }

    private static File safeChild(File root, String rel) throws IOException {
        if (!safeRelPath(rel)) throw new IOException("unsafe content path: " + rel);
        File out = new File(root, rel);
        String rootPath = root.getCanonicalPath();
        String outPath = out.getCanonicalPath();
        if (!outPath.startsWith(rootPath + File.separator)) {
            throw new IOException("content path escapes destination: " + rel);
        }
        return out;
    }

    private static class WantedDoc {
        final DocEntry entry;
        final String rel; // files-root-relative
        final long expectedSize;

        WantedDoc(DocEntry entry, String rel, long expectedSize) {
            this.entry = entry;
            this.rel = rel;
            this.expectedSize = expectedSize;
        }
    }

    private void collectWanted(Uri treeUri, String dirDocId, String relPrefix,
            ArrayList<WantedDoc> out, HashSet<String> visitedDirs, int[] nodeCount,
            int depth) throws IOException {
        if (depth >= 256) throw new IOException("document provider tree exceeds 256 directory levels");
        if (!visitedDirs.add(dirDocId)) {
            throw new IOException("document provider tree contains a directory cycle or alias");
        }
        for (DocEntry e : listChildren(treeUri, dirDocId)) {
            if (sImportCancel) return;
            if (++nodeCount[0] > MAX_FST_ENTRIES) {
                throw new IOException("document provider tree exceeds 65,536 nodes");
            }
            String rel = relPrefix.isEmpty() ? e.name : relPrefix + "/" + e.name;
            if (!safeDisplayName(e.name) || !safeRelPath(rel) || rel.length() >= 1024) {
                throw new IOException("unsafe document provider display name: " + rel);
            }
            if (e.isDirectory()) {
                // Only descend into trees that can still contain wanted files
                // (skips movie/ = 336MB and dll/ entirely).
                if (rel.equals("data") || rel.equals("mess") || rel.equals("mic") || rel.equals("sound")
                        || rel.startsWith("data/") || rel.startsWith("mess/") || rel.startsWith("mic/")) {
                    collectWanted(treeUri, e.documentId, rel, out, visitedDirs,
                            nodeCount, depth + 1);
                }
            } else if (wantedFile(rel)) {
                out.add(new WantedDoc(e, rel, -1));
            }
        }
    }

    private boolean copyDoc(Uri treeUri, DocEntry src, File dest, String rel, long expectedSize) {
        sImportCurrent = rel;
        File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            sImportError = "could not create " + parent.getAbsolutePath();
            return false;
        }
        Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, src.documentId);
        byte[] buf = new byte[1024 * 1024];
        boolean success = false;
        long copied = 0;
        int zeroReads = 0;
        try (InputStream in = getContentResolver().openInputStream(docUri);
                OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                sImportError = "could not open " + rel;
                return false;
            }
            int got;
            while ((got = in.read(buf)) != -1) {
                if (got == 0) {
                    if (++zeroReads > 8) throw new IOException("source made no progress at " + rel);
                    continue;
                }
                zeroReads = 0;
                if (sImportCancel) {
                    return false;
                }
                if ((expectedSize >= 0 && (copied > expectedSize || got > expectedSize - copied)) ||
                        (expectedSize < 0 && (copied > MAX_WANTED_BYTES ||
                                got > MAX_WANTED_BYTES - copied))) {
                    sImportError = "source exceeds expected length at " + rel;
                    return false;
                }
                out.write(buf, 0, got);
                copied += got;
                sImportBytesDone += got;
            }
            if (sImportCancel) return false;
            out.flush();
            long requiredSize = expectedSize >= 0 ? expectedSize : (src.sizeKnown ? src.size : -1);
            if (requiredSize >= 0 && copied != requiredSize) {
                sImportError = "source length mismatch at " + rel + " (expected " + requiredSize
                        + " bytes, copied " + copied + ")";
                return false;
            }
            success = true;
            return true;
        } catch (Exception e) {
            sImportError = "copy failed at " + rel + ": " + e.getMessage() + " (out of space?)";
            return false;
        } finally {
            if (!success && dest.isFile()) dest.delete();
        }
    }

    private static long be32(byte[] b, int off) {
        return ((long)(b[off] & 0xff) << 24) | ((long)(b[off + 1] & 0xff) << 16)
                | ((long)(b[off + 2] & 0xff) << 8) | (long)(b[off + 3] & 0xff);
    }

    private static class FstFile {
        final String path;
        final long size;

        FstFile(String path, long size) {
            this.path = path;
            this.size = size;
        }
    }

    private static ArrayList<FstFile> validateFst(byte[] fst) throws IOException {
        if (fst == null || fst.length < 12 || be32(fst, 0) != 0x01000000L) {
            throw new IOException("fst.bin is too small or its root is not the canonical directory entry");
        }
        if (fst.length > MAX_FST_BYTES) throw new IOException("fst.bin exceeds 8 MiB");
        if (be32(fst, 4) != 0) throw new IOException("fst.bin root parent index is nonzero");
        long countLong = be32(fst, 8);
        if (countLong <= 0 || countLong > MAX_FST_ENTRIES || countLong * 12L > fst.length) {
            throw new IOException("fst.bin entry count is outside the file");
        }
        int count = (int)countLong;
        int stringsAt = count * 12;
        /* Retail GP6E01 starts its string table with "CVS" and its first
         * child uses name offset 0. The root name is implicit: word0 must be
         * canonical above, but fst[stringsAt] must not be required to be NUL. */
        int[] stackIndex = new int[256];
        int[] stackEnd = new int[256];
        String[] stackPath = new String[256];
        ArrayList<FstFile> files = new ArrayList<>();
        HashSet<String> hostPaths = new HashSet<>();
        int depth = 1;
        stackIndex[0] = 0; stackEnd[0] = count; stackPath[0] = "";
        for (int i = 1; i < count; ++i) {
            while (depth > 0 && i >= stackEnd[depth - 1]) --depth;
            if (depth == 0) throw new IOException("fst.bin entry lies outside the root");
            int off = i * 12;
            long word0 = be32(fst, off);
            long type = word0 >>> 24;
            if (type > 1) throw new IOException("fst.bin entry has invalid type at entry " + i);
            int nameOff = (int)(word0 & 0x00ffffffL);
            int nameAt = stringsAt + nameOff;
            if (nameOff < 0 || nameAt < stringsAt || nameAt >= fst.length) {
                throw new IOException("fst.bin name offset is out of range at entry " + i);
            }
            int nul = nameAt;
            while (nul < fst.length && fst[nul] != 0) ++nul;
            if (nul == fst.length || nul == nameAt) {
                throw new IOException("fst.bin name is empty or unterminated at entry " + i);
            }
            String name = new String(fst, nameAt, nul - nameAt, "ISO-8859-1");
            if (!safeDisplayName(name)) {
                throw new IOException("unsafe fst.bin filename: " + name);
            }
            String full = stackPath[depth - 1].isEmpty() ? name : stackPath[depth - 1] + "/" + name;
            if (!safeRelPath(full)) throw new IOException("unsafe fst.bin path: " + full);
            if (full.length() >= 1024) throw new IOException("fst.bin path is too long at entry " + i);
            if (!hostPaths.add(full.toUpperCase(Locale.ROOT))) {
                throw new IOException("fst.bin contains case-colliding host paths: " + full);
            }
            if (type == 1) {
                long parent = be32(fst, off + 4);
                long end = be32(fst, off + 8);
                if (parent != stackIndex[depth - 1] || end <= i || end > stackEnd[depth - 1]
                        || depth >= stackIndex.length) {
                    throw new IOException("fst.bin directory hierarchy is invalid at entry " + i);
                }
                stackIndex[depth] = i;
                stackEnd[depth] = (int)end;
                stackPath[depth] = full;
                ++depth;
            } else {
                long position = be32(fst, off + 4);
                long fileSize = be32(fst, off + 8);
                if (position + fileSize > MAX_DISC_BYTES) {
                    throw new IOException("fst.bin file exceeds the GameCube disc range at entry " + i);
                }
                files.add(new FstFile(full, fileSize));
            }
        }
        return files;
    }

    private byte[] readDocBytes(Uri treeUri, DocEntry src, int maxBytes) throws IOException {
        Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, src.documentId);
        try (InputStream in = getContentResolver().openInputStream(uri);
                ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            if (in == null) throw new IOException("could not open " + src.name);
            byte[] buf = new byte[64 * 1024];
            int zeroReads = 0;
            int got;
            while ((got = in.read(buf)) != -1) {
                if (got == 0) {
                    if (++zeroReads > 8) throw new IOException(src.name + " made no read progress");
                    continue;
                }
                zeroReads = 0;
                if (sImportCancel) throw new IOException("folder import cancelled");
                if (out.size() + got > maxBytes) throw new IOException(src.name + " is implausibly large");
                out.write(buf, 0, got);
            }
            byte[] data = out.toByteArray();
            if (src.sizeKnown && data.length != src.size) {
                throw new IOException(src.name + " ended early");
            }
            return data;
        }
    }

    private static byte[] readFileBytes(File file, int maxBytes) throws IOException {
        try (InputStream in = new FileInputStream(file); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[64 * 1024];
            int got;
            while ((got = in.read(buf)) != -1) {
                if (got == 0) continue;
                if (out.size() + got > maxBytes) throw new IOException(file + " is implausibly large");
                out.write(buf, 0, got);
            }
            return out.toByteArray();
        }
    }

    private static void validateDiscRoot(File root) throws IOException {
        byte[] boot = readFileBytes(new File(root, "sys/boot.bin"), BOOT_BYTES);
        if (boot.length != BOOT_BYTES ||
                !new String(boot, 0, 6, "US-ASCII").equals("GP6E01")) {
            throw new IOException("sys/boot.bin must be exactly 0x440 bytes and identify GP6E01");
        }
        ArrayList<FstFile> manifest = validateFst(
                readFileBytes(new File(root, "sys/fst.bin"), MAX_FST_BYTES));
        int found = 0;
        long wantedBytes = 0;
        for (FstFile entry : manifest) {
            if (!wantedFile(entry.path)) continue;
            if (entry.size > MAX_WANTED_BYTES - wantedBytes) {
                throw new IOException("wanted FST manifest exceeds 1.5 GiB");
            }
            wantedBytes += entry.size;
            File file = safeChild(new File(root, "files"), entry.path);
            if (!file.isFile() || file.length() != entry.size) {
                throw new IOException("missing/truncated FST file: " + entry.path
                        + " (expected " + entry.size + " bytes)");
            }
            if (entry.size > 0) {
                if (entry.path.equals("opening.bnr")) found |= 1 << 0;
                if (entry.path.equals("sound/MP6_SND.msm")) found |= 1 << 1;
                if (entry.path.equals("sound/MP6_Str.pdt")) found |= 1 << 2;
                if (entry.path.startsWith("data/")) found |= 1 << 3;
                if (entry.path.startsWith("mess/")) found |= 1 << 4;
                if (entry.path.startsWith("mic/")) found |= 1 << 5;
            }
        }
        if (found != 0x3f) {
            throw new IOException("incomplete FST/runtime tree (need opening.bnr, both sound banks, data, mess, and mic)");
        }
    }

    private static boolean deleteTree(File file) {
        if (!file.exists()) return true;
        try {
            /* File.listFiles() follows directory symlinks. Never let
             * transactional cleanup walk through one into an unrelated tree. */
            if (java.nio.file.Files.isSymbolicLink(file.toPath())) return false;
        } catch (RuntimeException ignored) {
            return false; /* cannot prove the target is safe to recurse */
        }
        File[] children = file.listFiles();
        if (children != null) for (File child : children) if (!deleteTree(child)) return false;
        return file.delete();
    }

    private static boolean validDiscRoot(File root) {
        if (!root.isDirectory()) return false;
        try {
            validateDiscRoot(root);
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    private static boolean replaceViaQuarantine(File candidate, File dest, File quarantine) {
        if (quarantine.exists() && !deleteTree(quarantine)) return false;
        if (!dest.renameTo(quarantine)) return false;
        if (candidate.renameTo(dest)) {
            deleteTree(quarantine);
            return true;
        }
        /* Best-effort rollback. If this rename itself is interrupted/fails,
         * the next recovery recognizes the quarantine and restores it. */
        quarantine.renameTo(dest);
        return false;
    }

    private static void recoverTree(File dest) {
        File incoming = new File(dest.getPath() + ".incoming");
        File previous = new File(dest.getPath() + ".previous");
        File quarantine = new File(dest.getPath() + ".recovery");
        boolean destValid = validDiscRoot(dest);
        boolean previousValid = validDiscRoot(previous);
        boolean incomingValid = new File(incoming, "sys/.mp6-content-complete").isFile()
                && validDiscRoot(incoming);

        if (destValid) {
            deleteTree(previous);
            deleteTree(incoming);
            deleteTree(quarantine);
            return;
        }

        if (dest.exists()) {
            if (previousValid && replaceViaQuarantine(previous, dest, quarantine)) {
                deleteTree(incoming);
                return;
            }
            if (incomingValid && !previousValid &&
                    replaceViaQuarantine(incoming, dest, quarantine)) {
                new File(dest, "sys/.mp6-content-complete").delete();
                deleteTree(previous);
                return;
            }
            /* With no proven replacement, preserve the pre-existing tree and
             * its backup. Only an uncommitted incoming tree is disposable. */
            if (!incomingValid) deleteTree(incoming);
            return;
        }

        /* Finish or roll back a recovery interrupted after dest was moved out
         * of the way but before a validated replacement became active. */
        if (quarantine.exists()) {
            File candidate = previousValid ? previous : (incomingValid ? incoming : null);
            if (candidate != null && candidate.renameTo(dest)) {
                if (candidate == incoming) {
                    new File(dest, "sys/.mp6-content-complete").delete();
                }
                deleteTree(quarantine);
                if (candidate != previous) deleteTree(previous);
                if (candidate != incoming) deleteTree(incoming);
                return;
            }
            quarantine.renameTo(dest);
            return;
        }

        if (previousValid && previous.renameTo(dest)) {
            deleteTree(incoming);
            return;
        }
        if (incomingValid && incoming.renameTo(dest)) {
            new File(dest, "sys/.mp6-content-complete").delete();
            deleteTree(previous);
            return;
        }
        /* Invalid backups are evidence, not bootable content. Preserve them
         * under their sibling names for diagnostics/retry. */
        if (!incomingValid) deleteTree(incoming);
    }

    private static void cleanupFailedIncoming(File incoming, File dest) {
        if (!incoming.exists()) return;
        File previous = new File(dest.getPath() + ".previous");
        File quarantine = new File(dest.getPath() + ".recovery");
        if (validDiscRoot(incoming) && !validDiscRoot(dest)
                && !validDiscRoot(previous) && !validDiscRoot(quarantine)) {
            android.util.Log.e("Mp6ContentImport",
                    "preserving the only validated content tree after a failed publish: "
                            + incoming);
            return;
        }
        if (!deleteTree(incoming)) {
            android.util.Log.e("Mp6ContentImport",
                    "could not clean failed incoming content tree: " + incoming);
        }
    }

    private static void publishTree(File incoming, File dest) throws IOException {
        validateDiscRoot(incoming);
        File marker = new File(incoming, "sys/.mp6-content-complete");
        try (FileOutputStream out = new FileOutputStream(marker)) {
            out.write("MP6-CONTENT-1\n".getBytes("US-ASCII"));
            out.getFD().sync();
        }
        File previous = new File(dest.getPath() + ".previous");
        if (!deleteTree(previous)) throw new IOException("could not remove stale content backup");
        boolean hadDest = dest.exists();
        if (hadDest && !dest.renameTo(previous)) throw new IOException("could not preserve existing content");
        if (!incoming.renameTo(dest)) {
            if (hadDest) previous.renameTo(dest);
            throw new IOException("could not publish imported content");
        }
        new File(dest, "sys/.mp6-content-complete").delete();
        deleteTree(previous);
    }

    private void runTreeImport(Uri treeUri, String destRoot) {
        File incoming = new File(destRoot + ".incoming");
        try {
            String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);

            // Locate the disc root: the picked folder itself, GP6E01/ inside
            // it, or a Dolphin-style DATA/ child (content_import.cpp mirror).
            String discDocId = null;
            for (String cand : new String[] { "", "GP6E01", "DATA" }) {
                String docId = rootDocId;
                if (!cand.isEmpty()) {
                    DocEntry e = findChild(treeUri, rootDocId, cand);
                    if (e == null || !e.isDirectory()) {
                        continue;
                    }
                    docId = e.documentId;
                }
                DocEntry sys = findChild(treeUri, docId, "sys");
                DocEntry files = findChild(treeUri, docId, "files");
                if (sys != null && sys.isDirectory() && files != null && files.isDirectory()
                        && findChild(treeUri, sys.documentId, "fst.bin") != null) {
                    discDocId = docId;
                    break;
                }
            }
            if (discDocId == null) {
                sImportError = "the selected folder doesn't look like an extracted GameCube disc "
                        + "(need sys/fst.bin + files/ inside it, or a GP6E01 folder containing them)";
                sImportState = IMPORT_FAILED;
                return;
            }

            DocEntry sysDir = findChild(treeUri, discDocId, "sys");
            DocEntry filesDir = findChild(treeUri, discDocId, "files");
            DocEntry fstBin = findChild(treeUri, sysDir.documentId, "fst.bin");
            DocEntry bootBin = findChild(treeUri, sysDir.documentId, "boot.bin");

            // Game-ID validation from boot.bin (offset 0, 6 bytes). GP6E01 =
            // Mario Party 6 (USA); region siblings get a precise message.
            if (bootBin == null) {
                throw new IOException("the selected folder is missing sys/boot.bin; GP6E01 cannot be verified");
            }
            byte[] bootBytes = readDocBytes(treeUri, bootBin, BOOT_BYTES);
            if (bootBytes.length != BOOT_BYTES) {
                throw new IOException("sys/boot.bin must be exactly 0x440 bytes");
            }
            String gameId = new String(bootBytes, 0, 6, "US-ASCII");
            if (!gameId.equals("GP6E01")) {
                sImportError = gameId.startsWith("GP6")
                        ? "wrong region: this disc is " + gameId
                                + " -- the port needs the USA release (GP6E01)"
                        : "not Mario Party 6: game ID " + gameId
                                + " (need GP6E01, Mario Party 6 USA)";
                sImportState = IMPORT_FAILED;
                return;
            }
            byte[] fstBytes = readDocBytes(treeUri, fstBin, MAX_FST_BYTES);
            ArrayList<FstFile> manifest = validateFst(fstBytes);

            ArrayList<WantedDoc> provided = new ArrayList<>();
            collectWanted(treeUri, filesDir.documentId, "", provided,
                    new HashSet<String>(), new int[] { 0 }, 0);
            if (sImportCancel) {
                sImportState = IMPORT_CANCELLED;
                return;
            }
            if (provided.isEmpty()) {
                sImportError = "no Mario Party 6 files found under files/ in the selected folder";
                sImportState = IMPORT_FAILED;
                return;
            }
            HashMap<String, WantedDoc> byPath = new HashMap<>();
            for (WantedDoc w : provided) {
                if (byPath.put(w.rel, w) != null) {
                    throw new IOException("the provider returned the same path more than once: " + w.rel);
                }
            }
            ArrayList<WantedDoc> wanted = new ArrayList<>();
            int found = 0;
            long wantedBytes = 0;
            for (FstFile fstFile : manifest) {
                if (!wantedFile(fstFile.path)) continue;
                if (fstFile.size > MAX_WANTED_BYTES - wantedBytes) {
                    throw new IOException("wanted FST manifest exceeds 1.5 GiB");
                }
                wantedBytes += fstFile.size;
                WantedDoc source = byPath.remove(fstFile.path);
                if (source == null) throw new IOException("missing file required by fst.bin: " + fstFile.path);
                if (source.entry.sizeKnown && source.entry.size != fstFile.size) {
                    throw new IOException("provider size disagrees with fst.bin for " + fstFile.path
                            + " (expected " + fstFile.size + ", found " + source.entry.size + ")");
                }
                wanted.add(new WantedDoc(source.entry, source.rel, fstFile.size));
                if (fstFile.size > 0) {
                    if (fstFile.path.equals("opening.bnr")) found |= 1 << 0;
                    if (fstFile.path.equals("sound/MP6_SND.msm")) found |= 1 << 1;
                    if (fstFile.path.equals("sound/MP6_Str.pdt")) found |= 1 << 2;
                    if (fstFile.path.startsWith("data/")) found |= 1 << 3;
                    if (fstFile.path.startsWith("mess/")) found |= 1 << 4;
                    if (fstFile.path.startsWith("mic/")) found |= 1 << 5;
                }
            }
            if (!byPath.isEmpty()) {
                throw new IOException("selected folder contains wanted files not present in fst.bin: "
                        + byPath.keySet().iterator().next());
            }
            if (found != 0x3f) {
                throw new IOException("the selected folder is incomplete (need opening.bnr, both sound banks, data, mess, and mic)");
            }

            long total = fstBytes.length + bootBytes.length + wantedBytes;
            sImportBytesTotal = total;
            sImportFilesTotal = wanted.size() + 1 + (bootBin != null ? 1 : 0);

            File dest = new File(destRoot);
            recoverTree(dest);
            File previous = new File(destRoot + ".previous");
            File quarantine = new File(destRoot + ".recovery");
            if (validDiscRoot(incoming) || validDiscRoot(previous)
                    || validDiscRoot(quarantine)) {
                throw new IOException("could not resolve an existing valid transactional content tree near "
                        + dest);
            }
            if (!deleteTree(incoming) || !incoming.mkdirs()) {
                throw new IOException("could not prepare transactional import directory " + incoming);
            }
            for (WantedDoc w : wanted) {
                if (sImportCancel) {
                    cleanupFailedIncoming(incoming, dest);
                    sImportState = IMPORT_CANCELLED;
                    return;
                }
                File output = safeChild(incoming, "files/" + w.rel);
                if (!copyDoc(treeUri, w.entry, output, w.rel, w.expectedSize)) {
                    cleanupFailedIncoming(incoming, dest);
                    sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                    return;
                }
                sImportFilesDone++;
            }

            // sys/ last, fst.bin very last (the torn-import contract).
            if (sImportCancel) {
                cleanupFailedIncoming(incoming, dest);
                sImportState = IMPORT_CANCELLED;
                return;
            }
            if (bootBin != null) {
                if (!copyDoc(treeUri, bootBin, safeChild(incoming, "sys/boot.bin"), "sys/boot.bin", bootBytes.length)) {
                    cleanupFailedIncoming(incoming, dest);
                    sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                    return;
                }
                sImportFilesDone++;
            }
            if (!copyDoc(treeUri, fstBin, safeChild(incoming, "sys/fst.bin"), "sys/fst.bin", fstBytes.length)) {
                cleanupFailedIncoming(incoming, dest);
                sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                return;
            }
            sImportFilesDone++;
            if (sImportCancel) {
                cleanupFailedIncoming(incoming, dest);
                sImportState = IMPORT_CANCELLED;
                return;
            }
            publishTree(incoming, dest);
            sImportState = IMPORT_DONE;
        } catch (Exception e) {
            cleanupFailedIncoming(incoming, new File(destRoot));
            sImportError = "folder import failed: " + e;
            sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
        }
    }
}
