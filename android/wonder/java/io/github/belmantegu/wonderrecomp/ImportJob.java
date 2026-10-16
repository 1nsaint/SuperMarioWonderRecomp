package io.github.belmantegu.wonderrecomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * One import at a time, on its own thread; the launcher polls the fields. Two kinds:
 * - dump: a folder the player picked (SAF) with prod.keys (+ title.keys) and the game's XCI or NSPs (sub-folders are
 *   searched). NativeImporter writes analysis/ and extract/romfs/; then the translated shaders bundled in the APK
 *   (assets/spirv.zip) go to spirv/ and the default config.txt is written when there is none.
 * - zip: a data pack made earlier (ZipImporter).
 */
final class ImportJob {
    private static volatile ImportJob current;

    volatile boolean acknowledged, cancelled, finished, dump;
    volatile long done, total;
    volatile int files;
    volatile String error;
    volatile String phase = "";
    volatile String currentName = "";
    volatile double seconds;
    volatile ZipImporter.Result result;
    /** Steps of the dump import, for the progress card: keys, container, extract. */
    volatile boolean keysOk, containerOk;
    volatile String containerName = "";
    final AtomicBoolean cancel = new AtomicBoolean();
    private final long startedAt = System.nanoTime();

    static ImportJob current() {
        return current;
    }

    void requestCancel() {
        cancel.set(true);
        if (dump && NativeImporter.load()) NativeImporter.cancel();
    }

    private static synchronized ImportJob begin(boolean dump) {
        if (current != null && !current.finished) return null;
        ImportJob j = new ImportJob();
        j.dump = dump;
        current = j;
        return j;
    }

    static ImportJob startDump(Context context, Uri tree, File dir) {
        final ImportJob j = begin(true);
        if (j == null) return current;
        final Context app = context.getApplicationContext();
        final Uri t = tree;
        final File d = dir;
        new Thread(new Runnable() {
            @Override
            public void run() {
                j.workDump(app, t, d);
            }
        }, "wonder-import").start();
        return j;
    }

    static ImportJob startZip(Context context, Uri zip, File dir) {
        final ImportJob j = begin(false);
        if (j == null) return current;
        final Context app = context.getApplicationContext();
        final Uri u = zip;
        final File d = dir;
        new Thread(new Runnable() {
            @Override
            public void run() {
                j.workZip(app, u, d);
            }
        }, "wonder-import").start();
        return j;
    }

    /** Only the shaders (and config): an APK update that ships a newer shader set over an imported game. */
    static ImportJob startShaders(Context context, File dir) {
        final ImportJob j = begin(true);
        if (j == null) return current;
        j.phase = "shaders";
        final Context app = context.getApplicationContext();
        final File d = dir;
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    j.installShaders(app, d);
                    AppFiles.installConfig(app);
                } catch (IOException | RuntimeException e) {
                    j.error = e.getMessage();
                } finally {
                    j.finish();
                }
            }
        }, "wonder-shaders").start();
        return j;
    }

    private void finish() {
        seconds = (System.nanoTime() - startedAt) / 1e9;
        finished = true;
    }

    // ---- ZIP data pack ----

    private void workZip(Context context, Uri uri, File dir) {
        try {
            dir.mkdirs();
            ZipImporter.cleanParts(dir);
            InputStream in = context.getContentResolver().openInputStream(uri);
            if (in == null) throw new IOException(Lang.s("Could not open the chosen file", "Não foi possível abrir o arquivo escolhido"));
            result = ZipImporter.run(in, dir, new ZipImporter.Listener() {
                @Override
                public void onProgress(long bytes, int n, String name) {
                    done = bytes;
                    files = n;
                    currentName = name;
                }
            }, cancel);
            AppFiles.installConfig(context);
            ZipImporter.cleanParts(dir);
        } catch (ZipImporter.Cancelled e) {
            cancelled = true;
            error = Lang.s("Import cancelled. The files already copied are kept.", "Importação cancelada. Os arquivos já copiados foram mantidos.");
        } catch (SecurityException e) {
            error = Lang.s("No permission to read the file: ", "Sem permissão para ler o arquivo: ") + e.getMessage();
        } catch (IOException e) {
            error = e.getMessage();
        } catch (RuntimeException e) {
            error = Lang.s("Unexpected error: ", "Erro inesperado: ") + e;
        } finally {
            finish();
        }
    }

    // ---- dump + keys folder ----

    private static final class Doc {
        String name;
        Uri uri;
    }

    /** Keys and the dump's containers: .xci and its split parts (.xc0 .xc1 .. , .xci.00 ..), .nsp. */
    static boolean wanted(String name) {
        String low = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return low.endsWith(".xci") || low.matches(".*\\.xc[0-9]") || low.matches(".*\\.xci\\.[0-9]+")
                || low.endsWith(".nsp") || isKeys(low);
    }

    static boolean isKeys(String low) {
        return low.equals("prod.keys") || low.equals("title.keys") || low.equals("keys.txt");
    }

    private static boolean isContainer(String low) {
        return wanted(low) && !isKeys(low);
    }

    private static void list(ContentResolver cr, Uri tree, String docId, int depth, List<Doc> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = cr.query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 3) list(cr, tree, id, depth + 1, out);
                } else if (wanted(name)) {
                    Doc d = new Doc();
                    d.name = name;
                    d.uri = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    out.add(d);
                }
            }
        }
    }

    private void workDump(Context context, Uri tree, File dir) {
        ContentResolver cr = context.getContentResolver();
        List<ParcelFileDescriptor> open = new ArrayList<>();
        try {
            phase = "scan";
            if (!NativeImporter.load()) throw new IOException("libnximport: " + NativeImporter.loadError());
            if (!dir.isDirectory() && !dir.mkdirs())
                throw new IOException(Lang.s("Could not create the folder ", "Não foi possível criar a pasta ") + dir);
            List<Doc> docs = new ArrayList<>();
            list(cr, tree, DocumentsContract.getTreeDocumentId(tree), 0, docs);
            boolean keys = false;
            int containers = 0;
            for (Doc d : docs) {
                String low = d.name.toLowerCase(Locale.ROOT);
                keys |= low.equals("prod.keys") || low.equals("keys.txt");
                if (isContainer(low)) {
                    containers++;
                    if (containerName.isEmpty()) containerName = d.name;
                }
            }
            if (!keys)
                throw new IOException(Lang.s("prod.keys was not found in the chosen folder (or its sub-folders).",
                        "O prod.keys não foi encontrado na pasta escolhida (nem nas subpastas)."));
            keysOk = true;
            if (containers == 0)
                throw new IOException(Lang.s("No .xci or .nsp file in the chosen folder (or its sub-folders).",
                        "Nenhum arquivo .xci ou .nsp na pasta escolhida (nem nas subpastas)."));
            String[] names = new String[docs.size()];
            int[] fds = new int[docs.size()];
            for (int i = 0; i < docs.size(); i++) {
                ParcelFileDescriptor pfd = cr.openFileDescriptor(docs.get(i).uri, "r");
                if (pfd == null) throw new IOException(Lang.s("Could not open ", "Não foi possível abrir ") + docs.get(i).name);
                open.add(pfd);
                names[i] = docs.get(i).name;
                fds[i] = pfd.getFd();
            }
            int rc = NativeImporter.run(dir.getAbsolutePath(), names, fds, NativeImporter.TITLE_ID, false,
                    new NativeImporter.Callback() {
                        @Override
                        public void progress(String ph, long d, long t, String item) {
                            if (!"scan".equals(ph)) containerOk = true;
                            phase = ph;
                            done = d;
                            total = t;
                            currentName = item;
                            if (cancel.get()) NativeImporter.cancel();
                        }

                        @Override
                        public void log(String line) {
                            Log.i(AppFiles.TAG, "import: " + line);
                        }
                    });
            if (rc == 4) {
                cancelled = true;
                throw new IOException(Lang.s("Import cancelled. What was already written is kept; the next import continues from there.",
                        "Importação cancelada. O que já foi gravado fica; a próxima importação continua dali."));
            }
            if (rc != 0) throw new IOException(NativeImporter.lastError());
            installShaders(context, dir);
            AppFiles.installConfig(context);
            phase = "done";
        } catch (IOException e) {
            error = e.getMessage();
        } catch (SecurityException e) {
            error = Lang.s("No permission to read the folder: ", "Sem permissão para ler a pasta: ") + e.getMessage();
        } catch (RuntimeException e) {
            error = Lang.s("Unexpected error: ", "Erro inesperado: ") + e;
        } finally {
            for (ParcelFileDescriptor p : open) {
                try {
                    p.close();
                } catch (IOException ignored) {
                }
            }
            finish();
        }
    }

    /** assets/spirv.zip -> dir/spirv/ (the translated shaders), unless this APK's set is already there. */
    void installShaders(Context context, File dir) throws IOException {
        File spirv = new File(dir, "spirv");
        File stamp = new File(spirv, ".apk-shaders");
        String version = shadersId(context);
        if (stamp.isFile() && version.equals(read(stamp))) return;
        phase = "shaders";
        if (!spirv.isDirectory() && !spirv.mkdirs()) throw new IOException("cannot create " + spirv);
        String root = spirv.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        int n = 0;
        try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("spirv.zip"))) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String name = e.getName();
                name = name.substring(name.lastIndexOf('/') + 1);
                File out = new File(spirv, name);
                if (name.isEmpty() || !out.getCanonicalPath().startsWith(root)) continue;
                currentName = name;
                try (OutputStream os = new FileOutputStream(out)) {
                    int r;
                    while ((r = zip.read(buf)) > 0) os.write(buf, 0, r);
                }
                n++;
            }
        } catch (java.io.FileNotFoundException e) {
            Log.w(AppFiles.TAG, "import: this APK has no assets/spirv.zip");
            return;  // the launcher reports the missing shaders (AppFiles.hasShaders)
        }
        try (OutputStream os = new FileOutputStream(stamp)) {
            os.write(version.getBytes("UTF-8"));
        }
        Log.i(AppFiles.TAG, "import: " + n + " shader files installed");
    }

    static boolean apkHasShaders(Context c) {
        try (InputStream in = c.getAssets().open("spirv.zip")) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /** The id of the shader set in this APK (assets/spirv.id, written by the build script). */
    static String shadersId(Context c) {
        try (InputStream in = c.getAssets().open("spirv.id")) {
            byte[] b = new byte[128];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "unknown";
        } catch (IOException e) {
            return "unknown";
        }
    }

    /** True when this APK has shaders that are not (yet) in dir/spirv. */
    static boolean shadersMissing(Context c, File dir) {
        if (!apkHasShaders(c)) return false;
        File stamp = new File(new File(dir, "spirv"), ".apk-shaders");
        return !stamp.isFile() || !shadersId(c).equals(read(stamp));
    }

    private static String read(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
        } catch (IOException e) {
            return "";
        }
    }
}
