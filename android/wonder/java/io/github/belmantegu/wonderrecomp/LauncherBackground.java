package io.github.belmantegu.wonderrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BlendMode;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

/**
 * The living background of the mockup (.bg): the wallpaper drifts slowly (zoom 1.02 to 1.12 and a small pan, 26 s,
 * back and forth), a soft light sweeps across it (9 s) and seven sparkles twinkle. One bitmap drawn with a matrix, a
 * gradient shader moved with a matrix and a few small paths per frame: no allocations in onDraw. It animates only
 * while resumed and attached.
 */
final class LauncherBackground extends View {
    private static final float[][] SPARKLES = {  // left %, top %, delay s
        {56, 12, 0.2f}, {78, 8, 1.1f}, {90, 30, 2f}, {62, 58, 0.7f}, {48, 36, 2.6f}, {84, 62, 1.7f}, {70, 30, 3f}};
    private final Bitmap art;
    private final Paint artPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Paint sweepPaint = new Paint();
    private final Paint sparkPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path spark = new Path();
    private final Matrix m = new Matrix();
    private final Matrix sm = new Matrix();
    private LinearGradient sweep;
    private final long t0 = SystemClock.uptimeMillis();
    private boolean running = true;

    LauncherBackground(Context c) {
        super(c);
        art = Wui.asset(c, "launcher/background.jpg");
        setBackgroundColor(Wui.CREAM);
        sweepPaint.setBlendMode(BlendMode.SOFT_LIGHT);
        spark.moveTo(50, 0);
        float[][] pts = {{60, 40}, {100, 50}, {60, 60}, {50, 100}, {40, 60}, {0, 50}, {40, 40}};
        for (float[] p : pts) spark.lineTo(p[0], p[1]);
        spark.close();
    }

    void setRunning(boolean on) {
        running = on;
        if (on) postInvalidateOnAnimation();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postInvalidateOnAnimation();
    }

    private static float ease(float x) {  // 0..1..0 triangle -> smooth
        return 0.5f - 0.5f * (float) Math.cos(Math.PI * x);
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        float t = (SystemClock.uptimeMillis() - t0) / 1000f;
        if (art != null) {
            float aw = w * 1.12f, ah = h * 1.12f;
            float s0 = Math.max(aw / art.getWidth(), ah / art.getHeight());
            float ox = (aw - art.getWidth() * s0) * 0.5f, oy = (ah - art.getHeight() * s0) * 0.3f;
            float ph = (t / 26f) % 2f;
            float p = ease(ph > 1f ? 2f - ph : ph);
            float k = 1.02f + 0.10f * p;
            float tx = (-0.012f + 0.028f * p) * aw, ty = (0.006f - 0.020f * p) * ah;
            m.setScale(s0, s0);
            m.postTranslate(ox - 0.06f * w, oy - 0.06f * h);
            m.postScale(k, k, w / 2, h / 2);
            m.postTranslate(tx, ty);
            c.drawBitmap(art, m, artPaint);
        }
        if (sweep == null) {
            float a = (float) Math.toRadians(105), dx = (float) Math.sin(a) * 0.375f * w, dy = (float) -Math.cos(a) * 0.375f * w;
            sweep = new LinearGradient(-dx, -dy, dx, dy, new int[] {0x00FFFFFF, 0x66FFFFFF, 0x00FFFFFF},
                    new float[] {0f, 0.5f, 1f}, Shader.TileMode.CLAMP);
        }
        float f = (t / 9f) % 1f;
        sm.setTranslate(w * (-0.7f + 3.9f * f), h / 2);
        sweep.setLocalMatrix(sm);
        sweepPaint.setShader(sweep);
        c.drawRect(0, 0, w, h, sweepPaint);
        float size = 1.6f * Wui.u;
        for (float[] s : SPARKLES) {
            float tt = t - s[2];
            if (tt < 0) continue;
            float ph = (tt / 3.6f) % 1f;
            float e = ease(ph < 0.5f ? ph * 2f : 2f - ph * 2f);
            if (e <= 0.01f) continue;
            float cx = w * s[0] / 100f + size / 2, cy = h * s[1] / 100f + size / 2;
            float sc = (0.3f + 0.7f * e) * size / 100f;
            c.save();
            c.translate(cx, cy);
            c.rotate(45f * e);
            c.scale(sc, sc);
            c.translate(-50, -50);
            sparkPaint.setColor(Wui.alpha(0xFF8FD2FF, 0.45f * e));
            c.save();
            c.translate(50, 50);
            c.scale(1.35f, 1.35f);
            c.translate(-50, -50);
            c.drawPath(spark, sparkPaint);
            c.restore();
            sparkPaint.setColor(Wui.alpha(0xFFFFFFFF, e));
            c.drawPath(spark, sparkPaint);
            c.restore();
        }
        if (running && isAttachedToWindow()) postInvalidateOnAnimation();
    }
}
