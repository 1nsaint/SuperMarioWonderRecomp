package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.view.MotionEvent;
import android.view.View;

/**
 * The Wonder button of the mockup: a pill with a thin dark outline and a thin hard shadow underneath that presses down
 * when touched (.wbtn). Styles: cream, red, blue, green, ghost and "wonder" (blue-to-pink gradient); red and wonder
 * carry the glossy stripe. Optional Lucide icon, a bold title and a small second line.
 */
final class WButton extends View {
    enum Style { CREAM, RED, WONDER, BLUE, GREEN, GHOST }

    private final Style style;
    private final float fs;          // the button's font size in px (1.25em of the mockup unless given)
    private final boolean center;    // text centred (the .btns rows) instead of growing to the left
    private String title = "", sub = "";
    private Drawable icon;
    private boolean tab, active;
    private int faceOverride = 0, fgOverride = 0;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final TextPaint titlePaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final TextPaint subPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();
    private final android.graphics.Path facePath = new android.graphics.Path();
    private StaticLayout titleLayout, subLayout;
    private int layoutWidth = -1;
    private boolean pressedDown;

    WButton(Context c, Style style, float fontPx, boolean center) {
        super(c);
        this.style = style;
        this.fs = fontPx;
        this.center = center;
        setClickable(true);
        setFocusable(true);
        setDefaultFocusHighlightEnabled(false);
        titlePaint.setTypeface(Wui.bold());
        subPaint.setTypeface(Wui.medium());
    }

    WButton text(String title, String sub) {
        this.title = title == null ? "" : title;
        this.sub = sub == null ? "" : sub;
        layoutWidth = -1;
        requestLayout();
        invalidate();
        return this;
    }

    WButton icon(String name) {
        icon = name == null ? null : Wui.icon(getContext(), name, 0);
        requestLayout();
        invalidate();
        return this;
    }

    /** A sidebar tab: smaller; the active one is red. */
    WButton tab(boolean isTab) {
        tab = isTab;
        return this;
    }

    WButton active(boolean a) {
        active = a;
        invalidate();
        return this;
    }

    WButton colors(int face, int fg) {
        faceOverride = face;
        fgOverride = fg;
        invalidate();
        return this;
    }

    private float border() {
        return fs * 0.18f;
    }

    private float lift() {
        return fs * 0.2f;
    }

    private int fg() {
        if (fgOverride != 0) return fgOverride;
        Style s = effective();
        return s == Style.RED || s == Style.WONDER || s == Style.BLUE || s == Style.GREEN ? 0xFFFFFFFF : Wui.INK;
    }

    private Style effective() {
        return tab && active ? Style.RED : style;
    }

    private float iconSize() {
        return fs * 1.1f * (tab ? 1.18f : 1f);
    }

    private void build(int width) {
        float padX = fs * 0.88f;  // padding .75em/1.1em of the button's own 1.25em font, as in the mockup
        float avail = width - 2 * border() - 2 * padX - (icon != null ? iconSize() + fs * 0.7f : 0);
        titlePaint.setTextSize(fs);
        subPaint.setTextSize(fs * 0.62f);
        int w = Math.max(1, (int) avail);
        Layout.Alignment al = center ? Layout.Alignment.ALIGN_CENTER : Layout.Alignment.ALIGN_NORMAL;
        titleLayout = StaticLayout.Builder.obtain(title, 0, title.length(), titlePaint, w).setAlignment(al).build();
        subLayout = sub.isEmpty() ? null
                : StaticLayout.Builder.obtain(sub, 0, sub.length(), subPaint, w).setAlignment(al).build();
        layoutWidth = width;
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int mode = MeasureSpec.getMode(wSpec), size = MeasureSpec.getSize(wSpec);
        float padX = fs * 0.88f;
        float extra = 2 * border() + 2 * padX + (icon != null ? iconSize() + fs * 0.7f : 0);
        int width;
        if (mode == MeasureSpec.EXACTLY) {
            width = size;
        } else {
            titlePaint.setTextSize(fs);
            subPaint.setTextSize(fs * 0.62f);
            float t = titlePaint.measureText(title), s = sub.isEmpty() ? 0 : subPaint.measureText(sub);
            width = (int) Math.ceil(Math.max(t, s) + extra + fs * 0.1f);
            if (mode == MeasureSpec.AT_MOST) width = Math.min(width, size);
        }
        build(width);
        float textH = titleLayout.getHeight() + (subLayout != null ? subLayout.getHeight() + fs * 0.25f * 0.62f : 0);
        float padY = fs * (tab ? 0.62f : 0.75f);
        int height = (int) Math.ceil(Math.max(textH, icon != null ? iconSize() : 0) + 2 * padY + 2 * border() + lift());
        setMeasuredDimension(width, height);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (layoutWidth != getWidth()) build(getWidth());
        boolean down = pressedDown && isEnabled();
        float b = border(), lift = lift();
        float dy = down ? lift * 0.8f : 0;
        float shadow = down ? lift * 0.2f : lift;
        float h = getHeight() - lift, w = getWidth();
        float radius = Math.min(fs * 1.2f, h / 2f);
        Style s = effective();
        canvas.save();
        if (!isEnabled()) canvas.saveLayerAlpha(0, 0, getWidth(), getHeight(), 166);
        // hard shadow: only the sliver below the face (ghost buttons are translucent)
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        r.set(0, dy, w, dy + h);
        facePath.reset();
        facePath.addRoundRect(r, radius, radius, android.graphics.Path.Direction.CW);
        canvas.save();
        canvas.clipOutPath(facePath);
        paint.setColor(Wui.INK);
        RectF sr = new RectF(0, dy + shadow, w, dy + shadow + h);
        canvas.drawRoundRect(sr, radius, radius, paint);
        canvas.restore();
        // face, then the outline on top
        if (faceOverride != 0) {
            paint.setColor(faceOverride);
        } else if (s == Style.WONDER) {
            paint.setShader(new LinearGradient(0, r.top, 0, r.bottom, Wui.BLUE, Wui.PINK, Shader.TileMode.CLAMP));
        } else {
            paint.setColor(s == Style.RED ? Wui.RED : s == Style.BLUE ? Wui.BLUE : s == Style.GREEN ? Wui.GREEN
                    : s == Style.GHOST ? Wui.alpha(Wui.CREAM, 0.8f) : Wui.CREAM);
        }
        canvas.drawRoundRect(r, radius, radius, paint);
        paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(b);
        paint.setColor(Wui.INK);
        r.inset(b / 2, b / 2);
        canvas.drawRoundRect(r, radius - b / 2, radius - b / 2, paint);
        paint.setStyle(Paint.Style.FILL);
        r.inset(b / 2, b / 2);
        if (s == Style.RED || s == Style.WONDER) {  // glossy stripe
            paint.setColor(0x33FFFFFF);
            float top = r.top, left = r.left + fs * 0.32f, right = r.right - fs * 0.32f;
            RectF st = new RectF(left, top, right, top + r.height() * 0.38f);
            canvas.drawRoundRect(st, fs * 0.64f, fs * 0.64f, paint);
        }
        // content
        int fg = fg();
        float padX = fs * 0.88f + b;
        float x = padX;
        float cy = dy + h / 2f;
        if (icon != null) {
            icon.setTint(fg);
            int is = (int) iconSize();
            icon.setBounds((int) x, (int) (cy - is / 2f), (int) x + is, (int) (cy + is / 2f));
            icon.draw(canvas);
            x += is + fs * 0.7f;
        }
        titlePaint.setColor(fg);
        if (s == Style.WONDER) titlePaint.setShadowLayer(fs * 0.06f, 0, fs * 0.06f, 0x44000000);
        else titlePaint.clearShadowLayer();
        float textH = titleLayout.getHeight() + (subLayout != null ? subLayout.getHeight() + fs * 0.155f : 0);
        float ty = cy - textH / 2f;
        canvas.save();
        canvas.translate(x, ty);
        titleLayout.draw(canvas);
        canvas.restore();
        if (subLayout != null) {
            subPaint.setColor(Wui.alpha(fg, 0.75f));
            canvas.save();
            canvas.translate(x, ty + titleLayout.getHeight() + fs * 0.155f);
            subLayout.draw(canvas);
            canvas.restore();
        }
        if (!isEnabled()) canvas.restore();
        canvas.restore();
        if (isFocused()) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(fs * 0.16f);
            paint.setColor(Wui.YELLOW);
            r.set(-b * 0.2f, dy - b * 0.2f, w + b * 0.2f, dy + h + shadow + b * 0.2f);
            canvas.drawRoundRect(r, radius, radius, paint);
            paint.setStyle(Paint.Style.FILL);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!isEnabled()) return true;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                pressedDown = true;
                invalidate();
                break;
            case MotionEvent.ACTION_MOVE:
                boolean inside = e.getX() >= 0 && e.getX() <= getWidth() && e.getY() >= 0 && e.getY() <= getHeight();
                if (inside != pressedDown) {
                    pressedDown = inside;
                    invalidate();
                }
                break;
            case MotionEvent.ACTION_UP:
                boolean was = pressedDown;
                pressedDown = false;
                invalidate();
                if (was) performClick();
                break;
            case MotionEvent.ACTION_CANCEL:
                pressedDown = false;
                invalidate();
                break;
            default:
                break;
        }
        return true;
    }

    @Override
    public boolean onKeyDown(int code, android.view.KeyEvent ev) {
        if (code == android.view.KeyEvent.KEYCODE_DPAD_CENTER || code == android.view.KeyEvent.KEYCODE_ENTER
                || code == android.view.KeyEvent.KEYCODE_BUTTON_A) {
            pressedDown = true;
            invalidate();
            return true;
        }
        return super.onKeyDown(code, ev);
    }

    @Override
    public boolean onKeyUp(int code, android.view.KeyEvent ev) {
        if (pressedDown && (code == android.view.KeyEvent.KEYCODE_DPAD_CENTER || code == android.view.KeyEvent.KEYCODE_ENTER
                || code == android.view.KeyEvent.KEYCODE_BUTTON_A)) {
            pressedDown = false;
            invalidate();
            performClick();
            return true;
        }
        return super.onKeyUp(code, ev);
    }

    @Override
    protected void onFocusChanged(boolean gain, int dir, android.graphics.Rect prev) {
        super.onFocusChanged(gain, dir, prev);
        invalidate();
    }
}
