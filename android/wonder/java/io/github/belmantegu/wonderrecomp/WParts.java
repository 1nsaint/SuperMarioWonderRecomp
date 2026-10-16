package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Small pieces of the mockup: the status pill, the round badge, the progress bar, the numbered step rows. */
final class WParts {
    private WParts() {
    }

    /** .pill: a status line with a coloured dot. */
    static final class Pill extends View {
        private final float em;
        private String text = "";
        private int dot = Wui.GREEN;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();

        Pill(Context c, float em) {
            super(c);
            this.em = em;
            p.setTypeface(Wui.semi());
            setClickable(true);
            setFocusable(true);
            setDefaultFocusHighlightEnabled(false);
        }

        void set(String text, int dotColor) {
            this.text = text;
            this.dot = dotColor;
            requestLayout();
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            p.setTextSize(em * 0.85f);
            float textW = p.measureText(text);
            float width = textW + em * 0.85f * (1f + 0.5f + 0.7f) + em * 0.4f;
            int max = MeasureSpec.getMode(w) == MeasureSpec.UNSPECIFIED ? Integer.MAX_VALUE : MeasureSpec.getSize(w);
            setMeasuredDimension(Math.min(max, Math.round(width)), Math.round(em * 0.85f * 2.2f + em * 0.12f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float f = em * 0.85f, b = em * 0.2f, sh = em * 0.12f, h = getHeight() - sh, w = getWidth();
            p.setStyle(Paint.Style.FILL);
            p.setColor(Wui.INK);
            r.set(0, sh, w, h + sh);
            c.drawRoundRect(r, h, h, p);
            r.set(0, 0, w, h);
            c.drawRoundRect(r, h, h, p);
            r.inset(b, b);
            p.setColor(Wui.CREAM);
            c.drawRoundRect(r, h, h, p);
            float dotR = f * 0.35f, cx = b + f * 1.0f, cy = h / 2;
            p.setColor(Wui.alpha(dot, 0.27f));
            c.drawCircle(cx, cy, dotR + f * 0.18f, p);
            p.setColor(dot);
            c.drawCircle(cx, cy, dotR, p);
            p.setColor(Wui.INK);
            p.setTextSize(f);
            c.drawText(text, cx + dotR + f * 0.6f, cy - (p.ascent() + p.descent()) / 2, p);
            if (isFocused()) WControls.ring(c, p, em, w, h + sh);
        }
    }

    /** The round Projeto Belmante badge (.brand): outline and a thin shadow. */
    static final class Badge extends View {
        private final Bitmap bmp;
        private final float em;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint img = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final android.graphics.Matrix m = new android.graphics.Matrix();
        private boolean down;

        Badge(Context c, Bitmap bmp, float em) {
            super(c);
            this.bmp = bmp;
            this.em = em;
            setClickable(true);
            setFocusable(true);
            setDefaultFocusHighlightEnabled(false);
            if (bmp != null) img.setShader(new BitmapShader(bmp, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP));
            setContentDescription("Projeto Belmante");
        }

        @Override
        protected void onMeasure(int w, int h) {
            int s = Math.round(em * 3.4f);
            setMeasuredDimension(s, s + Math.round(em * 0.12f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float s = em * 3.4f, b = em * 0.14f, sh = em * 0.12f;
            float dy = down ? -em * 0.1f : 0;
            p.setColor(Wui.INK);
            c.drawCircle(s / 2, s / 2 + sh + dy, s / 2, p);
            c.drawCircle(s / 2, s / 2 + dy, s / 2, p);
            if (bmp != null) {
                float d = s - 2 * b;
                m.setScale(d / bmp.getWidth(), d / bmp.getHeight());
                m.postTranslate(b, b + dy);
                img.getShader().setLocalMatrix(m);
                c.drawCircle(s / 2, s / 2 + dy, d / 2, img);
            }
            if (isFocused()) {
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(em * 0.16f);
                p.setColor(Wui.YELLOW);
                c.drawCircle(s / 2, s / 2, s / 2 + em * 0.1f, p);
                p.setStyle(Paint.Style.FILL);
            }
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent e) {
            switch (e.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    down = true;
                    invalidate();
                    return true;
                case android.view.MotionEvent.ACTION_UP:
                    down = false;
                    invalidate();
                    performClick();
                    return true;
                case android.view.MotionEvent.ACTION_CANCEL:
                    down = false;
                    invalidate();
                    return true;
                default:
                    return true;
            }
        }

        @Override
        public boolean onKeyUp(int code, android.view.KeyEvent e) {
            if (code == android.view.KeyEvent.KEYCODE_DPAD_CENTER || code == android.view.KeyEvent.KEYCODE_ENTER
                    || code == android.view.KeyEvent.KEYCODE_BUTTON_A) {
                performClick();
                return true;
            }
            return super.onKeyUp(code, e);
        }
    }

    /** .prog: a pill-shaped bar with diagonal blue stripes and the percentage in the middle. */
    static final class Progress extends View {
        private final float em;
        private float fraction;
        private String label = "";
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final Path clip = new Path(), stripe = new Path();

        Progress(Context c, float em) {
            super(c);
            this.em = em;
            p.setTypeface(Wui.bold());
        }

        void set(float f, String text) {
            fraction = Math.max(0f, Math.min(1f, f));
            label = text;
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(MeasureSpec.getSize(w), Math.round(em * 1.5f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight(), w = getWidth(), b = em * 0.2f;
            p.setStyle(Paint.Style.FILL);
            p.setColor(Wui.INK);
            r.set(0, 0, w, h);
            c.drawRoundRect(r, h, h, p);
            r.inset(b, b);
            p.setColor(0xFFFFFFFF);
            c.drawRoundRect(r, h, h, p);
            clip.reset();
            clip.addRoundRect(r, h, h, Path.Direction.CW);
            c.save();
            c.clipPath(clip);
            float fillRight = r.left + r.width() * fraction;
            c.clipRect(r.left, r.top, fillRight, r.bottom);
            p.setColor(Wui.BLUE);
            c.drawRect(r, p);
            p.setColor(0xFF6CC4FF);
            float period = em * 1.6f, half = em * 0.8f, ih = r.height();
            for (float x = r.left - ih; x < fillRight + ih; x += period) {
                stripe.reset();
                stripe.moveTo(x + half, r.top);
                stripe.lineTo(x + 2 * half, r.top);
                stripe.lineTo(x + 2 * half - ih, r.bottom);
                stripe.lineTo(x + half - ih, r.bottom);
                stripe.close();
                c.drawPath(stripe, p);
            }
            c.restore();
            p.setColor(Wui.INK);
            p.setTextSize(em * 0.75f);
            p.setTextAlign(Paint.Align.CENTER);
            c.drawText(label, w / 2, h / 2 - (p.ascent() + p.descent()) / 2, p);
            p.setTextAlign(Paint.Align.LEFT);
        }
    }

    /** .step: a numbered (or checked) row with a bold line and a small muted one under it. */
    static LinearLayout step(Context c, float em, String marker, boolean done, String title, String sub) {
        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFFFFFFFF);
        bg.setCornerRadius(em * 0.9f);
        bg.setStroke(Math.max(1, Math.round(em * 0.18f)), 0x222b2d33);
        row.setBackground(bg);
        int px = Math.round(em * 0.8f), py = Math.round(em * 0.55f);
        row.setPadding(px, py, px, py);
        TextView n = new TextView(c);
        n.setText(marker);
        n.setTypeface(Wui.bold());
        n.setTextColor(0xFFFFFFFF);
        n.setGravity(Gravity.CENTER);
        n.setIncludeFontPadding(false);
        n.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.85f);
        GradientDrawable circle = new GradientDrawable();
        circle.setShape(GradientDrawable.OVAL);
        circle.setColor(done ? Wui.GREEN : Wui.INK);
        n.setBackground(circle);
        int d = Math.round(em * 1.8f);
        row.addView(n, new LinearLayout.LayoutParams(d, d));
        LinearLayout col = new LinearLayout(c);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView t = new TextView(c);
        t.setText(title);
        t.setTypeface(Wui.semi());
        t.setTextColor(Wui.INK);
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.95f);
        col.addView(t);
        if (sub != null && !sub.isEmpty()) {
            TextView s = new TextView(c);
            s.setText(sub);
            s.setTypeface(Wui.medium());
            s.setTextColor(Wui.MUTED);
            s.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.78f);
            col.addView(s);
        }
        LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(0, -2, 1f);
        cl.leftMargin = Math.round(em * 0.8f);
        row.addView(col, cl);
        return row;
    }
}
