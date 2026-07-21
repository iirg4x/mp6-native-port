/* MP6 native port -- U-A2 (docs/UA2_ANDROID_GRAPHICS.md): the one activity.
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
 * A4 (docs/A4_ANDROID_UI.md) additions:
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
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;

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
        if (activity == null || sImportState == IMPORT_RUNNING) {
            return false;
        }
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
        sImportThread.start();
        return true;
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

        DocEntry(String documentId, String name, String mime, long size) {
            this.documentId = documentId;
            this.name = name;
            this.mime = mime;
            this.size = size;
        }

        boolean isDirectory() {
            return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
        }
    }

    private ArrayList<DocEntry> listChildren(Uri treeUri, String parentDocId) {
        ArrayList<DocEntry> out = new ArrayList<>();
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        Cursor c = getContentResolver().query(childrenUri, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE }, null, null, null);
        if (c != null) {
            try {
                while (c.moveToNext()) {
                    out.add(new DocEntry(c.getString(0), c.getString(1), c.getString(2),
                            c.isNull(3) ? 0 : c.getLong(3)));
                }
            } finally {
                c.close();
            }
        }
        return out;
    }

    private DocEntry findChild(Uri treeUri, String parentDocId, String name) {
        for (DocEntry e : listChildren(treeUri, parentDocId)) {
            if (e.name != null && e.name.equals(name)) {
                return e;
            }
        }
        // Tolerate case drift in hand-copied trees.
        for (DocEntry e : listChildren(treeUri, parentDocId)) {
            if (e.name != null && e.name.equalsIgnoreCase(name)) {
                return e;
            }
        }
        return null;
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

    private static class WantedDoc {
        final DocEntry entry;
        final String rel; // files-root-relative

        WantedDoc(DocEntry entry, String rel) {
            this.entry = entry;
            this.rel = rel;
        }
    }

    private void collectWanted(Uri treeUri, String dirDocId, String relPrefix, ArrayList<WantedDoc> out) {
        for (DocEntry e : listChildren(treeUri, dirDocId)) {
            if (sImportCancel || e.name == null) {
                return;
            }
            String rel = relPrefix.isEmpty() ? e.name : relPrefix + "/" + e.name;
            if (e.isDirectory()) {
                // Only descend into trees that can still contain wanted files
                // (skips movie/ = 336MB and dll/ entirely).
                if (rel.equals("data") || rel.equals("mess") || rel.equals("mic") || rel.equals("sound")
                        || rel.startsWith("data/") || rel.startsWith("mess/") || rel.startsWith("mic/")) {
                    collectWanted(treeUri, e.documentId, rel, out);
                }
            } else if (wantedFile(rel)) {
                out.add(new WantedDoc(e, rel));
            }
        }
    }

    private boolean copyDoc(Uri treeUri, DocEntry src, File dest, String rel) {
        sImportCurrent = rel;
        File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            sImportError = "could not create " + parent.getAbsolutePath();
            return false;
        }
        Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, src.documentId);
        byte[] buf = new byte[1024 * 1024];
        try (InputStream in = getContentResolver().openInputStream(docUri);
                OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                sImportError = "could not open " + rel;
                return false;
            }
            int got;
            while ((got = in.read(buf)) > 0) {
                if (sImportCancel) {
                    return false;
                }
                out.write(buf, 0, got);
                sImportBytesDone += got;
            }
            return true;
        } catch (Exception e) {
            sImportError = "copy failed at " + rel + ": " + e.getMessage() + " (out of space?)";
            return false;
        } finally {
            if (sImportError.isEmpty() && sImportCancel && dest.isFile()) {
                dest.delete(); // drop the torn partial on cancel
            }
        }
    }

    private void runTreeImport(Uri treeUri, String destRoot) {
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
            if (bootBin != null) {
                Uri bootUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, bootBin.documentId);
                try (InputStream in = getContentResolver().openInputStream(bootUri)) {
                    byte[] id = new byte[6];
                    if (in != null && in.read(id) == 6) {
                        String gameId = new String(id, "US-ASCII");
                        if (!gameId.equals("GP6E01")) {
                            sImportError = gameId.startsWith("GP6")
                                    ? "wrong region: this disc is " + gameId
                                            + " -- the port needs the USA release (GP6E01)"
                                    : "not Mario Party 6: game ID " + gameId
                                            + " (need GP6E01, Mario Party 6 USA)";
                            sImportState = IMPORT_FAILED;
                            return;
                        }
                    }
                }
            }

            ArrayList<WantedDoc> wanted = new ArrayList<>();
            collectWanted(treeUri, filesDir.documentId, "", wanted);
            if (sImportCancel) {
                sImportState = IMPORT_CANCELLED;
                return;
            }
            if (wanted.isEmpty()) {
                sImportError = "no Mario Party 6 files found under files/ in the selected folder";
                sImportState = IMPORT_FAILED;
                return;
            }

            long total = fstBin.size + (bootBin != null ? bootBin.size : 0);
            for (WantedDoc w : wanted) {
                total += w.entry.size;
            }
            sImportBytesTotal = total;
            sImportFilesTotal = wanted.size() + 1 + (bootBin != null ? 1 : 0);

            File destFiles = new File(destRoot, "files");
            for (WantedDoc w : wanted) {
                if (sImportCancel) {
                    sImportState = IMPORT_CANCELLED;
                    return;
                }
                if (!copyDoc(treeUri, w.entry, new File(destFiles, w.rel), w.rel)) {
                    sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                    return;
                }
                sImportFilesDone++;
            }

            // sys/ last, fst.bin very last (the torn-import contract).
            File destSys = new File(destRoot, "sys");
            if (bootBin != null) {
                if (!copyDoc(treeUri, bootBin, new File(destSys, "boot.bin"), "sys/boot.bin")) {
                    sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                    return;
                }
                sImportFilesDone++;
            }
            if (!copyDoc(treeUri, fstBin, new File(destSys, "fst.bin"), "sys/fst.bin")) {
                sImportState = sImportCancel ? IMPORT_CANCELLED : IMPORT_FAILED;
                return;
            }
            sImportFilesDone++;
            sImportState = IMPORT_DONE;
        } catch (Exception e) {
            sImportError = "folder import failed: " + e;
            sImportState = IMPORT_FAILED;
        }
    }
}
