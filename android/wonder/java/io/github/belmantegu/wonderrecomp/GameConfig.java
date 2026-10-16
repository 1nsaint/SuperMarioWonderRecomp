package io.github.belmantegu.wonderrecomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * A key=value text file kept line by line (config.txt, app.txt). The runtime applies the FIRST line of a key (it sets
 * environment variables without overriding), so set() rewrites that line and drops later duplicates; lines the
 * launcher does not know (comments, NX_* switches the player added by hand) are kept as they are.
 */
final class GameConfig {
    private final List<String> lines = new ArrayList<>();

    static GameConfig parse(String text) {
        GameConfig c = new GameConfig();
        if (text == null || text.isEmpty()) return c;
        String[] parts = text.split("\n", -1);
        int n = parts.length;
        if (n > 0 && parts[n - 1].isEmpty()) n--;
        for (int i = 0; i < n; i++) c.lines.add(stripEnd(parts[i]));
        return c;
    }

    private static String stripEnd(String s) {
        int n = s.length();
        while (n > 0 && (s.charAt(n - 1) == '\r' || s.charAt(n - 1) == ' ')) n--;
        return s.substring(0, n);
    }

    private static boolean isKey(String line, String key) {
        int eq = line.indexOf('=');
        return eq > 0 && line.substring(0, eq).equals(key);
    }

    private int find(String key) {
        for (int i = 0; i < lines.size(); i++) if (isKey(lines.get(i), key)) return i;
        return -1;
    }

    /** The value of the first line of the key, or null. */
    String get(String key) {
        int i = find(key);
        return i < 0 ? null : lines.get(i).substring(lines.get(i).indexOf('=') + 1);
    }

    /** value null removes every line of the key. */
    void set(String key, String value) {
        if (value == null) {
            for (int i = lines.size() - 1; i >= 0; i--) if (isKey(lines.get(i), key)) lines.remove(i);
            return;
        }
        int i = find(key);
        if (i < 0) {
            lines.add(key + "=" + value);
            return;
        }
        lines.set(i, key + "=" + value);
        for (int j = lines.size() - 1; j > i; j--) if (isKey(lines.get(j), key)) lines.remove(j);
    }

    String toText() {
        StringBuilder sb = new StringBuilder();
        for (String l : lines) sb.append(l).append('\n');
        return sb.toString();
    }

    static GameConfig load(File f) {
        if (f == null || !f.isFile()) return new GameConfig();
        try (FileInputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int r;
            while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
            return parse(new String(out.toByteArray(), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return new GameConfig();
        }
    }

    void save(File f) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(toText().getBytes(StandardCharsets.UTF_8));
        }
        if (tmp.renameTo(f)) return;
        f.delete();
        if (!tmp.renameTo(f)) throw new IOException("rename failed: " + f);
    }
}
