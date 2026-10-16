package io.github.belmantegu.wonderrecomp;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

/**
 * The game: a NativeActivity whose code is libwonder.so (android_main in runtime/src/platform_android.cpp), in its own
 * process (:game). It adds the FPS counter as a panel window over the game's surface. The touch buttons are drawn by the
 * runtime itself. config.txt is read by the runtime at start; changing options means restarting the game.
 */
public class GameActivity extends NativeActivity {
    private static boolean nativeOk;

    static {
        try {
            System.loadLibrary("wonder");
            nativeOk = true;
        } catch (Throwable t) {
            Log.e(AppFiles.TAG, "libwonder.so: " + t);
        }
    }

    private static native long nativeFrames();

    /** Frames presented so far, or -1 when the runtime has no counter. */
    static long frames() {
        if (!nativeOk) return -1;
        try {
            return nativeFrames();
        } catch (Throwable t) {
            return -1;
        }
    }

    private FrameLayout overlay;
    private FpsOverlay fps;
    private boolean attached;

    @Override
    protected void onCreate(Bundle b) {
        Settings.applyLanguage(this);
        Wui.init(this);
        AppFiles.dropLegacyDataRoot(this);
        Diagnostics.sessionBegin(this);
        super.onCreate(b);
        final View decor = getWindow().getDecorView();
        decor.post(new Runnable() {
            @Override
            public void run() {
                attachOverlay(decor);
            }
        });
    }

    private int fpsMode() {
        try {
            String v = AppFiles.getApp(this, "fps_mode");
            return v == null ? 0 : Math.max(0, Math.min(2, Integer.parseInt(v)));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private String sizeLine() {
        GameConfig c = GameConfig.load(AppFiles.configFile(this));
        String res = c.get("NX_RENDER_RES");
        if (res != null && res.matches("\\d+x\\d+")) return res;
        String h = c.get("NX_HANDHELD");
        return h != null && h.startsWith("0") ? "1920x1080" : "1280x720";
    }

    private FrameLayout.LayoutParams fpsParams() {
        boolean right = "1".equals(AppFiles.getApp(this, "fps_pos"));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, (right ? Gravity.END : Gravity.START) | Gravity.TOP);
        lp.topMargin = Math.round(Wui.u * 0.6f);
        int side = Math.round(Wui.u * 6f);  // clear of the camera cutout and the L button
        if (right) lp.rightMargin = side;
        else lp.leftMargin = side;
        return lp;
    }

    private void attachOverlay(View decor) {
        if (attached || isFinishing() || fpsMode() == 0) return;
        attached = true;
        try {
            overlay = new FrameLayout(this);
            fps = new FpsOverlay(this, sizeLine());
            overlay.addView(fps, fpsParams());
            fps.setMode(fpsMode());
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(-1, -1,
                    WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                    android.graphics.PixelFormat.TRANSLUCENT);
            lp.token = decor.getWindowToken();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            lp.setTitle("wonder-overlay");
            getWindowManager().addView(overlay, lp);
        } catch (Throwable t) {
            Log.e(AppFiles.TAG, "FPS overlay failed: " + t);
        }
    }

    @Override
    protected void onDestroy() {
        if (isFinishing()) Diagnostics.sessionEnd(this);
        if (overlay != null) {
            try {
                getWindowManager().removeViewImmediate(overlay);
            } catch (Throwable ignored) {
            }
            overlay = null;
        }
        attached = false;
        super.onDestroy();
    }
}
