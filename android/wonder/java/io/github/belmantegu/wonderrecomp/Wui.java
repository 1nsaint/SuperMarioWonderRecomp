package io.github.belmantegu.wonderrecomp;

import android.app.Activity;
import android.content.Context;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

import java.io.IOException;
import java.io.InputStream;

/**
 * The launcher's look (android/launcher-mockup/index.html): colours, the Fredoka font (assets/fonts, OFL), Lucide icons
 * (res/drawable/ic_*.xml) and the size unit. The mockup sizes everything in em with 1em = 1.5% of the screen width
 * (cqw) so the whole screen scales like the phone: U is 1% of the width (capped for screens wider than 19.5:9).
 */
final class Wui {
    static final int INK = Color.parseColor("#2b2d33");
    static final int INK2 = Color.parseColor("#44464e");
    static final int CREAM = Color.parseColor("#fffbea");
    static final int CREAM2 = Color.parseColor("#f3ecd2");
    static final int RED = Color.parseColor("#e8262b");
    static final int BLUE = Color.parseColor("#2aa8ff");
    static final int PINK = Color.parseColor("#ff4fa8");
    static final int GREEN = Color.parseColor("#3dbb4a");
    static final int YELLOW = Color.parseColor("#ffd23a");
    static final int MUTED = Color.parseColor("#6d6f78");

    private static Typeface semi, bold, medium;

    /** 1% of the screen width in px, landscape. */
    static float u;

    private Wui() {
    }

    static void init(Context c) {
        DisplayMetrics m = c.getResources().getDisplayMetrics();
        float w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);
        u = Math.min(w, h * 19.5f / 9f) / 100f;
        if (bold == null) {
            AssetManager a = c.getAssets();
            medium = font(a, 500, Typeface.DEFAULT);
            semi = font(a, 600, Typeface.DEFAULT_BOLD);
            bold = font(a, 700, Typeface.DEFAULT_BOLD);
        }
    }

    private static Typeface font(AssetManager a, int weight, Typeface fallback) {
        try {
            return new Typeface.Builder(a, "fonts/Fredoka.ttf").setFontVariationSettings("'wght' " + weight).build();
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    /** em of the mockup's screens (font-size 1.5cqw) in px. */
    static float em(float k) {
        return k * 1.5f * u;
    }

    static Typeface medium() {
        return medium;
    }

    static Typeface semi() {
        return semi;
    }

    static Typeface bold() {
        return bold;
    }

    static int alpha(int color, float a) {
        return Color.argb(Math.round(a * 255), Color.red(color), Color.green(color), Color.blue(color));
    }

    /** A Lucide icon (res/drawable/ic_<name>.xml), tinted. */
    static Drawable icon(Context c, String name, int tint) {
        int id = c.getResources().getIdentifier("ic_" + name.replace('-', '_'), "drawable", c.getPackageName());
        if (id == 0) return null;
        Drawable d = c.getDrawable(id).mutate();
        d.setTint(tint);
        return d;
    }

    static Bitmap asset(Context c, String path) {
        try (InputStream in = c.getAssets().open(path)) {
            return BitmapFactory.decodeStream(in);
        } catch (IOException e) {
            return null;
        }
    }

    static void hideSystemBars(Activity a) {
        Window w = a.getWindow();
        w.setDecorFitsSystemWindows(false);
        WindowInsetsController c = w.getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    static void toast(Context c, String text) {
        Toast.makeText(c, text, Toast.LENGTH_LONG).show();
    }

    static void press(View v) {
        v.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
    }
}
