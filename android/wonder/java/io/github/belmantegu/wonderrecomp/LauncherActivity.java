package io.github.belmantegu.wonderrecomp;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/**
 * The home screen of the mockup (logo, status pill, Play, Files, Settings, the Projeto Belmante badge) and the two
 * import cards (choose, progress) over the living background. Play starts GameActivity.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_OPTIONS = 4, REQ_ZIP = 1, REQ_FOLDER = 5;
    private static final String CHANNEL = "https://www.youtube.com/@belmante";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private LauncherBackground background;
    private FrameLayout root;
    private WParts.Pill pill;
    private WButton play;
    private FrameLayout cardLayer;
    private boolean cardChoose;
    private String builtLang = "";
    private volatile long fileCount = -1, fileBytes;
    private boolean counting;
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refresh();
            ImportJob j = ImportJob.current();
            if (j != null && !(j.finished && j.acknowledged)) ui.postDelayed(this, 250);
        }
    };

    private boolean importing() {
        ImportJob j = ImportJob.current();
        return j != null && !j.finished;
    }

    /** 0 ready, 1 incomplete, 2 not imported. */
    private int dataState() {
        if (AppFiles.ready(this)) return 0;
        return AppFiles.hasAnalysis(this) || AppFiles.hasRomfs(this) ? 1 : 2;
    }

    private boolean canPlay() {
        return dataState() == 0 && !importing() && AppFiles.hasGameLib(this);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        AppFiles.dropLegacyDataRoot(this);
        AppFiles.installSharedFont(this);
        Settings.applyLanguage(this);
        Wui.init(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        build();
    }

    private void build() {
        builtLang = Lang.s("en", "pt");
        float em = Wui.em(1);
        root = new FrameLayout(this);
        root.setBackgroundColor(Wui.CREAM);
        background = new LauncherBackground(this);
        root.addView(background, new FrameLayout.LayoutParams(-1, -1));

        // the cream wash behind the left column
        View wash = new View(this);
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xF2FFFBEA, 0xDDFFFBEA, 0x00FFFBEA});
        wash.setBackground(g);
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        int screenW = Math.max(dm.widthPixels, dm.heightPixels);
        root.addView(wash, new FrameLayout.LayoutParams(Math.round(screenW * 0.56f), -1));

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_VERTICAL);
        int colW = Math.round(em * 23f);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(colW, -1);
        lp.leftMargin = Math.round(screenW * 0.035f);
        lp.topMargin = Math.round(screenW * 0.02f);
        lp.bottomMargin = Math.round(screenW * 0.04f);
        root.addView(left, lp);

        Bitmap logo = Wui.asset(this, "launcher/logo.png");
        ImageView logoView = new ImageView(this);
        logoView.setImageBitmap(logo);
        logoView.setAdjustViewBounds(true);
        logoView.setScaleType(ImageView.ScaleType.FIT_START);
        logoView.setContentDescription("Super Mario Bros. Wonder");
        LinearLayout.LayoutParams ll = new LinearLayout.LayoutParams(Math.round(colW * 0.68f), -2);
        ll.bottomMargin = Math.round(em * 0.2f);
        left.addView(logoView, ll);

        pill = new WParts.Pill(this, em);
        pill.setOnClickListener(v -> showDetails());
        left.addView(pill, spaced(-2, -2, em * 0.8f, Gravity.START));

        float fs = Wui.em(1.05f);
        play = new WButton(this, WButton.Style.RED, fs, false).icon("play").text(Lang.s("PLAY", "JOGAR"),
                Lang.s("Start Super Mario Bros. Wonder", "Iniciar Super Mario Bros. Wonder"));
        play.setOnClickListener(v -> onPlay());
        left.addView(play, spaced(-1, -2, em * 0.8f, 0));

        LinearLayout row = new LinearLayout(this);
        WButton files = new WButton(this, WButton.Style.WONDER, fs, false).icon("folder-open").text(Lang.s("Files", "Arquivos"), "XCI / NSP");
        files.setOnClickListener(v -> showChoose());
        WButton opts = new WButton(this, WButton.Style.BLUE, fs, false).icon("settings")
                .text(Lang.s("Settings", "Configurações"), Lang.s("Image, performance...", "Imagem, desempenho…"));
        opts.setOnClickListener(v -> startActivityForResult(new Intent(this, OptionsActivity.class), REQ_OPTIONS));
        LinearLayout.LayoutParams a = new LinearLayout.LayoutParams(0, -2, 1f), c = new LinearLayout.LayoutParams(0, -2, 1f);
        c.leftMargin = Math.round(em);
        row.addView(files, a);
        row.addView(opts, c);
        left.addView(row, spaced(-1, -2, em * 0.8f, 0));
        // the row of two is wider than the column in the mockup too (it fills the 23em column)

        TextView foot = new TextView(this);
        foot.setText(Lang.s("Projeto Belmante  ·  no game files, no keys  ·  you bring your own dump",
                "Projeto Belmante  ·  sem arquivos do jogo, sem keys  ·  você traz o seu próprio dump"));
        foot.setTypeface(Wui.medium());
        foot.setTextColor(0xC02b2d33);
        foot.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, em * 0.62f);
        FrameLayout.LayoutParams fl = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.START);
        fl.leftMargin = Math.round(screenW * 0.035f);
        fl.bottomMargin = Math.round(screenW * 0.02f * 0.5f);
        root.addView(foot, fl);

        WParts.Badge badge = new WParts.Badge(this, Wui.asset(this, "links/belmante.jpg"), em);
        badge.setOnClickListener(v -> openChannel());
        FrameLayout.LayoutParams bl = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.END);
        bl.rightMargin = Math.round(screenW * 0.026f);
        bl.bottomMargin = Math.round(screenW * 0.04f * 0.6f);
        root.addView(badge, bl);

        cardLayer = new FrameLayout(this);
        cardLayer.setVisibility(View.GONE);
        cardLayer.setBackgroundColor(0x992b2d33);
        cardLayer.setClickable(true);
        root.addView(cardLayer, new FrameLayout.LayoutParams(-1, -1));

        setContentView(root);
        Wui.hideSystemBars(this);
        play.requestFocus();
        refresh();
    }

    private static LinearLayout.LayoutParams spaced(int w, int h, float top, int gravity) {
        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(w, h);
        l.topMargin = Math.round(top);
        if (gravity != 0) l.gravity = gravity;
        return l;
    }

    private void openChannel() {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(CHANNEL)));
        } catch (RuntimeException e) {
            Wui.toast(this, CHANNEL);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) Wui.hideSystemBars(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Settings.applyLanguage(this);
        if (!builtLang.equals(Lang.s("en", "pt"))) build();
        background.setRunning(true);
        fileCount = -1;
        ui.removeCallbacks(tick);
        ui.post(tick);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(tick);
        background.setRunning(false);
        super.onPause();
    }

    // ---- state ----

    private void countFiles() {
        if (fileCount >= 0 || counting || !AppFiles.ready(this)) return;
        counting = true;
        final File dir = AppFiles.dataDir(this);
        new Thread(new Runnable() {
            @Override
            public void run() {
                long[] t = new long[2];
                AppFiles.tally(dir, t);
                fileBytes = t[1];
                fileCount = t[0];
                counting = false;
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        refresh();
                    }
                });
            }
        }, "wonder-count").start();
    }

    private void refresh() {
        final ImportJob j = ImportJob.current();
        if (j != null && !j.finished) {
            showProgress(j);
            pill.set(Lang.s("Importing...", "Importando…") + String.format(Locale.US, " %d%%", percent(j)), Wui.YELLOW);
            play.setEnabled(false);
            return;
        }
        if (j != null && !j.acknowledged) {
            j.acknowledged = true;
            fileCount = -1;
            hideCard();
            ui.post(new Runnable() {
                @Override
                public void run() {
                    importFinished(j);
                }
            });
        }
        int st = dataState();
        play.setEnabled(canPlay());
        String version = "v1.0.0";
        if (st == 2) {
            pill.set(Lang.s("Game not imported", "Jogo não importado"), Wui.RED);
        } else if (st == 1) {
            pill.set(Lang.s("Game data incomplete", "Dados do jogo incompletos"), Wui.YELLOW);
        } else if (!AppFiles.hasGameLib(this)) {
            pill.set(Lang.s("Imported, but this APK has no game library", "Importado, mas este APK não tem a biblioteca do jogo"), Wui.YELLOW);
        } else if (Diagnostics.lastEndedAbnormally(this)) {
            pill.set(Lang.s("Game ready · last session ended unexpectedly", "Jogo pronto · a última sessão terminou de forma inesperada"), Wui.YELLOW);
        } else {
            pill.set(Lang.s("Game ready · ", "Jogo pronto · ") + version, Wui.GREEN);
            countFiles();
        }
    }

    private int percent(ImportJob j) {
        if (j.total <= 0) return 0;
        return (int) Math.min(100, j.done * 100 / j.total);
    }

    private void importFinished(ImportJob j) {
        if (j.error != null) {
            WCard.alert(this, j.cancelled ? Lang.s("Import cancelled", "Importação cancelada") : Lang.s("Import failed", "Falha na importação"), j.error);
        } else if (j.dump && !AppFiles.hasShaders(this)) {
            WCard.alert(this, Lang.s("Game files imported", "Arquivos do jogo importados"),
                    Lang.s("The game files are in place, but this APK has no translated shaders (assets/spirv.zip), so the game cannot start yet. Build the APK with the shaders (tools: see docs/WONDER-ANDROID-OPTIONS.md).",
                            "Os arquivos do jogo estão no lugar, mas este APK não tem os shaders traduzidos (assets/spirv.zip), então o jogo ainda não inicia. Gere o APK com os shaders (veja docs/WONDER-ANDROID-OPTIONS.md)."));
        } else if (j.result != null) {
            Wui.toast(this, String.format(Locale.US, Lang.s("Import done: %d files (%d copied, %d already there) in %.0f s",
                    "Importação concluída: %d arquivos (%d copiados, %d já existiam) em %.0f s"),
                    j.result.files, j.result.copied, j.result.skipped, j.seconds));
        } else {
            Wui.toast(this, String.format(Locale.US, Lang.s("Import done in %.0f s: the game is ready", "Importação concluída em %.0f s: o jogo está pronto"), j.seconds));
        }
        refresh();
    }

    // ---- play ----

    private void onPlay() {
        if (importing()) {
            Wui.toast(this, Lang.s("The import is still running", "A importação ainda está rodando"));
            return;
        }
        if (dataState() != 0) {
            if (dataState() == 1 && AppFiles.hasAnalysis(this) && AppFiles.hasRomfs(this) && ImportJob.shadersMissing(this, AppFiles.dataDir(this))) {
                installShaders();
                return;
            }
            Wui.toast(this, Lang.s("Import the game first: Files", "Importe o jogo primeiro: Arquivos"));
            showChoose();
            return;
        }
        if (!AppFiles.hasGameLib(this)) {
            WCard.alert(this, Lang.s("No game library", "Sem a biblioteca do jogo"),
                    Lang.s("This APK was built without libwonder.so (launcher-only build), so Play cannot start the game.",
                            "Este APK foi gerado sem o libwonder.so (build só do launcher), então Jogar não consegue iniciar o jogo."));
            return;
        }
        if (ImportJob.shadersMissing(this, AppFiles.dataDir(this))) {
            installShaders();
            return;
        }
        try {
            AppFiles.installConfig(this);
        } catch (java.io.IOException ignored) {
        }
        startActivity(new Intent(this, GameActivity.class));
    }

    private void installShaders() {
        ImportJob.startShaders(this, AppFiles.dataDir(this));
        Wui.toast(this, Lang.s("Installing this version's shaders; press Play again when done", "Instalando os shaders desta versão; toque em Jogar de novo ao terminar"));
        ui.removeCallbacks(tick);
        ui.post(tick);
    }

    // ---- import cards ----

    private void showCard(WCard card) {
        cardLayer.removeAllViews();
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.round(Wui.em(44f)), -2, Gravity.CENTER);
        cardLayer.addView(card, lp);
        cardLayer.setVisibility(View.VISIBLE);
    }

    private void hideCard() {
        cardLayer.setVisibility(View.GONE);
        cardLayer.removeAllViews();
        cardChoose = false;
    }

    private void showChoose() {
        if (importing()) {
            showProgress(ImportJob.current());
            return;
        }
        cardChoose = true;
        float em = Wui.em(1);
        WCard card = new WCard(this);
        card.addView(WCard.title(this, Lang.s("Import the game", "Importar o jogo")));
        TextView p = WCard.body(this, Lang.s(
                "The launcher does not include the game, keys or firmware. Choose the folder that has YOUR prod.keys (+ title.keys) and your dump of Super Mario Bros. Wonder 1.0.0 (the cartridge .xci, or .nsp). Put only the 1.0.0 dump there, no updates: the recompiled code is for 1.0.0. The phone reads the files where they are and extracts them to the app's own folder.",
                "O launcher não traz o jogo, nem keys, nem firmware. Escolha a pasta que tem o SEU prod.keys (+ title.keys) e o seu dump do Super Mario Bros. Wonder 1.0.0 (o cartucho .xci, ou .nsp). Coloque só o dump 1.0.0 lá, sem atualizações: o código recompilado é da 1.0.0. O celular lê os arquivos onde estão e extrai para a pasta própria do app."));
        LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(-1, -2);
        pl.topMargin = Math.round(em);
        card.addView(p, pl);
        LinearLayout steps = new LinearLayout(this);
        steps.setOrientation(LinearLayout.VERTICAL);
        String[][] s = {
            {Lang.s("Choose the dump", "Escolher o dump"), Lang.s(".xci (cartridge) or .nsp, in the folder or below it", ".xci (cartucho) ou .nsp, na pasta ou abaixo dela")},
            {Lang.s("prod.keys in the same folder", "prod.keys na mesma pasta"), Lang.s("from your own Switch (Lockpick_RCM)", "da sua própria Switch (Lockpick_RCM)")},
            {Lang.s("Extract", "Extrair"), Lang.s("about 3.7 GB · continues where it stopped if interrupted", "≈ 3,7 GB · continua de onde parou se for interrompido")}};
        for (int i = 0; i < 3; i++) {
            LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(-1, -2);
            sl.topMargin = Math.round(em * 0.55f);
            steps.addView(WParts.step(this, em, String.valueOf(i + 1), false, s[i][0], s[i][1]), sl);
        }
        LinearLayout.LayoutParams stl = new LinearLayout.LayoutParams(-1, -2);
        stl.topMargin = Math.round(em * 0.45f);
        card.addView(steps, stl);
        LinearLayout btns = new LinearLayout(this);
        float fs = Wui.em(1.05f);
        WButton pick = new WButton(this, WButton.Style.RED, fs, true).text(Lang.s("Choose the folder", "Escolher a pasta"), null);
        pick.setOnClickListener(v -> pickFolder());
        WButton zip = new WButton(this, WButton.Style.GHOST, fs, true).text(Lang.s("A data .zip", "Um .zip de dados"), null);
        zip.setOnClickListener(v -> pickZip());
        WButton cancel = new WButton(this, WButton.Style.GHOST, fs, true).text(Lang.s("Cancel", "Cancelar"), null);
        cancel.setOnClickListener(v -> hideCard());
        for (int i = 0; i < 3; i++) {
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(0, -2, 1f);
            if (i > 0) bp.leftMargin = Math.round(em * 0.9f);
            btns.addView(i == 0 ? pick : i == 1 ? zip : cancel, bp);
        }
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(-1, -2);
        bl.topMargin = Math.round(em);
        card.addView(btns, bl);
        showCard(card);
        pick.requestFocus();
    }

    private void showProgress(final ImportJob j) {
        // rebuilt every tick: cheap, and the steps change state
        float em = Wui.em(1);
        WCard card = new WCard(this);
        card.addView(WCard.title(this, Lang.s("Importing...", "Importando…")));
        LinearLayout steps = new LinearLayout(this);
        steps.setOrientation(LinearLayout.VERTICAL);
        String ph = j.phase == null ? "" : j.phase;
        if (j.dump) {
            steps.addView(WParts.step(this, em, j.keysOk ? "✓" : "1", j.keysOk, j.keysOk ? Lang.s("keys read", "keys lidas") : Lang.s("Reading the keys", "Lendo as keys"),
                    j.keysOk ? "prod.keys · ok" : ""), stepLp(em));
            boolean opened = j.containerOk;
            steps.addView(WParts.step(this, em, opened ? "✓" : "2", opened,
                    opened ? (j.containerName.isEmpty() ? Lang.s("dump opened", "dump aberto") : j.containerName + Lang.s(" opened", " aberto")) : Lang.s("Opening the dump", "Abrindo o dump"),
                    opened ? "" : j.containerName), stepLp(em));
            steps.addView(WParts.step(this, em, "3", false, phaseLabel(ph),
                    String.format(Locale.US, Lang.s("%.2f GB of %.2f GB", "%.2f GB de %.2f GB"), j.done / 1.073741824e9, j.total / 1.073741824e9)
                            + (j.currentName == null || j.currentName.isEmpty() ? "" : "  ·  " + shortName(j.currentName))), stepLp(em));
        } else {
            steps.addView(WParts.step(this, em, "1", false, Lang.s("Copying the data pack", "Copiando o pacote de dados"),
                    String.format(Locale.US, Lang.s("%.2f GB · file %d", "%.2f GB · arquivo %d"), j.done / 1.073741824e9, j.files)), stepLp(em));
        }
        LinearLayout.LayoutParams stl = new LinearLayout.LayoutParams(-1, -2);
        stl.topMargin = Math.round(em);
        card.addView(steps, stl);
        WParts.Progress bar = new WParts.Progress(this, em);
        int pct = percent(j);
        bar.set(j.total > 0 ? j.done / (float) j.total : 0f, j.total > 0 ? pct + "%" : "...");
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(-1, -2);
        bl.topMargin = Math.round(em);
        card.addView(bar, bl);
        WButton cancel = new WButton(this, WButton.Style.GHOST, Wui.em(1.05f), true).text(Lang.s("Cancel the import", "Cancelar importação"), null);
        cancel.setOnClickListener(v -> WCard.confirm(this, Lang.s("Cancel the import?", "Cancelar a importação?"),
                Lang.s("The files already written are kept; the next import continues from them.", "Os arquivos já gravados são mantidos; a próxima importação continua deles."),
                Lang.s("Cancel the import", "Cancelar a importação"), Lang.s("Keep going", "Continuar"), new Runnable() {
                    @Override
                    public void run() {
                        j.requestCancel();
                    }
                }));
        LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(-1, -2);
        cl.topMargin = Math.round(em);
        card.addView(cancel, cl);
        boolean wasFocused = cancel.hasFocus();
        showCard(card);
        if (cardChoose || wasFocused) cancel.requestFocus();
        cardChoose = false;
    }

    private static LinearLayout.LayoutParams stepLp(float em) {
        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(-1, -2);
        l.topMargin = Math.round(em * 0.55f);
        return l;
    }

    private static String phaseLabel(String phase) {
        switch (phase) {
            case "scan": return Lang.s("Reading the dump", "Lendo o dump");
            case "exefs": return Lang.s("Extracting ExeFS (program)", "Extraindo o ExeFS (programa)");
            case "romfs": return Lang.s("Extracting RomFS", "Extraindo o RomFS");
            case "verify": return Lang.s("Verifying", "Verificando");
            case "shaders": return Lang.s("Installing shaders", "Instalando os shaders");
            case "done": return Lang.s("Done", "Pronto");
            default: return Lang.s("Extracting", "Extraindo");
        }
    }

    private static String shortName(String name) {
        String n = name.substring(name.lastIndexOf('/') + 1);
        return n.length() > 30 ? n.substring(0, 29) + "…" : n;
    }

    private void pickFolder() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
        } catch (RuntimeException e) {
            Wui.toast(this, Lang.s("No folder chooser available: ", "Nenhum seletor de pastas disponível: ") + e.getMessage());
        }
    }

    private void pickZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        try {
            startActivityForResult(i, REQ_ZIP);
        } catch (RuntimeException e) {
            Wui.toast(this, Lang.s("No file chooser available: ", "Nenhum seletor de arquivos disponível: ") + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_OPTIONS) {
            if (res == RESULT_OK && data != null && "import".equals(data.getStringExtra("action"))) showChoose();
            return;
        }
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        if (req == REQ_FOLDER) ImportJob.startDump(this, data.getData(), AppFiles.dataDir(this));
        else if (req == REQ_ZIP) ImportJob.startZip(this, data.getData(), AppFiles.dataDir(this));
        else return;
        ui.removeCallbacks(tick);
        ui.post(tick);
    }

    @Override
    public void onBackPressed() {
        if (cardLayer.getVisibility() == View.VISIBLE && !importing()) {
            hideCard();
            return;
        }
        super.onBackPressed();
    }

    // ---- details (tap on the status pill) ----

    private void showDetails() {
        File dir = AppFiles.dataDir(this);
        String found = Lang.s("found", "encontrado"), missing = Lang.s("missing", "ausente");
        StringBuilder sb = new StringBuilder();
        sb.append(Lang.s("Game data folder:\n", "Pasta dos dados do jogo:\n")).append(dir.getAbsolutePath()).append("\n\n");
        sb.append("analysis/main.bin, sdk.bin: ").append(AppFiles.hasAnalysis(this) ? found : missing).append('\n');
        sb.append("extract/romfs: ").append(AppFiles.hasRomfs(this) ? found : missing).append('\n');
        sb.append("spirv/results.tsv: ").append(AppFiles.hasShaders(this) ? found : missing)
                .append(ImportJob.apkHasShaders(this) ? "" : Lang.s("  (this APK has no assets/spirv.zip)", "  (este APK não tem assets/spirv.zip)")).append('\n');
        sb.append("libwonder.so: ").append(AppFiles.hasGameLib(this) ? found : missing).append('\n');
        if (fileCount >= 0)
            sb.append(String.format(Locale.US, Lang.s("\n%d files, %.1f GB\n", "\n%d arquivos, %.1f GB\n"), fileCount, fileBytes / 1.073741824e9));
        String exits = Diagnostics.exits(this);
        if (!exits.isEmpty()) sb.append("\n").append(Lang.s("What Android recorded for this app:\n", "O que o Android registrou para este app:\n")).append(exits);
        sb.append("\n").append(Lang.s("Log of the last run: ", "Log da última partida: ")).append(new File(dir, "nx.log").getAbsolutePath());
        WCard.show(this, Lang.s("Details", "Detalhes"), sb.toString(), null, new WCard.Action("OK", WButton.Style.RED, null));
    }
}
