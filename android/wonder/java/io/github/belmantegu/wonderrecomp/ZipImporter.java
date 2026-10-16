package io.github.belmantegu.wonderrecomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * A data pack made earlier (tools/pack_phone_wonder.py: the folder "Super Mario Bros. Wonder/" with analysis/, extract/,
 * spirv/, ...), copied as it is. Files already there with the same size are skipped, so an interrupted copy continues.
 * Only the folders the runtime reads are accepted; paths cannot leave the data folder.
 */
final class ZipImporter {
    static final class Cancelled extends IOException {
    }

    static final class Result {
        int files, copied, skipped;
    }

    interface Listener {
        void onProgress(long bytes, int files, String name);
    }

    private static final String[] TOP = {"analysis/", "extract/", "spirv/", "fonts/", "user/"};

    private ZipImporter() {
    }

    static void cleanParts(File dir) {
        File[] list = dir.listFiles();
        if (list != null) for (File f : list) if (f.getName().endsWith(".part")) f.delete();
    }

    private static String wanted(String name) {
        name = name.replace('\\', '/');
        int slash = name.indexOf('/');
        String rest = name;
        for (int guard = 0; guard < 2; guard++) {
            for (String t : TOP) if (rest.startsWith(t)) return rest;
            if (rest.equals("config.txt")) return null;  // the player's own config.txt is never overwritten
            slash = rest.indexOf('/');
            if (slash < 0) return null;
            rest = rest.substring(slash + 1);  // the pack's top folder
        }
        return null;
    }

    static Result run(InputStream raw, File dir, Listener l, AtomicBoolean cancel) throws IOException {
        Result r = new Result();
        String root = dir.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        long bytes = 0;
        try (ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                if (cancel.get()) throw new Cancelled();
                if (e.isDirectory()) continue;
                String rel = wanted(e.getName());
                if (rel == null) continue;
                File out = new File(dir, rel);
                if (!out.getCanonicalPath().startsWith(root)) continue;
                r.files++;
                if (e.getSize() >= 0 && out.isFile() && out.length() == e.getSize()) {
                    r.skipped++;
                    bytes += e.getSize();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                    throw new IOException(Lang.s("Could not create ", "Não foi possível criar ") + parent);
                File part = new File(out.getPath() + ".part");
                try (OutputStream os = new FileOutputStream(part)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        bytes += n;
                        if (cancel.get()) throw new Cancelled();
                    }
                }
                out.delete();
                if (!part.renameTo(out)) throw new IOException("rename failed: " + out);
                r.copied++;
                if ((r.files & 31) == 0) l.onProgress(bytes, r.files, rel);
            }
        }
        l.onProgress(bytes, r.files, "");
        return r;
    }
}
