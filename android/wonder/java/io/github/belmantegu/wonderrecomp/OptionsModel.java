package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads and writes the option rows (Props) against config.txt and app.txt. The runtime applies the first line of a key,
 * and a row left at the runtime's own default has no line at all (so the runtime's defaults keep applying); rows marked
 * explicit are always written. Pure logic behind the Backend interface, so it runs in a plain JVM test.
 */
final class OptionsModel {
    interface Backend {
        String cfg(String key);

        void setCfg(String key, String value);

        String app(String key);

        void setApp(String key, String value);
    }

    private final Backend b;
    private final List<Props.Prop> props = Props.all();

    OptionsModel(Backend b) {
        this.b = b;
    }

    List<Props.Prop> props() {
        return props;
    }

    Props.Prop find(String id) {
        return Props.find(props, id);
    }

    boolean advancedOn() {
        return "1".equals(b.app("ui_advanced"));
    }

    /** The rows of a tab that are shown now (advanced rows and rows whose toggle is off are hidden). */
    List<Props.Prop> rows(String tab) {
        List<Props.Prop> out = new ArrayList<>();
        boolean adv = advancedOn();
        for (Props.Prop p : props) {
            if (!p.tab.equals(tab)) continue;
            if (p.advanced && !adv) continue;
            if (p.requires != null) {
                Props.Prop r = find(p.requires);
                if (r == null || index(r) != 1) continue;
            }
            out.add(p);
        }
        return out;
    }

    private String read(Props.Prop p) {
        return p.store == Props.Store.CONFIG ? b.cfg(p.key) : p.store == Props.Store.APP ? b.app(p.key) : null;
    }

    /** The stored text of the row, or null when there is no line. */
    String raw(Props.Prop p) {
        return read(p);
    }

    /** Index of the current choice (toggles: 0 off, 1 on). A line with a value the row does not know shows the default. */
    int index(Props.Prop p) {
        String v = read(p);
        int i = p.indexOf(v);
        if (i >= 0) return i;
        i = p.indexOf(p.dflt);
        return i >= 0 ? i : 0;
    }

    void setIndex(Props.Prop p, int i) {
        String v = p.values[i];
        boolean isDefault = v == null || (!p.explicit && v.equals(p.dflt));
        write(p, isDefault ? null : v);
    }

    int slider(Props.Prop p) {
        String v = read(p);
        if (v != null) {
            try {
                return Math.max(p.min, Math.min(p.max, Integer.parseInt(v.trim())));
            } catch (NumberFormatException ignored) {
            }
        }
        return p.sliderDefault;
    }

    void setSlider(Props.Prop p, int v) {
        write(p, v == p.sliderDefault ? null : String.valueOf(v));
    }

    private void write(Props.Prop p, String v) {
        if (p.store == Props.Store.CONFIG) b.setCfg(p.key, v);
        else if (p.store == Props.Store.APP) b.setApp(p.key, v);
    }

    String label(Props.Prop p, int i) {
        return Lang.portuguese() ? p.pt[i] : p.en[i];
    }

    String[] labels(Props.Prop p) {
        return Lang.portuguese() ? p.pt : p.en;
    }

    /** config.txt and app.txt lines of the rows that differ from their defaults, for the diagnostics file. */
    String summary() {
        StringBuilder sb = new StringBuilder();
        for (Props.Prop p : props) {
            if (p.key == null) continue;
            String v = read(p);
            if (v == null) continue;
            if (sb.length() > 0) sb.append(' ');
            sb.append(p.key).append('=').append(v);
        }
        return sb.length() == 0 ? "defaults" : sb.toString();
    }

    /** The files: config.txt in the data folder (created from the APK's default on first write), app.txt internal. */
    static final class FileBackend implements Backend {
        private final Context c;

        FileBackend(Context c) {
            this.c = c.getApplicationContext() != null ? c.getApplicationContext() : c;
        }

        @Override
        public String cfg(String key) {
            return GameConfig.load(AppFiles.configFile(c)).get(key);
        }

        @Override
        public void setCfg(String key, String value) {
            File f = AppFiles.configFile(c);
            try {
                if (!f.isFile()) AppFiles.installConfig(c);  // options changed before the import: start from the default
                GameConfig g = GameConfig.load(f);
                g.set(key, value);
                g.save(f);
            } catch (IOException e) {
                Log.w(AppFiles.TAG, "config.txt: " + e);
            }
        }

        @Override
        public String app(String key) {
            return AppFiles.getApp(c, key);
        }

        @Override
        public void setApp(String key, String value) {
            AppFiles.setApp(c, key, value);
        }
    }
}
