package io.github.belmantegu.wonderrecomp;

import java.util.Locale;

/**
 * Where the drawn touch buttons sit. The runtime draws and hit-tests them (runtime/src/touch_layout.cpp); this class
 * edits the same numbers and stores them in config.txt as NX_TOUCH_LAYOUT=A:x,y,r;B:... (x, y: 0..1 across the landscape
 * screen, r: a fraction of its height). The defaults here must equal kWonder in touch_layout.cpp.
 */
final class ControlLayout {
    static final String[] NAMES = {"A", "B", "Y", "X", "ZR", "R", "L", "PLUS", "MINUS"};
    private static final float[][] DEFAULTS = {
        {0.905f, 0.74f, 0.105f}, {0.775f, 0.86f, 0.085f}, {0.775f, 0.60f, 0.085f}, {0.905f, 0.47f, 0.075f},
        {0.955f, 0.25f, 0.055f}, {0.955f, 0.07f, 0.045f}, {0.045f, 0.07f, 0.045f}, {0.545f, 0.07f, 0.040f},
        {0.455f, 0.07f, 0.040f}};
    static final String[] LABELS = {"A", "B", "Y", "X", "ZR", "R", "L", "+", "-"};

    final float[][] b = new float[NAMES.length][3];

    static ControlLayout defaults() {
        ControlLayout l = new ControlLayout();
        for (int i = 0; i < NAMES.length; i++) l.b[i] = DEFAULTS[i].clone();
        return l;
    }

    static ControlLayout parse(String text) {
        ControlLayout l = defaults();
        if (text == null) return l;
        for (String item : text.split(";")) {
            int c = item.indexOf(':');
            if (c < 0) continue;
            String[] n = item.substring(c + 1).split(",");
            if (n.length != 3) continue;
            for (int i = 0; i < NAMES.length; i++) {
                if (!NAMES[i].equals(item.substring(0, c))) continue;
                try {
                    float x = Float.parseFloat(n[0]), y = Float.parseFloat(n[1]), r = Float.parseFloat(n[2]);
                    l.b[i] = new float[] {clamp(x), clamp(y), Math.max(0.02f, Math.min(0.2f, r))};
                } catch (NumberFormatException ignored) {
                }
            }
        }
        return l;
    }

    private static float clamp(float v) {
        return Math.max(0f, Math.min(1f, v));
    }

    String serialize() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < NAMES.length; i++) {
            if (i > 0) sb.append(';');
            sb.append(String.format(Locale.US, "%s:%.3f,%.3f,%.3f", NAMES[i], b[i][0], b[i][1], b[i][2]));
        }
        return sb.toString();
    }

    boolean isDefault() {
        return serialize().equals(defaults().serialize());
    }

    void mirror() {
        for (float[] x : b) x[0] = 1f - x[0];
    }
}
