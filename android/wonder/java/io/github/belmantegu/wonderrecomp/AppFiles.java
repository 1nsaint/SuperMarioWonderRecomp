package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;

/**
 * Where things are. The game's data folder is the app's external files folder, which platform_android.cpp reads
 * when it has analysis/main.bin (no storage permission needed):
 *   analysis/{main,sdk}.bin, main.npdm   the modules (importer)
 *   extract/romfs/...                    the RomFS (importer)
 *   spirv/*.spv, results.tsv             translated shaders (assets/spirv.zip of this APK)
 *   user/save, user/cache*               saves
 *   config.txt                           NX_* switches (become environment variables) and a few plain keys
 *   nx.log, nx.log.prev                  the runtime's log
 * app.txt (internal files) holds the launcher's own settings: language, FPS counter, advanced rows.
 */
final class AppFiles {
    static final String TAG = "wonder";

    private AppFiles() {
    }

    static File dataDir(Context c) {
        File d = c.getExternalFilesDir(null);
        return d != null ? d : c.getFilesDir();
    }

    static File configFile(Context c) {
        return new File(dataDir(c), "config.txt");
    }

    static File appFile(Context c) {
        return new File(c.getFilesDir(), "app.txt");
    }

    static boolean hasAnalysis(Context c) {
        File a = new File(dataDir(c), "analysis");
        return new File(a, "main.bin").isFile() && new File(a, "sdk.bin").isFile();
    }

    static boolean hasRomfs(Context c) {
        String[] list = new File(dataDir(c), "extract/romfs").list();
        return list != null && list.length > 0;
    }

    static boolean hasShaders(Context c) {
        return new File(dataDir(c), "spirv/results.tsv").isFile();
    }

    /** True when this APK carries the game library (a launcher-only build does not). */
    static boolean hasGameLib(Context c) {
        return new File(c.getApplicationInfo().nativeLibraryDir, "libwonder.so").isFile();
    }

    static boolean ready(Context c) {
        return hasAnalysis(c) && hasRomfs(c) && hasShaders(c);
    }

    /** Files and bytes under analysis, extract and spirv. */
    static void tally(File dir, long[] out) {
        for (String n : new String[] {"analysis", "extract", "spirv"}) count(new File(dir, n), out);
    }

    private static void count(File f, long[] out) {
        File[] list = f.listFiles();
        if (list == null) return;
        for (File x : list) {
            if (x.isDirectory()) {
                count(x, out);
            } else {
                out[0]++;
                out[1] += x.length();
            }
        }
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File x : list) deleteTree(x);
        f.delete();
    }

    static String getApp(Context c, String key) {
        return GameConfig.load(appFile(c)).get(key);
    }

    static synchronized void setApp(Context c, String key, String value) {
        GameConfig g = GameConfig.load(appFile(c));
        g.set(key, value);
        try {
            g.save(appFile(c));
        } catch (IOException e) {
            Log.w(TAG, "app.txt: " + e);
        }
    }

    /** config.txt for the first run: from assets/config.txt when there is one, else the one-line phone default. */
    static void installConfig(Context c) throws IOException {
        File cfg = configFile(c);
        if (cfg.exists()) return;
        File dir = cfg.getParentFile();
        if (dir != null) dir.mkdirs();
        GameConfig g = new GameConfig();
        try (java.io.InputStream in = c.getAssets().open("config.txt")) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int r;
            while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
            g = GameConfig.parse(new String(out.toByteArray(), "UTF-8"));
        } catch (java.io.FileNotFoundException e) {
            g.set("NX_HANDHELD", "1");
        }
        g.save(cfg);
    }

    /**
     * fonts/shared_5.ttf: the stand-in for the Switch's controller-button font (tools/fonts/make_button_font.py on
     * Fredoka, OFL), which the runtime looks for first (hle_pl.cpp). Never replaces a file that is already there, so a
     * font the player put in by hand wins.
     */
    static void installSharedFont(Context c) {
        File font = new File(dataDir(c), "fonts/shared_5.ttf");
        if (font.isFile() && font.length() > 0) return;
        File dir = font.getParentFile();
        if (dir != null) dir.mkdirs();
        File tmp = new File(dir, "shared_5.ttf.part");
        try (java.io.InputStream in = c.getAssets().open("fonts/shared_5.ttf");
             java.io.FileOutputStream out = new java.io.FileOutputStream(tmp)) {
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
            out.getFD().sync();
        } catch (IOException e) {
            Log.w(TAG, "shared_5.ttf: " + e);
            tmp.delete();
            return;
        }
        if (!tmp.renameTo(font)) tmp.delete();
    }

    /** The old Home screen wrote data_root.txt; a stale one would send the runtime to another folder. */
    static void dropLegacyDataRoot(Context c) {
        new File(c.getFilesDir(), "data_root.txt").delete();
    }
}
