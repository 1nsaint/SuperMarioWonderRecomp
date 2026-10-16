package io.github.belmantegu.wonderrecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/** The options screen of the mockup: eight-to-seven tabs on the left, the rows of the current tab on the right. */
public class OptionsActivity extends Activity {
    private static final int REQ_DIAG = 11, REQ_SAVES_OUT = 12, REQ_SAVES_IN = 13;
    private static final String CHANNEL = "https://www.youtube.com/@belmante";

    private OptionsModel model;
    private String cur = Props.T_IMAGE;
    private LauncherBackground background;
    private LinearLayout tabs, list;
    private TextView title, note;
    private ScrollView listScroll;
    private float em;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Settings.applyLanguage(this);
        Wui.init(this);
        em = Wui.em(1);
        model = new OptionsModel(new OptionsModel.FileBackend(this));
        build();
    }

    private void build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Wui.CREAM);
        background = new LauncherBackground(this);
        root.addView(background, new FrameLayout.LayoutParams(-1, -1));
        View veil = new View(this);
        veil.setBackgroundColor(0xEEFFFBEA);
        root.addView(veil, new FrameLayout.LayoutParams(-1, -1));

        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        int screenW = Math.max(dm.widthPixels, dm.heightPixels);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        FrameLayout.LayoutParams pl = new FrameLayout.LayoutParams(-1, -1);
        int padX = Math.round(screenW * 0.03f), padY = Math.round(screenW * 0.022f);
        page.setPadding(padX, padY, padX, padY);
        root.addView(page, pl);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        WButton back = new WButton(this, WButton.Style.CREAM, Wui.em(1f), false).icon("chevron-left").text(Lang.s("Back", "Voltar"), null);
        back.setOnClickListener(v -> finish());
        top.addView(back, new LinearLayout.LayoutParams(-2, -2));
        title = new TextView(this);
        title.setTypeface(Wui.bold());
        title.setTextColor(Wui.INK);
        title.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 1.9f);
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(-2, -2);
        tl.leftMargin = Math.round(em);
        top.addView(title, tl);
        note = new TextView(this);
        note.setTypeface(Wui.medium());
        note.setTextColor(Wui.MUTED);
        note.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.9f);
        top.addView(note, tl);
        page.addView(top, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout body = new LinearLayout(this);
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(-1, 0, 1f);
        bl.topMargin = Math.round(em * 0.9f);
        page.addView(body, bl);

        ScrollView tabScroll = new ScrollView(this);
        tabScroll.setVerticalScrollBarEnabled(false);
        tabScroll.setVerticalFadingEdgeEnabled(true);
        tabScroll.setFadingEdgeLength(Math.round(em * 1.2f));
        tabScroll.setClipToPadding(false);
        tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.VERTICAL);
        tabs.setPadding(Math.round(em * 0.1f), Math.round(em * 0.1f), Math.round(em * 0.3f), Math.round(em * 0.6f));
        tabScroll.addView(tabs);
        body.addView(tabScroll, new LinearLayout.LayoutParams(Math.round(em * 15.5f), -1));

        WCard panel = WCard.panel(this);
        listScroll = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        // Room under the last row and a fade at the edge, so a row cut by the panel reads as "scroll", not as a bug.
        list.setPadding(0, 0, 0, Math.round(em * 1.1f));
        listScroll.setVerticalFadingEdgeEnabled(true);
        listScroll.setFadingEdgeLength(Math.round(em * 1.6f));
        listScroll.setVerticalScrollBarEnabled(false);
        listScroll.addView(list);
        panel.addView(listScroll, new LinearLayout.LayoutParams(-1, -1));
        LinearLayout.LayoutParams ql = new LinearLayout.LayoutParams(0, -1, 1f);
        ql.leftMargin = Math.round(em * 1.2f);
        ql.bottomMargin = Math.round(em * 0.3f);
        body.addView(panel, ql);

        LinearLayout legend = new LinearLayout(this);
        legend.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        legend.addView(WControls.tag(this, OptionsText.tagAdvanced(), Wui.INK2, em));
        legend.addView(WControls.tag(this, OptionsText.tagExperimental(), Wui.PINK, em), margin(em * 0.6f));
        legend.addView(WControls.tag(this, Lang.s("RESTARTS THE GAME", "REINICIA O JOGO"), Wui.BLUE, em), margin(em * 0.6f));
        LinearLayout.LayoutParams lgl = new LinearLayout.LayoutParams(-1, -2);
        lgl.topMargin = Math.round(em * 0.3f);
        page.addView(legend, lgl);

        setContentView(root);
        Wui.hideSystemBars(this);
        renderTabs();
        renderList();
    }

    private LinearLayout.LayoutParams margin(float left) {
        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(-2, -2);
        l.leftMargin = Math.round(left);
        return l;
    }

    @Override
    public void onWindowFocusChanged(boolean f) {
        super.onWindowFocusChanged(f);
        if (f) Wui.hideSystemBars(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        background.setRunning(true);
    }

    @Override
    protected void onPause() {
        background.setRunning(false);
        super.onPause();
    }

    private void renderTabs() {
        tabs.removeAllViews();
        for (int i = 0; i < Props.TABS.length; i++) {
            final String id = Props.TABS[i];
            WButton t = new WButton(this, WButton.Style.CREAM, Wui.em(0.8f), false).tab(true).icon(Props.TAB_ICONS[i])
                    .text(OptionsText.tabName(id), null).active(id.equals(cur));
            t.setOnClickListener(v -> {
                cur = id;
                renderTabs();
                renderList();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            if (i > 0) lp.topMargin = Math.round(em * 0.25f);
            tabs.addView(t, lp);
        }
        title.setText(OptionsText.tabName(cur));
        note.setText(OptionsText.tabNote(cur));
    }

    private void renderList() {
        int scroll = listScroll.getScrollY();
        list.removeAllViews();
        for (Props.Prop p : model.rows(cur)) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.bottomMargin = Math.round(em * 0.7f);
            list.addView(row(p), lp);
        }
        listScroll.post(() -> listScroll.scrollTo(0, scroll));
    }

    private View row(final Props.Prop p) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Wui.CREAM);
        bg.setCornerRadius(em * 0.9f);
        bg.setStroke(Math.max(1, Math.round(em * 0.18f)), 0x222b2d33);
        row.setBackground(bg);
        int px = Math.round(em * 0.8f), py = Math.round(em * 0.6f);
        row.setPadding(px, py, px, py + Math.round(em * 0.12f));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this);
        t.setText(OptionsText.title(p.id));
        t.setTypeface(Wui.bold());
        t.setTextColor(Wui.INK);
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 1.02f);
        head.addView(t);
        if (p.advanced) head.addView(WControls.tag(this, OptionsText.tagAdvanced(), Wui.INK2, em), margin(em * 0.5f));
        if (p.experimental) head.addView(WControls.tag(this, OptionsText.tagExperimental(), Wui.PINK, em), margin(em * 0.5f));
        if (p.restart) head.addView(WControls.tag(this, OptionsText.tagRestart(), Wui.BLUE, em), margin(em * 0.5f));
        text.addView(head);
        String d = OptionsText.desc(p.id);
        if (!d.isEmpty()) {
            TextView dv = new TextView(this);
            dv.setText(d);
            dv.setTypeface(Wui.medium());
            dv.setTextColor(Wui.MUTED);
            dv.setLineSpacing(0, 1.05f);
            dv.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.76f);
            text.addView(dv);
        }
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(0, -2, 1f);
        tl.rightMargin = Math.round(em);
        row.addView(text, tl);

        View ctl = null;
        switch (p.kind) {
            case TOGGLE:
                ctl = new WControls.Toggle(this, em, model.index(p) == 1, v -> {
                    model.setIndex(p, v);
                    afterChange(p);
                });
                break;
            case CHOICE:
                ctl = new WControls.Choice(this, em, model.labels(p), model.index(p), v -> {
                    model.setIndex(p, v);
                    afterChange(p);
                });
                break;
            case SLIDER:
                ctl = new WControls.Slider(this, em, p.min, p.max, p.step, model.slider(p), v -> model.setSlider(p, v));
                break;
            case ACTION: {
                final String id = p.id;
                WButton b = new WButton(this, "wipe".equals(id) ? WButton.Style.RED : "clearcache".equals(id) ? WButton.Style.CREAM : WButton.Style.BLUE,
                        Wui.em(0.85f), false).icon(OptionsText.actionIcon(id)).text(OptionsText.actionLabel(id), null);
                b.setOnClickListener(v -> action(id));
                ctl = b;
                break;
            }
            default: {
                TextView v = new TextView(this);
                v.setText(info(p.id));
                v.setTypeface(Wui.bold());
                v.setTextColor(Wui.INK2);
                v.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em);
                v.setGravity(Gravity.END);
                ctl = v;
            }
        }
        row.addView(ctl, new LinearLayout.LayoutParams(-2, -2));
        return row;
    }

    private void afterChange(Props.Prop p) {
        if ("lang".equals(p.id)) {
            Settings.applyLanguage(this);
            build();  // texts change language
            return;
        }
        if ("advanced".equals(p.id) || "fsr".equals(p.id)) renderList();
    }

    private String info(String id) {
        switch (id) {
            case "state":
                int s = AppFiles.ready(this) ? 0 : AppFiles.hasAnalysis(this) || AppFiles.hasRomfs(this) ? 1 : 2;
                return s == 0 ? Lang.s("Imported ✓", "Importados ✓") : s == 1 ? Lang.s("Incomplete", "Incompletos") : Lang.s("Not imported", "Não importados");
            case "space":
                try {
                    StatFs fs = new StatFs(AppFiles.dataDir(this).getAbsolutePath());
                    return String.format(Locale.US, "%.1f GB", fs.getAvailableBytes() / 1.073741824e9);
                } catch (RuntimeException e) {
                    return "?";
                }
            case "version":
                try {
                    android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
                    return pi.versionName + " (build " + pi.getLongVersionCode() + ")";
                } catch (android.content.pm.PackageManager.NameNotFoundException e) {
                    return "?";
                }
            default:
                return "";
        }
    }

    // ---- actions ----

    private File data() {
        return AppFiles.dataDir(this);
    }

    private void saveConfig(String key, String value) {
        new OptionsModel.FileBackend(this).setCfg(key, value);
    }

    private void action(String id) {
        switch (id) {
            case "layout":
                startActivity(new Intent(this, LayoutEditorActivity.class));
                break;
            case "mirror": {
                GameConfig c = GameConfig.load(AppFiles.configFile(this));
                ControlLayout l = ControlLayout.parse(c.get("NX_TOUCH_LAYOUT"));
                l.mirror();
                saveConfig("NX_TOUCH_LAYOUT", l.isDefault() ? null : l.serialize());
                boolean now = !"1".equals(c.get("NX_TOUCH_MIRROR"));
                saveConfig("NX_TOUCH_MIRROR", now ? "1" : null);
                Wui.toast(this, now ? Lang.s("Layout mirrored (left-handed)", "Layout espelhado (canhoto)") : Lang.s("Layout back to right-handed", "Layout de volta para destro"));
                break;
            }
            case "resetlayout":
                saveConfig("NX_TOUCH_LAYOUT", null);
                saveConfig("NX_TOUCH_MIRROR", null);
                Wui.toast(this, Lang.s("Layout restored", "Layout restaurado"));
                break;
            case "import":
                setResult(RESULT_OK, new Intent().putExtra("action", "import"));
                finish();
                break;
            case "saves_export":
                createDocument("wonder-saves.zip", REQ_SAVES_OUT);
                break;
            case "saves_import":
                openDocument(REQ_SAVES_IN);
                break;
            case "clearcache": {
                int n = 0;
                File[] fs = data().listFiles();
                if (fs != null) for (File f : fs) if (f.getName().startsWith("pipelines_") && f.getName().endsWith(".bin") && f.delete()) n++;
                Wui.toast(this, String.format(Locale.US, Lang.s("%d cache files deleted", "%d arquivos de cache apagados"), n));
                break;
            }
            case "wipe":
                WCard.confirm(this, Lang.s("Delete the imported data?", "Apagar os dados importados?"),
                        Lang.s("Removes analysis, extract and spirv (about 3.7 GB). Saves stay. You will have to import the game again.",
                                "Remove analysis, extract e spirv (cerca de 3,7 GB). Os saves ficam. Você precisará importar o jogo de novo."),
                        Lang.s("Delete", "Apagar"), Lang.s("Cancel", "Cancelar"), new Runnable() {
                            @Override
                            public void run() {
                                wipe();
                            }
                        });
                break;
            case "logpath": {
                File log = new File(data(), "nx.log");
                String tail = Diagnostics.tailText(log, 8192);
                WCard.show(this, "Logs", Lang.s("Files:\n", "Arquivos:\n") + log.getAbsolutePath() + "\n" + new File(data(), "nx.log.prev").getAbsolutePath()
                        + "\n\n" + Lang.s("Last lines of nx.log:\n", "Últimas linhas do nx.log:\n") + (tail.isEmpty() ? Lang.s("(no log yet)", "(ainda sem log)") : tail), null,
                        new WCard.Action("OK", WButton.Style.RED, null));
                break;
            }
            case "diag":
                createDocument("wonder-diagnostics.zip", REQ_DIAG);
                break;
            case "device":
                WCard.show(this, Lang.s("This device", "Este aparelho"), Diagnostics.device(this)
                        + "\n" + Lang.s("The game needs Vulkan 1.3 (android.hardware.vulkan.version >= 0x403000), 64-bit ARM.",
                        "O jogo exige Vulkan 1.3 (android.hardware.vulkan.version >= 0x403000), ARM de 64 bits.")
                        + "\n" + String.format(Locale.US, "RAM: %d MB", Settings.totalRamMb(this)), null, new WCard.Action("OK", WButton.Style.RED, null));
                break;
            case "credits":
                WCard.show(this, Lang.s("Credits", "Créditos"), Lang.s(
                        "Projeto Belmante: https://www.youtube.com/@belmante\n\nThe recompiler, runtime and launcher are by the project. The game runs only from files you dump from your own console.\n\nShader translation uses the Ryujinx translator (MIT) offline. Fredoka font (SIL OFL), Lucide icons (ISC).",
                        "Projeto Belmante: https://www.youtube.com/@belmante\n\nO recompilador, o runtime e o launcher são do projeto. O jogo roda só com arquivos que você extrai do seu próprio console.\n\nA tradução de shaders usa o tradutor do Ryujinx (MIT), offline. Fonte Fredoka (SIL OFL), ícones Lucide (ISC)."),
                        null, new WCard.Action("OK", WButton.Style.RED, null));
                break;
            case "yt":
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(CHANNEL)));
                } catch (RuntimeException e) {
                    Wui.toast(this, CHANNEL);
                }
                break;
            case "licenses":
                WCard.show(this, Lang.s("Licences", "Licenças"), licenses(), null, new WCard.Action("OK", WButton.Style.RED, null));
                break;
            default:
                break;
        }
    }

    private String licenses() {
        StringBuilder sb = new StringBuilder();
        for (String n : new String[] {"OFL-Fredoka.txt", "LICENSE-lucide.txt"}) {
            try (InputStream in = getAssets().open("licenses/" + n)) {
                java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
                byte[] b = new byte[4096];
                int r;
                while ((r = in.read(b)) > 0) o.write(b, 0, r);
                sb.append("== ").append(n).append(" ==\n").append(new String(o.toByteArray(), "UTF-8")).append("\n\n");
            } catch (IOException ignored) {
            }
        }
        return sb.toString();
    }

    private void wipe() {
        ImportJob j = ImportJob.current();
        if (j != null && !j.finished) {
            Wui.toast(this, Lang.s("An import is running", "Há uma importação rodando"));
            return;
        }
        File d = data();
        for (String n : new String[] {"analysis", "extract", "spirv", ".import"}) AppFiles.deleteTree(new File(d, n));
        Wui.toast(this, Lang.s("Imported data deleted", "Dados importados apagados"));
        renderList();
    }

    private void createDocument(String name, int req) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_TITLE, name);
        try {
            startActivityForResult(i, req);
        } catch (RuntimeException e) {
            Wui.toast(this, e.getMessage());
        }
    }

    private void openDocument(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, req);
        } catch (RuntimeException e) {
            Wui.toast(this, e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        try {
            if (req == REQ_DIAG) {
                try (OutputStream out = getContentResolver().openOutputStream(uri)) {
                    Diagnostics.write(this, out);
                }
                Wui.toast(this, Lang.s("Diagnostics saved", "Diagnóstico salvo"));
            } else if (req == REQ_SAVES_OUT) {
                try (OutputStream out = getContentResolver().openOutputStream(uri); ZipOutputStream z = new ZipOutputStream(out)) {
                    int n = zipTree(z, new File(data(), "user/save"), "user/save/");
                    Wui.toast(this, String.format(Locale.US, Lang.s("%d save files exported", "%d arquivos de save exportados"), n));
                }
            } else if (req == REQ_SAVES_IN) {
                WCard.confirm(this, Lang.s("Import saves?", "Importar saves?"),
                        Lang.s("Files with the same name in user/save are replaced.", "Arquivos com o mesmo nome em user/save serão substituídos."),
                        Lang.s("Import", "Importar"), Lang.s("Cancel", "Cancelar"), new Runnable() {
                            @Override
                            public void run() {
                                importSaves(uri);
                            }
                        });
            }
        } catch (IOException | RuntimeException e) {
            WCard.alert(this, Lang.s("Failed", "Falhou"), String.valueOf(e.getMessage()));
        }
    }

    private static int zipTree(ZipOutputStream z, File dir, String prefix) throws IOException {
        File[] fs = dir.listFiles();
        if (fs == null) return 0;
        int n = 0;
        byte[] buf = new byte[65536];
        for (File f : fs) {
            if (f.isDirectory()) {
                n += zipTree(z, f, prefix + f.getName() + "/");
                continue;
            }
            z.putNextEntry(new ZipEntry(prefix + f.getName()));
            try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
                int r;
                while ((r = in.read(buf)) > 0) z.write(buf, 0, r);
            }
            z.closeEntry();
            n++;
        }
        return n;
    }

    private void importSaves(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            File root = new File(data(), "user/save");
            String canon = root.getCanonicalPath() + File.separator;
            ZipEntry e;
            int n = 0;
            byte[] buf = new byte[65536];
            while ((e = z.getNextEntry()) != null) {
                String name = e.getName();
                if (e.isDirectory() || !name.startsWith("user/save/")) continue;
                File out = new File(data(), name);
                if (!out.getCanonicalPath().startsWith(canon)) continue;
                out.getParentFile().mkdirs();
                try (OutputStream os = new java.io.FileOutputStream(out)) {
                    int r;
                    while ((r = z.read(buf)) > 0) os.write(buf, 0, r);
                }
                n++;
            }
            Wui.toast(this, String.format(Locale.US, Lang.s("%d save files imported", "%d arquivos de save importados"), n));
        } catch (IOException | RuntimeException e) {
            WCard.alert(this, Lang.s("Failed", "Falhou"), String.valueOf(e.getMessage()));
        }
    }
}
