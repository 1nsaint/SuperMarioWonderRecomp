package io.github.belmantegu.wonderrecomp;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.drawable.ColorDrawable;
import android.text.method.LinkMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** The cream card of the mockup (.card): thin outline, hard shadow underneath; and dialogs built on it. */
final class WCard extends LinearLayout {
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();
    private final float em;
    private float shadowEm = 0.25f, borderEm = 0.2f, radiusEm = 1.5f;
    private int fill = Wui.CREAM;

    WCard(Context c) {
        super(c);
        em = Wui.em(1);
        setOrientation(VERTICAL);
        setWillNotDraw(false);
        int padY = Math.round(em * 1.6f), padX = Math.round(em * 1.8f);
        setPadding(padX, padY, padX, padY + Math.round(em * 0.25f));
    }

    /** The options list panel (.list): translucent white, thinner shadow, smaller corners. */
    static WCard panel(Context c) {
        WCard p = new WCard(c);
        p.shadowEm = 0.2f;
        p.borderEm = 0.18f;
        p.radiusEm = 1.2f;
        p.fill = 0xAAFFFFFF;
        int pad = Math.round(p.em * 0.9f);
        p.setPadding(pad, pad, pad, pad + Math.round(p.em * 0.2f));
        return p;
    }

    private final android.graphics.Path face = new android.graphics.Path();

    @Override
    protected void onDraw(Canvas c) {
        float sh = em * shadowEm, b = em * borderEm, rad = em * radiusEm;
        float w = getWidth(), h = getHeight() - sh;
        face.reset();
        r.set(0, 0, w, h);
        face.addRoundRect(r, rad, rad, android.graphics.Path.Direction.CW);
        // hard shadow: only the sliver below the face (the face may be translucent)
        c.save();
        c.clipOutPath(face);
        p.setStyle(Paint.Style.FILL);
        p.setColor(Wui.INK);
        r.set(0, sh, w, h + sh);
        c.drawRoundRect(r, rad, rad, p);
        c.restore();
        p.setColor(fill);
        r.set(0, 0, w, h);
        c.drawRoundRect(r, rad, rad, p);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(b);
        p.setColor(Wui.INK);
        r.inset(b / 2, b / 2);
        c.drawRoundRect(r, rad - b / 2, rad - b / 2, p);
        p.setStyle(Paint.Style.FILL);
    }

    static TextView title(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTypeface(Wui.bold());
        t.setTextColor(Wui.INK);
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Wui.em(1.6f));
        return t;
    }

    static TextView body(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTypeface(Wui.medium());
        t.setTextColor(0xFF43454D);
        t.setLineSpacing(0, 1.12f);
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Wui.em(0.95f));
        t.setMovementMethod(LinkMovementMethod.getInstance());
        return t;
    }

    static final class Action {
        final String label;
        final WButton.Style style;
        final Runnable run;

        Action(String label, WButton.Style style, Runnable run) {
            this.label = label;
            this.style = style;
            this.run = run;
        }
    }

    /** A modal card over a dim backdrop. body: text (shown scrollable) or null; extra: an extra view under the text. */
    static Dialog show(Activity a, String title, String bodyText, View extra, Action... actions) {
        final Dialog d = new Dialog(a, android.R.style.Theme_Material_NoActionBar_Fullscreen);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(0x992b2d33));
        w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        FrameLayout root = new FrameLayout(a);
        WCard card = new WCard(a);
        float em = Wui.em(1);
        card.addView(title(a, title));
        if (bodyText != null) {
            ScrollView sv = new ScrollView(a);
            sv.setVerticalScrollBarEnabled(true);
            TextView b = body(a, bodyText);
            b.setTextIsSelectable(true);
            sv.addView(b);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, 0, 1f);
            lp.topMargin = Math.round(em);
            card.addView(sv, lp);
        }
        if (extra != null) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.topMargin = Math.round(em);
            card.addView(extra, lp);
        }
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < actions.length; i++) {
            final Action act = actions[i];
            WButton bt = new WButton(a, act.style, Wui.em(1.05f), true).text(act.label, null);
            bt.setOnClickListener(v -> {
                d.dismiss();
                if (act.run != null) act.run.run();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1f);
            if (i > 0) lp.leftMargin = Math.round(em * 0.9f);
            row.addView(bt, lp);
        }
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(-1, -2);
        rl.topMargin = Math.round(em * 1.1f);
        card.addView(row, rl);
        android.util.DisplayMetrics m = a.getResources().getDisplayMetrics();
        int width = Math.min(Math.round(em * 44f), Math.round(m.widthPixels * 0.9f));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        root.addView(card, lp);
        root.setOnClickListener(null);
        d.setContentView(root);
        d.setCancelable(true);
        d.show();
        w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        // the card may not be taller than the screen
        card.setMinimumHeight(0);
        card.post(() -> {
            int maxH = Math.round(m.heightPixels * 0.9f);
            if (card.getHeight() > maxH) {
                ViewGroup.LayoutParams l = card.getLayoutParams();
                l.height = maxH;
                card.setLayoutParams(l);
            }
        });
        return d;
    }

    static void alert(Activity a, String title, String text) {
        show(a, title, text, null, new Action("OK", WButton.Style.RED, null));
    }

    static void confirm(Activity a, String title, String text, String yes, String no, Runnable onYes) {
        show(a, title, text, null, new Action(yes, WButton.Style.RED, onYes), new Action(no, WButton.Style.GHOST, null));
    }
}
