package io.github.belmantegu.wonderrecomp;

/**
 * The on-device importer (android/wonder/importer, libnximport.so): the player's own prod.keys/title.keys and the dump
 * (XCI, or NSPs) -> analysis/*.bin and extract/romfs/ in the app's data folder. Streams, checks every block against the
 * NCA's own hashes, resumes where it stopped, never logs key values. The JNI names are fixed by this class.
 */
final class NativeImporter {
    /** Super Mario Bros. Wonder 1.0.0 (games/wonder/game.cpp). */
    static final String TITLE_ID = "010015100B514000";

    interface Callback {
        void progress(String phase, long done, long total, String item);

        void log(String line);
    }

    private static boolean loaded;
    private static String loadError;

    static synchronized boolean load() {
        if (!loaded && loadError == null) {
            try {
                System.loadLibrary("nximport");
                loaded = true;
            } catch (Throwable t) {
                loadError = String.valueOf(t);
            }
        }
        return loaded;
    }

    static String loadError() {
        return loadError;
    }

    /** 0 on success, else see lastError(); 4 = cancelled. The fds stay open (the caller closes them). */
    static native int run(String outDir, String[] names, int[] fds, String titleId, boolean verify, Callback cb);

    static native void cancel();

    static native String lastError();

    private NativeImporter() {
    }
}
