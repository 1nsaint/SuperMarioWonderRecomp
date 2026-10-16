package io.github.belmantegu.wonderrecomp;

import java.util.Locale;

/** English / Portuguese text: Lang.s(en, pt). The mode comes from app.txt (lang=auto|en|pt). */
final class Lang {
    static final String AUTO = "auto";
    static final String EN = "en";
    static final String PT = "pt";
    private static volatile String mode = AUTO;

    private Lang() {
    }

    static void set(String m) {
        mode = EN.equals(m) || PT.equals(m) ? m : AUTO;
    }

    static boolean portuguese() {
        if (PT.equals(mode)) return true;
        if (EN.equals(mode)) return false;
        return PT.equals(Locale.getDefault().getLanguage());
    }

    static String s(String en, String pt) {
        return portuguese() ? pt : en;
    }
}
