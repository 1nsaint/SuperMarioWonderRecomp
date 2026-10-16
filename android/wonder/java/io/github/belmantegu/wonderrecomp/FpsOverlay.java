package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Debug;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;

import java.util.Locale;

/**
 * The FPS counter shown over the game (a panel window added by GameActivity). The count is frames presented by the
 * runtime (GameActivity.nativeFrames), sampled every 500 ms. Simple: the number. Full: also the mean frame time, the
 * game's memory and the render size.
 */
final class FpsOverlay extends View {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG), big = new Paint(Paint.ANTI_ALIAS_FLAG),
            small = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();
    private final String sizeLine;
    private final float u;
    private int mode;
    private long prevFrames = -1, prevNs;
    private double fps;
    private String l1 = "...", l2 = "", l3 = "";
    private boolean running;
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            sample();
            if (running) handler.postDelayed(this, 500);
        }
    };

    FpsOverlay(Context c, String sizeLine) {
        super(c);
        this.sizeLine = sizeLine;
        u = Wui.u;
        big.setTypeface(Wui.bold());
        big.setTextSize(2.2f * u);
        small.setTypeface(Wui.semi());
        small.setTextSize(1.45f * u);
        bg.setColor(0xB3000000);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(0.18f * u);
        setFocusable(false);
    }

    void setMode(int m) {
        mode = Math.max(0, Math.min(2, m));
        setVisibility(mode == 0 ? GONE : VISIBLE);
        handler.removeCallbacks(tick);
        running = mode != 0 && isAttachedToWindow();
        prevFrames = -1;
        if (running) handler.post(tick);
        requestLayout();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        running = mode != 0;
        if (running) handler.post(tick);
    }

    @Override
    protected void onDetachedFromWindow() {
        running = false;
        handler.removeCallbacks(tick);
        super.onDetachedFromWindow();
    }

    private void sample() {
        long f = GameActivity.frames();
        long now = SystemClock.elapsedRealtimeNanos();
        if (f >= 0 && prevFrames >= 0 && now > prevNs) {
            double s = (now - prevNs) / 1e9;
            fps = (f - prevFrames) / s;
            l1 = String.format(Locale.US, "%.0f", fps);
            if (mode == 2) {
                l2 = fps > 0.5 ? String.format(Locale.US, "%.1f ms", 1000.0 / fps) : "-";
                Debug.MemoryInfo mi = new Debug.MemoryInfo();
                Debug.getMemoryInfo(mi);
                l3 = String.format(Locale.US, "%d MB  %s", mi.getTotalPss() / 1024, sizeLine);
            }
        } else if (f < 0) {
            l1 = "?";
        }
        prevFrames = f;
        prevNs = now;
        invalidate();
    }

    @Override
    protected void onMeasure(int w, int h) {
        float width = mode == 2 ? 15f * u : 7f * u;
        float height = mode == 2 ? 8.4f * u : 4.2f * u;
        setMeasuredDimension(Math.round(width), Math.round(height));
    }

    @Override
    protected void onDraw(Canvas c) {
        r.set(0, 0, getWidth(), getHeight());
        c.drawRoundRect(r, u, u, bg);
        int col = fps >= 55 ? 0xFF7EE787 : fps >= 28 ? 0xFFFFD23A : 0xFFFF5F56;
        big.setColor(col);
        float x = 0.9f * u;
        c.drawText(l1, x, 3.0f * u, big);
        small.setColor(0xFFFFFFFF);
        float nx = x + big.measureText(l1) + 0.5f * u;
        c.drawText("FPS", nx, 3.0f * u, small);
        if (mode == 2) {
            c.drawText(l2, x, 5.5f * u, small);
            small.setColor(0xFFC9C9CF);
            c.drawText(l3, x, 7.4f * u, small);
        }
    }
}
