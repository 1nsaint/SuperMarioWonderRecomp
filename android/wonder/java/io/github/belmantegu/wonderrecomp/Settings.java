package io.github.belmantegu.wonderrecomp;

import android.app.ActivityManager;
import android.content.Context;

/** Small readers over app.txt and the system for the launcher's own settings. */
final class Settings {
    private Settings() {
    }

    static void applyLanguage(Context c) {
        String l = AppFiles.getApp(c, "lang");
        Lang.set(l == null ? Lang.AUTO : l);
    }

    static long totalRamMb(Context c) {
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return 0;
        am.getMemoryInfo(mi);
        return mi.totalMem >> 20;
    }
}
