package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.TextView;

/** The option row controls of the mockup: toggle (.tog), choice pill (.val), slider (.sl) and the tags (.tag). */
final class WControls {
    private WControls() {
    }

    interface OnChange {
        void changed(int value);
    }

    static TextView tag(Context c, String text, int color, float em) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTypeface(Wui.bold());
        t.setTextColor(0xFFFFFFFF);
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.62f);
        t.setLetterSpacing(0.03f);
        t.setGravity(Gravity.CENTER);
        t.setIncludeFontPadding(false);
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(em);
        t.setBackground(g);
        int px = Math.round(em * 0.62f * 0.7f), py = Math.round(em * 0.62f * 0.15f);
        t.setPadding(px, py + 1, px, py + 1);
        return t;
    }

    /** .tog: 4.2em x 2.1em pill, green when on. */
    static final class Toggle extends View {
        private final float em;
        private boolean on;
        private final OnChange cb;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();

        Toggle(Context c, float em, boolean on, OnChange cb) {
            super(c);
            this.em = em;
            this.on = on;
            this.cb = cb;
            setClickable(true);
            setFocusable(true);
            setDefaultFocusHighlightEnabled(false);
            setOnClickListener(v -> {
                this.on = !this.on;
                invalidate();
                cb.changed(this.on ? 1 : 0);
            });
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(em * 4.2f), Math.round(em * 2.1f + em * 0.12f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float b = em * 0.2f, hh = em * 2.1f, w = em * 4.2f, sh = em * 0.12f;
            p.setStyle(Paint.Style.FILL);
            p.setColor(Wui.INK);
            r.set(0, sh, w, hh + sh);
            c.drawRoundRect(r, hh / 2, hh / 2, p);
            r.set(0, 0, w, hh);
            c.drawRoundRect(r, hh / 2, hh / 2, p);
            r.inset(b, b);
            p.setColor(on ? Wui.GREEN : 0xFFC9C9CF);
            c.drawRoundRect(r, hh / 2 - b, hh / 2 - b, p);
            float k = em * 1.45f, kx = on ? em * 2.05f : em * 0.2f, ky = em * 0.17f;
            p.setColor(Wui.INK);
            c.drawCircle(kx + k / 2, ky + k / 2, k / 2, p);
            p.setColor(0xFFFFFFFF);
            c.drawCircle(kx + k / 2, ky + k / 2, k / 2 - em * 0.15f, p);
            if (isFocused()) ring(c, p, em, w, hh + sh);
        }

        @Override
        public boolean onKeyUp(int code, KeyEvent e) {
            if (code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_BUTTON_A) {
                performClick();
                return true;
            }
            return super.onKeyUp(code, e);
        }
    }

    static void ring(Canvas c, Paint p, float em, float w, float h) {
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(em * 0.16f);
        p.setColor(Wui.YELLOW);
        c.drawRoundRect(new RectF(0, 0, w, h), em, em, p);
        p.setStyle(Paint.Style.FILL);
    }

    /** .val: a pill showing the current choice; the left half steps back, the right half forward. */
    static final class Choice extends View {
        private final float em;
        private final String[] labels;
        private int index;
        private final OnChange cb;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private boolean down;

        Choice(Context c, float em, String[] labels, int index, OnChange cb) {
            super(c);
            this.em = em;
            this.labels = labels;
            this.index = index;
            this.cb = cb;
            setClickable(true);
            setFocusable(true);
            setDefaultFocusHighlightEnabled(false);
            p.setTypeface(Wui.bold());
        }

        private void step(int d) {
            index = (index + d + labels.length) % labels.length;
            requestLayout();
            invalidate();
            cb.changed(index);
        }

        @Override
        protected void onMeasure(int w, int h) {
            p.setTextSize(em * 0.95f);
            float widest = 0;
            for (String s : labels) widest = Math.max(widest, p.measureText(s));
            float arrows = em * 0.95f * 1.6f;
            float width = Math.max(em * 9.5f * 0.95f, widest + em * 1.4f * 0.95f + arrows);
            setMeasuredDimension(Math.round(Math.min(width, em * 16f)), Math.round(em * 0.95f * 1.8f + em * 0.12f + em * 0.36f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float f = em * 0.95f, b = em * 0.18f, sh = em * 0.12f, w = getWidth();
            float h = getHeight() - sh;
            float dy = down ? sh * 0.8f : 0;
            sh = down ? sh * 0.2f : sh;
            p.setStyle(Paint.Style.FILL);
            p.setColor(Wui.INK);
            r.set(0, dy + sh, w, dy + sh + h);
            c.drawRoundRect(r, em * 0.8f, em * 0.8f, p);
            r.set(0, dy, w, dy + h);
            c.drawRoundRect(r, em * 0.8f, em * 0.8f, p);
            r.inset(b, b);
            p.setColor(Wui.CREAM2);
            c.drawRoundRect(r, em * 0.8f - b, em * 0.8f - b, p);
            p.setTextSize(f);
            p.setTextAlign(Paint.Align.CENTER);
            p.setColor(Wui.INK);
            String s = labels[index];
            float max = w - 2 * (b + f * 1.1f);
            while (p.measureText(s) > max && p.getTextSize() > f * 0.6f) p.setTextSize(p.getTextSize() - 1);
            float base = dy + h / 2 - (p.ascent() + p.descent()) / 2;
            c.drawText(s, w / 2, base, p);
            p.setTextSize(f);
            p.setColor(Wui.alpha(Wui.INK, 0.4f));
            p.setTextAlign(Paint.Align.LEFT);
            c.drawText("‹", b + f * 0.5f, base, p);
            p.setTextAlign(Paint.Align.RIGHT);
            c.drawText("›", w - b - f * 0.5f, base, p);
            p.setTextAlign(Paint.Align.LEFT);
            if (isFocused()) ring(c, p, em, w, h + sh);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    down = true;
                    invalidate();
                    return true;
                case MotionEvent.ACTION_UP:
                    down = false;
                    invalidate();
                    if (e.getX() >= 0 && e.getX() <= getWidth() && e.getY() >= 0 && e.getY() <= getHeight())
                        step(e.getX() < getWidth() / 2f ? -1 : 1);
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    down = false;
                    invalidate();
                    return true;
                default:
                    return true;
            }
        }

        @Override
        public boolean onKeyDown(int code, KeyEvent e) {
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) {
                step(-1);
                return true;
            }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT || code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_ENTER
                    || code == KeyEvent.KEYCODE_BUTTON_A) {
                step(1);
                return true;
            }
            return super.onKeyDown(code, e);
        }
    }

    /** .sl: a track with a draggable knob and the value in %. */
    static final class Slider extends View {
        private final float em;
        private final int min, max, stepSize;
        private int value;
        private final OnChange cb;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();

        Slider(Context c, float em, int min, int max, int step, int value, OnChange cb) {
            super(c);
            this.em = em;
            this.min = min;
            this.max = max;
            this.stepSize = step;
            this.value = value;
            this.cb = cb;
            setClickable(true);
            setFocusable(true);
            setDefaultFocusHighlightEnabled(false);
            p.setTypeface(Wui.bold());
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(em * 15f), Math.round(em * 2.2f));
        }

        private float trackLeft() {
            return em * 0.8f;
        }

        private float trackRight() {
            return getWidth() - em * 3.6f;
        }

        @Override
        protected void onDraw(Canvas c) {
            float cy = getHeight() / 2f, th = em * 0.6f, l = trackLeft(), rr = trackRight();
            p.setStyle(Paint.Style.FILL);
            p.setColor(Wui.INK);
            r.set(l - em * 0.1f, cy - th / 2 - em * 0.1f, rr + em * 0.1f, cy + th / 2 + em * 0.1f);
            c.drawRoundRect(r, th, th, p);
            p.setColor(0xFFFFFFFF);
            r.inset(em * 0.14f, em * 0.14f);
            c.drawRoundRect(r, th, th, p);
            float f = (value - min) / (float) (max - min), kx = l + (rr - l) * f;
            p.setColor(Wui.RED);
            r.set(l, cy - th / 2 + em * 0.04f, kx, cy + th / 2 - em * 0.04f);
            c.drawRoundRect(r, th, th, p);
            p.setColor(Wui.INK);
            c.drawCircle(kx, cy, em * 0.85f, p);
            p.setColor(Wui.RED);
            c.drawCircle(kx, cy, em * 0.85f - em * 0.18f, p);
            p.setTextSize(em);
            p.setColor(Wui.INK);
            p.setTextAlign(Paint.Align.RIGHT);
            c.drawText(value + "%", getWidth(), cy - (p.ascent() + p.descent()) / 2, p);
            p.setTextAlign(Paint.Align.LEFT);
            if (isFocused()) ring(c, p, em, getWidth(), getHeight());
        }

        private void set(int v, boolean notify) {
            v = Math.max(min, Math.min(max, Math.round((v - min) / (float) stepSize) * stepSize + min));
            if (v == value) return;
            value = v;
            invalidate();
            if (notify) cb.changed(value);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN) getParent().requestDisallowInterceptTouchEvent(true);
            float f = (e.getX() - trackLeft()) / (trackRight() - trackLeft());
            set(Math.round(min + Math.max(0f, Math.min(1f, f)) * (max - min)), true);
            return true;
        }

        @Override
        public boolean onKeyDown(int code, KeyEvent e) {
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) {
                set(value - stepSize, true);
                return true;
            }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT) {
                set(value + stepSize, true);
                return true;
            }
            return super.onKeyDown(code, e);
        }
    }
}
