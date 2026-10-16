package io.github.belmantegu.wonderrecomp;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

/**
 * Drag the touch buttons where you want them (the runtime draws them there: NX_TOUCH_LAYOUT in config.txt). Tap a
 * button to select it, drag to move, - and + to resize. Positions are fractions of the landscape screen, as the runtime
 * reads them.
 */
public class LayoutEditorActivity extends Activity {
    private ControlLayout layout;
    private EditorView view;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Settings.applyLanguage(this);
        Wui.init(this);
        layout = ControlLayout.parse(new OptionsModel.FileBackend(this).cfg("NX_TOUCH_LAYOUT"));
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Wui.CREAM);
        root.addView(new LauncherBackground(this), new FrameLayout.LayoutParams(-1, -1));
        View veil = new View(this);
        veil.setBackgroundColor(0xCCFFFBEA);
        root.addView(veil, new FrameLayout.LayoutParams(-1, -1));
        view = new EditorView(this);
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));

        float fs = Wui.em(0.95f), em = Wui.em(1);
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER);
        WButton minus = new WButton(this, WButton.Style.CREAM, fs, true).text("−", null);
        WButton plus = new WButton(this, WButton.Style.CREAM, fs, true).text("+", null);
        WButton reset = new WButton(this, WButton.Style.CREAM, fs, true).icon("rotate-ccw").text(Lang.s("Reset", "Restaurar"), null);
        WButton save = new WButton(this, WButton.Style.RED, fs, true).icon("check").text(Lang.s("Save", "Salvar"), null);
        WButton cancel = new WButton(this, WButton.Style.GHOST, fs, true).icon("x").text(Lang.s("Cancel", "Cancelar"), null);
        minus.setOnClickListener(v -> view.resize(-0.005f));
        plus.setOnClickListener(v -> view.resize(0.005f));
        reset.setOnClickListener(v -> {
            layout = ControlLayout.defaults();
            view.invalidate();
        });
        save.setOnClickListener(v -> {
            new OptionsModel.FileBackend(this).setCfg("NX_TOUCH_LAYOUT", layout.isDefault() ? null : layout.serialize());
            Wui.toast(this, Lang.s("Layout saved; it applies the next time the game starts", "Layout salvo; vale na próxima vez que o jogo iniciar"));
            finish();
        });
        cancel.setOnClickListener(v -> finish());
        for (WButton w : new WButton[] {minus, plus, reset, save, cancel}) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = Math.round(em * 0.4f);
            lp.rightMargin = Math.round(em * 0.4f);
            bar.addView(w, lp);
        }
        root.addView(bar, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        setContentView(root);
        Wui.hideSystemBars(this);
    }

    @Override
    public void onWindowFocusChanged(boolean f) {
        super.onWindowFocusChanged(f);
        if (f) Wui.hideSystemBars(this);
    }

    private final class EditorView extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int sel = 0, drag = -1;

        EditorView(Context c) {
            super(c);
            p.setTypeface(Wui.bold());
        }

        void resize(float d) {
            float[] b = layout.b[sel];
            b[2] = Math.max(0.03f, Math.min(0.15f, b[2] + d));
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            for (int i = 0; i < ControlLayout.NAMES.length; i++) {
                float[] b = layout.b[i];
                float cx = b[0] * w, cy = b[1] * h, r = b[2] * h;
                p.setStyle(Paint.Style.FILL);
                p.setColor(i == sel ? Wui.alpha(Wui.RED, 0.85f) : Wui.alpha(Wui.INK, 0.55f));
                c.drawCircle(cx, cy, r, p);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(Wui.em(0.18f));
                p.setColor(Wui.INK);
                c.drawCircle(cx, cy, r, p);
                p.setStyle(Paint.Style.FILL);
                p.setColor(0xFFFFFFFF);
                p.setTextAlign(Paint.Align.CENTER);
                p.setTextSize(Math.max(r * 0.8f, Wui.em(0.9f)));
                c.drawText(ControlLayout.LABELS[i], cx, cy - (p.ascent() + p.descent()) / 2, p);
            }
            // the stick's zone, for orientation
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(Wui.em(0.12f));
            p.setColor(Wui.alpha(Wui.INK, 0.35f));
            boolean mirrored = "1".equals(new OptionsModel.FileBackend(LayoutEditorActivity.this).cfg("NX_TOUCH_MIRROR"));
            c.drawCircle((mirrored ? 0.85f : 0.15f) * w, 0.72f * h, 0.12f * h, p);
            p.setStyle(Paint.Style.FILL);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            float w = getWidth(), h = getHeight();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    drag = -1;
                    float best = 1e9f;
                    for (int i = 0; i < ControlLayout.NAMES.length; i++) {
                        float[] b = layout.b[i];
                        float d = (float) Math.hypot(e.getX() - b[0] * w, e.getY() - b[1] * h) / (b[2] * h);
                        if (d < 1.4f && d < best) {
                            best = d;
                            drag = i;
                        }
                    }
                    if (drag >= 0) sel = drag;
                    invalidate();
                    return true;
                }
                case MotionEvent.ACTION_MOVE:
                    if (drag >= 0) {
                        layout.b[drag][0] = Math.max(0.02f, Math.min(0.98f, e.getX() / w));
                        layout.b[drag][1] = Math.max(0.02f, Math.min(0.98f, e.getY() / h));
                        invalidate();
                    }
                    return true;
                default:
                    drag = -1;
                    return true;
            }
        }
    }
}
