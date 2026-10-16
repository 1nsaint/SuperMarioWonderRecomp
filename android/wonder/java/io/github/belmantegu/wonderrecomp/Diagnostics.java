package io.github.belmantegu.wonderrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Process;
import android.util.DisplayMetrics;
import android.view.Display;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Date;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** The diagnostics zip (device, settings, logs, exit reasons) and the "last session ended unexpectedly" marker. */
final class Diagnostics {
    private static final long TAIL = 2L << 20;
    private static final int LOGCAT = 4 << 20;

    private Diagnostics() {
    }

    private static File marker(Context c) {
        return new File(c.getFilesDir(), "session.marker");
    }

    static void sessionBegin(Context c) {
        try (FileWriter w = new FileWriter(marker(c))) {
            w.write(Process.myPid() + ";" + System.currentTimeMillis());
        } catch (IOException ignored) {
        }
    }

    static void sessionEnd(Context c) {
        marker(c).delete();
    }

    static boolean lastEndedAbnormally(Context c) {
        File m = marker(c);
        if (!m.isFile()) return false;
        try (BufferedReader r = new BufferedReader(new FileReader(m))) {
            String line = r.readLine();
            return !new File("/proc/" + Integer.parseInt(line.substring(0, line.indexOf(';')))).exists();
        } catch (IOException | RuntimeException e) {
            return true;
        }
    }

    static void write(Context c, OutputStream out) throws IOException {
        File dir = AppFiles.dataDir(c);
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            put(z, "device.txt", device(c));
            put(z, "settings.txt", "data=" + dir + (AppFiles.ready(c) ? "" : " (not ready)") + "\n[config.txt]\n"
                    + tailText(new File(dir, "config.txt"), 16384) + "\n[app.txt]\n" + tailText(AppFiles.appFile(c), 16384));
            tail(z, new File(dir, "nx.log"), "nx.log", TAIL);
            tail(z, new File(dir, "nx.log.prev"), "nx.log.prev", TAIL);
            put(z, "logcat.txt", logcat());
            put(z, "exit_info.txt", exits(c));
        }
    }

    private static void put(ZipOutputStream z, String name, String text) throws IOException {
        z.putNextEntry(new ZipEntry(name));
        z.write(text.getBytes(StandardCharsets.UTF_8));
        z.closeEntry();
    }

    private static void tail(ZipOutputStream z, File f, String name, long max) throws IOException {
        if (!f.isFile()) return;
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            r.seek(Math.max(0, r.length() - max));
            z.putNextEntry(new ZipEntry(name));
            byte[] buf = new byte[65536];
            int n;
            while ((n = r.read(buf)) > 0) z.write(buf, 0, n);
            z.closeEntry();
        }
    }

    /** The last `max` bytes of a text file ("" when there is none). */
    static String tailText(File f, long max) {
        if (f == null || !f.isFile()) return "";
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = Math.min(max, r.length());
            r.seek(r.length() - n);
            byte[] b = new byte[(int) n];
            r.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    static String device(Context c) {
        StringBuilder sb = new StringBuilder();
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            sb.append("app=").append(c.getPackageName()).append(' ').append(pi.versionName).append(" (")
                    .append(pi.getLongVersionCode()).append(")\n");
        } catch (PackageManager.NameNotFoundException e) {
            sb.append("app=?\n");
        }
        sb.append("manufacturer=").append(Build.MANUFACTURER).append("\nmodel=").append(Build.MODEL).append("\ndevice=")
                .append(Build.DEVICE).append("\nhardware=").append(Build.HARDWARE).append("\nboard=").append(Build.BOARD)
                .append("\nandroid=").append(Build.VERSION.RELEASE).append(" (sdk ").append(Build.VERSION.SDK_INT)
                .append(")\nfingerprint=").append(Build.FINGERPRINT).append('\n');
        sb.append("soc=").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        sb.append("abis=").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            sb.append("ram_mb=").append(mi.totalMem >> 20).append(" avail_mb=").append(mi.availMem >> 20).append('\n');
        }
        sb.append("cpus=").append(Runtime.getRuntime().availableProcessors()).append('\n');
        DisplayMetrics dm = c.getResources().getDisplayMetrics();
        sb.append("screen=").append(dm.widthPixels).append('x').append(dm.heightPixels).append(" density=").append(dm.densityDpi).append('\n');
        Display d = c.getDisplay() != null ? c.getDisplay() : null;
        if (d != null) for (Display.Mode m : d.getSupportedModes())
            sb.append("display_mode=").append(m.getModeId()).append(' ').append(m.getPhysicalWidth()).append('x')
                    .append(m.getPhysicalHeight()).append('@').append(m.getRefreshRate()).append('\n');
        sb.append("\n[gpu]\n").append(vulkanLines(c));
        for (String k : new String[] {"ro.hardware.vulkan", "ro.hardware.egl", "ro.board.platform", "ro.soc.model"})
            sb.append(k).append('=').append(getprop(k)).append('\n');
        sb.append("(the Vulkan device name is in nx.log: \"gpu:\" and \"vk:\")\n");
        return sb.toString();
    }

    /** android.hardware.vulkan.* features as the system reports them, with the version decoded. */
    static String vulkanLines(Context c) {
        StringBuilder sb = new StringBuilder();
        for (FeatureInfo f : c.getPackageManager().getSystemAvailableFeatures()) {
            if (f.name == null || !f.name.startsWith("android.hardware.vulkan")) continue;
            sb.append(f.name).append('=').append(f.version);
            if (f.name.endsWith("version"))
                sb.append(" (Vulkan ").append(f.version >>> 22).append('.').append((f.version >>> 12) & 0x3ff).append(')');
            sb.append('\n');
        }
        return sb.length() == 0 ? "no android.hardware.vulkan.* feature reported\n" : sb.toString();
    }

    private static String getprop(String key) {
        try {
            java.lang.Process p = new ProcessBuilder("getprop", key).redirectErrorStream(true).start();
            try (InputStream in = p.getInputStream()) {
                byte[] b = new byte[512];
                int n = in.read(b);
                p.waitFor(1, TimeUnit.SECONDS);
                return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : "";
            }
        } catch (IOException | InterruptedException e) {
            return "?";
        }
    }

    private static String logcat() {
        try {
            java.lang.Process p = new ProcessBuilder("logcat", "-d", "-v", "time", "-s", "wonder:*", "nximport:*", "AndroidRuntime:*",
                    "DEBUG:*", "libc:*").redirectErrorStream(true).start();
            StringBuilder sb = new StringBuilder();
            try (InputStream in = p.getInputStream()) {
                byte[] b = new byte[65536];
                int n;
                while ((n = in.read(b)) > 0 && sb.length() < LOGCAT) sb.append(new String(b, 0, n, StandardCharsets.UTF_8));
            }
            p.waitFor(3, TimeUnit.SECONDS);
            return sb.length() > LOGCAT ? sb.substring(sb.length() - LOGCAT) : sb.toString();
        } catch (IOException | InterruptedException e) {
            return "logcat unavailable: " + e + "\n";
        }
    }

    static String exits(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        StringBuilder sb = new StringBuilder();
        if (am == null) return "";
        for (ApplicationExitInfo i : am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 6))
            sb.append(new Date(i.getTimestamp())).append(" process=").append(i.getProcessName()).append(" reason=")
                    .append(i.getReason()).append(" status=").append(i.getStatus()).append(" importance=")
                    .append(i.getImportance()).append(' ').append(i.getDescription()).append('\n');
        return sb.toString();
    }
}
