package com.openport.galaxyandroid;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.method.ScrollingMovementMethod;
import android.util.AtomicFile;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class MainActivity extends Activity {
    private static final String TAG = "GalaxyAndroid";
    private static final String PREFS_NAME = "GalaxyPrefs";
    private static final Object LAUNCH_DIAGNOSTICS_LOCK = new Object();
    private static final long MAX_LAUNCH_DIAGNOSTICS_BYTES = 1024L * 1024L;
    private static final int REPORT_MAX_LINES = 5000;
    private static final int REPORT_NATIVE_MAX_BYTES = 256 * 1024;
    private static final int REPORT_OTHER_MAX_BYTES = 32 * 1024;
    private static final int REPORT_TRACE_MAX_BYTES = 24 * 1024;
    private static final int REQ_PICK_DISC = 1001;
    private static final int REQ_PICK_DRIVER = 1002;

    // Header & State
    private TextView txtHeaderTitle;
    private TextView txtHeaderSubtitle;
    private TextView txtGameStatusBadge;
    private TextView txtGameStatusDetail;

    // Disc Installer
    private TextView txtSourcePath;
    private CheckBox chkIncludeMovies;
    private Button btnSelectDisc;
    private Button btnAutoDetect;
    private Button btnStartCooking;
    private Button btnLaunchGame;
    private Button btnResetData;
    private Button btnBrowseGameFiles;
    private Button btnRes480;
    private Button btnRes720;
    private Button btnRes1080;
    private Button btnRes1440;
    private Button btnRes4K;
    private TextView txtQualityCurrent;

    // GPU Driver Manager
    private TextView txtGpuHardwareInfo;
    private TextView txtDriverStatusBadge;
    private TextView txtDriverDetail;
    private Button btnInstallDriver;
    private Button btnResetDriver;

    // Console & Progress
    private ProgressBar progressBar;
    private TextView txtProgressStatus;
    private TextView txtLogOutput;
    private Button btnClearLog;
    private Button btnCopyStartupReport;
    private Button btnViewLicenses;
    private Button btnViewChangelog;

    private String mSelectedSourcePath = "";
    private Uri mSelectedSourceUri = null;
    private String mSelectedDisplayName = "";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mIsCooking = false;
    private SharedPreferences mPrefs;

    public static boolean isGameDataReady() {
        File dataDir = new File("/sdcard/GalaxyData");
        if (!dataDir.exists()) {
            dataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
            if (!dataDir.exists()) return false;
        }
        File sentinel = new File(dataDir, ".installed_ok");
        File fst = new File(dataDir, "sys/fst.bin");
        File files = new File(dataDir, "files");
        File coin = new File(dataDir, "files/ObjectData/Coin.arc");
        File errorMessages = new File(dataDir, "sys/ErrorMessageArchive.arc");
        File storyEvents = new File(dataDir, "sys/StoryEvent.bcsv");
        File galaxyIds = new File(dataDir, "sys/GalaxyID.bcsv");
        if (!fst.isFile() || !files.isDirectory() || !coin.isFile() ||
            !errorMessages.isFile() || !storyEvents.isFile() || !galaxyIds.isFile()) {
            return false;
        }
        // Verify Coin.arc is converted to little-endian ARM64 ("CRAR")
        try (java.io.FileInputStream fis = new java.io.FileInputStream(coin)) {
            byte[] header = new byte[4];
            if (fis.read(header) == 4 && header[0] == 'C' && header[1] == 'R' && header[2] == 'A' && header[3] == 'R') {
                if (!sentinel.exists()) {
                    try { sentinel.createNewFile(); } catch (Exception ignored) {}
                }
                return true;
            }
        } catch (Exception ignored) {}
        return false;
    }

    public static void recordLaunchDiagnostic(Context context, String stage, Throwable error) {
        if (context == null || stage == null) return;
        StringBuilder entry = new StringBuilder()
            .append(System.currentTimeMillis()).append(" ").append(stage);
        if (error != null) {
            String stack = Log.getStackTraceString(error);
            if (stack != null && !stack.isEmpty()) entry.append('\n').append(stack);
        }
        entry.append('\n');
        byte[] bytes = entry.toString().getBytes(StandardCharsets.UTF_8);
        synchronized (LAUNCH_DIAGNOSTICS_LOCK) {
            File file = new File(context.getFilesDir(), "launch_bootstrap.log");
            if (file.length() + bytes.length > MAX_LAUNCH_DIAGNOSTICS_BYTES) {
                File previous = new File(file.getParentFile(), "launch_bootstrap.log.prev");
                previous.delete();
                if (!file.renameTo(previous)) file.delete();
            }
            try (FileOutputStream stream = new FileOutputStream(file, true)) {
                stream.write(bytes);
                stream.flush();
            } catch (Exception writeError) {
                Log.e(TAG, "Could not persist launch stage: " + stage, writeError);
            }
        }
        Log.i(TAG, "launch stage: " + stage);
    }

    public static void recordLaunchDiagnostic(Context context, String stage) {
        recordLaunchDiagnostic(context, stage, null);
    }

    private static String readLastLines(File file, int maxLines, int maxBytes) {
        if (file == null || !file.isFile() || maxLines <= 0 || maxBytes <= 0) return "";
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long fileLength = input.length();
            int length = (int)Math.min((long)maxBytes, fileLength);
            input.seek(Math.max(0L, fileLength - length));
            byte[] bytes = new byte[length];
            input.readFully(bytes);
            String text = new String(bytes, StandardCharsets.UTF_8);
            boolean clipped = fileLength > length;
            if (clipped) {
                int firstLineEnd = text.indexOf('\n');
                text = firstLineEnd >= 0 ? text.substring(firstLineEnd + 1) : "";
            }
            String[] lines = text.split("\n", -1);
            int lineCount = lines.length;
            if (lineCount > 0 && lines[lineCount - 1].isEmpty()) lineCount--;
            int firstLine = Math.max(0, lineCount - maxLines);
            StringBuilder result = new StringBuilder(Math.min(length + 64, maxBytes + 64));
            if (clipped) result.append("[...] registro recortado por límite de tamaño\n");
            for (int i = firstLine; i < lineCount; i++) {
                if (i > firstLine) result.append('\n');
                result.append(lines[i].replace("\r", ""));
            }
            if (lineCount > 0) result.append('\n');
            return result.toString();
        } catch (Exception ignored) {
            return "";
        }
    }

    private static int countLogLines(String text) {
        int count = 0;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) == '\n') count++;
        }
        return count;
    }

    private static String firstReadableLog(File... candidates) {
        for (File file : candidates) {
            String text = readLastLines(file, REPORT_MAX_LINES, REPORT_NATIVE_MAX_BYTES);
            if (!text.isEmpty()) return text;
        }
        return "";
    }

    private static int exitReasonConstant(Class<?> exitInfoClass, String name, int fallback) {
        try {
            return exitInfoClass.getField(name).getInt(null);
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private static String processExitReason(Class<?> exitInfoClass, int reason) {
        if (reason == exitReasonConstant(exitInfoClass, "REASON_ANR", 6)) return "ANR";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_CRASH", 4)) return "Java crash";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_CRASH_NATIVE", 5)) return "native crash";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_LOW_MEMORY", 3)) return "low memory";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_SIGNALED", 2)) return "signal";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_EXIT_SELF", 1)) return "self exit";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_INITIALIZATION_FAILURE", 7)) return "initialization failure";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_EXCESSIVE_RESOURCE_USAGE", 9)) return "excessive resource usage";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_USER_REQUESTED", 10)) return "user requested";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_USER_STOPPED", 11)) return "user stopped";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_DEPENDENCY_DIED", 12)) return "dependency died";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_PERMISSION_CHANGE", 8)) return "permission change";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_PACKAGE_UPDATED", 16)) return "package updated";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_PACKAGE_STATE_CHANGE", 15)) return "package state change";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_FREEZER", 14)) return "system freezer";
        if (reason == exitReasonConstant(exitInfoClass, "REASON_UNKNOWN", 0)) return "unknown";
        return "other(" + reason + ")";
    }

    private static long exitNumber(Object exit, String method) throws ReflectiveOperationException {
        Object value = exit.getClass().getMethod(method).invoke(exit);
        return value instanceof Number ? ((Number)value).longValue() : 0L;
    }

    private static String exitText(Object exit, String method) throws ReflectiveOperationException {
        Object value = exit.getClass().getMethod(method).invoke(exit);
        return value == null ? "" : value.toString();
    }

    private static String readExitTrace(Object exit) {
        if (exit == null) return "";
        try {
            Object traceValue = exit.getClass().getMethod("getTraceInputStream").invoke(exit);
            if (!(traceValue instanceof InputStream)) return "";
            try (InputStream input = (InputStream)traceValue;
                 ByteArrayOutputStream output = new ByteArrayOutputStream(REPORT_TRACE_MAX_BYTES)) {
                byte[] buffer = new byte[4096];
                int remaining = REPORT_TRACE_MAX_BYTES;
                while (remaining > 0) {
                    int count = input.read(buffer, 0, Math.min(buffer.length, remaining));
                    if (count < 0) break;
                    output.write(buffer, 0, count);
                    remaining -= count;
                }
                String trace = new String(output.toByteArray(), StandardCharsets.UTF_8);
                return trace.isEmpty() ? "" : trace + "\n[traza limitada a 24 KiB]\n";
            }
        } catch (Exception error) {
            return "[Android no permitió leer la traza: " + error.getClass().getSimpleName() + "]\n";
        }
    }

    private void appendProcessExitHistory(StringBuilder report) {
        report.append("\n--- CIERRES REGISTRADOS POR ANDROID ---\n");
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            report.append("API inferior a 30; Android no ofrece este historial.\n");
            return;
        }
        try {
            ActivityManager manager = (ActivityManager)getSystemService(Context.ACTIVITY_SERVICE);
            if (manager == null) {
                report.append("Servicio de actividad no disponible.\n");
                return;
            }
            Class<?> exitInfoClass = Class.forName("android.app.ApplicationExitInfo");
            Object rawExits = ActivityManager.class
                .getMethod("getHistoricalProcessExitReasons", String.class, int.class, int.class)
                .invoke(manager, getPackageName(), 0, 5);
            if (!(rawExits instanceof List) || ((List<?>)rawExits).isEmpty()) {
                report.append("Android no conserva cierres recientes para esta app.\n");
                return;
            }
            Object newestCrash = null;
            for (Object exit : (List<?>)rawExits) {
                int reason = (int)exitNumber(exit, "getReason");
                report.append("time=").append(exitNumber(exit, "getTimestamp"))
                    .append(" reason=").append(processExitReason(exitInfoClass, reason))
                    .append(" status=").append(exitNumber(exit, "getStatus"))
                    .append(" pid=").append(exitNumber(exit, "getPid"))
                    .append(" pssKB=").append(exitNumber(exit, "getPss"))
                    .append(" rssKB=").append(exitNumber(exit, "getRss"))
                    .append(" process=").append(exitText(exit, "getProcessName"))
                    .append(" description=").append(exitText(exit, "getDescription"))
                    .append('\n');
                if (newestCrash == null &&
                    (reason == exitReasonConstant(exitInfoClass, "REASON_CRASH_NATIVE", 5) ||
                     reason == exitReasonConstant(exitInfoClass, "REASON_CRASH", 4) ||
                     reason == exitReasonConstant(exitInfoClass, "REASON_SIGNALED", 2) ||
                     reason == exitReasonConstant(exitInfoClass, "REASON_ANR", 6))) {
                    newestCrash = exit;
                }
            }
            if (newestCrash != null) {
                String trace = readExitTrace(newestCrash);
                if (!trace.isEmpty()) report.append("--- TRAZA NATIVA/JAVA MÁS RECIENTE ---\n").append(trace);
            }
        } catch (Exception error) {
            report.append("No se pudo consultar el historial: ")
                .append(error.getClass().getSimpleName()).append(": ")
                .append(error.getMessage()).append('\n');
        }
    }

    private String buildDiagnosticReport() {
        StringBuilder report = new StringBuilder(16 * 1024);
        String versionName = getInstalledVersionName();
        report.append("Galaxy Android — informe local\n")
            .append("version=").append(versionName)
            .append(" sdk=").append(Build.VERSION.SDK_INT)
            .append(" device=").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append("\nEntradas máximas: ").append(REPORT_MAX_LINES)
            .append(" (el informe para copiar está limitado por seguridad de memoria)")
            .append("\nNo contiene ROM ni partidas.\n");

        File internal = getFilesDir();
        String bootstrapPrevious = readLastLines(
            new File(internal, "launch_bootstrap.log.prev"), 2500, REPORT_OTHER_MAX_BYTES);
        String bootstrapCurrent = readLastLines(
            new File(internal, "launch_bootstrap.log"),
            Math.max(1, REPORT_MAX_LINES - countLogLines(bootstrapPrevious)),
            REPORT_OTHER_MAX_BYTES);
        report.append("\n--- ARRANQUES JAVA/NATIVO (máx. 5.000 líneas) ---\n")
            .append(bootstrapPrevious).append(bootstrapCurrent);

        File nativeCurrent = new File(internal, "galaxy_log.txt");
        File nativePrevious = new File(internal, "galaxy_log.txt.prev");
        String nativeCurrentText = readLastLines(nativeCurrent, REPORT_MAX_LINES, REPORT_NATIVE_MAX_BYTES);
        int nativeRemainingLines = Math.max(1, REPORT_MAX_LINES - countLogLines(nativeCurrentText));
        int nativeRemainingBytes = Math.max(1, REPORT_NATIVE_MAX_BYTES -
            nativeCurrentText.getBytes(StandardCharsets.UTF_8).length);
        String nativePreviousText = readLastLines(nativePrevious, nativeRemainingLines, nativeRemainingBytes);
        if (nativeCurrentText.isEmpty() && nativePreviousText.isEmpty()) {
            File external = getExternalFilesDir(null);
            File legacyExternal = new File("/sdcard/GalaxyData/galaxy_log.txt");
            File legacyScoped = external == null ? null : new File(external, "galaxy_log.txt");
            nativeCurrentText = firstReadableLog(legacyExternal, legacyScoped);
            nativeRemainingLines = Math.max(1, REPORT_MAX_LINES - countLogLines(nativeCurrentText));
            nativeRemainingBytes = Math.max(1, REPORT_NATIVE_MAX_BYTES -
                nativeCurrentText.getBytes(StandardCharsets.UTF_8).length);
            nativePreviousText = readLastLines(
                new File("/sdcard/GalaxyData/galaxy_log.txt.prev"),
                nativeRemainingLines, nativeRemainingBytes);
            if (nativePreviousText.isEmpty() && external != null) {
                nativePreviousText = readLastLines(
                    new File(external, "galaxy_log.txt.prev"),
                    nativeRemainingLines, nativeRemainingBytes);
            }
        }
        report.append("\n--- MOTOR NATIVO (máx. 5.000 líneas entre sesiones) ---\n");
        if (nativePreviousText.isEmpty() && nativeCurrentText.isEmpty()) {
            report.append("Sin archivo de motor legible. internalFiles=")
                .append(internal.getAbsolutePath())
                .append(" currentBytes=").append(nativeCurrent.length())
                .append(" previousBytes=").append(nativePrevious.length()).append('\n');
        } else {
            report.append(nativePreviousText).append(nativeCurrentText);
        }

        String activityPrevious = readLastLines(
            new File(internal, "android_diagnostics.log.prev"), 2500, REPORT_OTHER_MAX_BYTES);
        String activityCurrent = readLastLines(
            new File(internal, "android_diagnostics.log"),
            Math.max(1, REPORT_MAX_LINES - countLogLines(activityPrevious)),
            REPORT_OTHER_MAX_BYTES);
        report.append("\n--- EVENTOS DE ACTIVIDAD/ANDROID ---\n")
            .append(activityPrevious).append(activityCurrent);
        appendProcessExitHistory(report);
        return report.toString();
    }

    private String getInstalledVersionName() {
        try {
            String versionName = getPackageManager()
                .getPackageInfo(getPackageName(), 0).versionName;
            if (versionName != null && !versionName.trim().isEmpty()) {
                return versionName;
            }
        } catch (Exception ignored) {}
        return "unknown";
    }

    private void copyStartupReport() {
        String report = buildDiagnosticReport();
        ClipboardManager clipboard = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard != null) {
            try {
                clipboard.setPrimaryClip(ClipData.newPlainText("Galaxy Android launch report", report));
                Toast.makeText(this, "Informe de hasta 5.000 entradas copiado", Toast.LENGTH_LONG).show();
            } catch (RuntimeException tooLarge) {
                Toast.makeText(this, "Informe grande; revisa la consola para copiarlo por partes", Toast.LENGTH_LONG).show();
            }
        } else {
            Toast.makeText(this, "Portapapeles no disponible; revisa la consola", Toast.LENGTH_LONG).show();
        }
        txtLogOutput.setText(report);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR);

        setContentView(R.layout.activity_main);
        TextView versionBadge = findViewById(R.id.txtVersionBadge);
        if (versionBadge != null) {
            versionBadge.setText("v" + getInstalledVersionName());
        }
        mPrefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        // Header & State
        txtHeaderTitle = findViewById(R.id.txtHeaderTitle);
        txtHeaderSubtitle = findViewById(R.id.txtHeaderSubtitle);
        txtGameStatusBadge = findViewById(R.id.txtGameStatusBadge);
        txtGameStatusDetail = findViewById(R.id.txtGameStatusDetail);

        // Disc Installer
        txtSourcePath = findViewById(R.id.txtSourcePath);
        chkIncludeMovies = findViewById(R.id.chkIncludeMovies);
        btnSelectDisc = findViewById(R.id.btnSelectDisc);
        btnAutoDetect = findViewById(R.id.btnAutoDetect);
        btnStartCooking = findViewById(R.id.btnStartCooking);
        btnLaunchGame = findViewById(R.id.btnLaunchGame);
        btnResetData = findViewById(R.id.btnResetData);
        btnBrowseGameFiles = findViewById(R.id.btnBrowseGameFiles);

        // GPU Driver Manager
        txtGpuHardwareInfo = findViewById(R.id.txtGpuHardwareInfo);
        txtDriverStatusBadge = findViewById(R.id.txtDriverStatusBadge);
        txtDriverDetail = findViewById(R.id.txtDriverDetail);
        btnInstallDriver = findViewById(R.id.btnInstallDriver);
        btnResetDriver = findViewById(R.id.btnResetDriver);

        // Resolution & Performance Presets
        btnRes480 = findViewById(R.id.btnRes480);
        btnRes720 = findViewById(R.id.btnRes720);
        btnRes1080 = findViewById(R.id.btnRes1080);
        btnRes1440 = findViewById(R.id.btnRes1440);
        btnRes4K = findViewById(R.id.btnRes4K);
        txtQualityCurrent = findViewById(R.id.txtQualityCurrent);

        if (btnRes480 != null) btnRes480.setOnClickListener(v -> setResolutionPreset(0));
        if (btnRes720 != null) btnRes720.setOnClickListener(v -> setResolutionPreset(1));
        if (btnRes1080 != null) btnRes1080.setOnClickListener(v -> setResolutionPreset(2));
        if (btnRes1440 != null) btnRes1440.setOnClickListener(v -> setResolutionPreset(3));
        if (btnRes4K != null) btnRes4K.setOnClickListener(v -> setResolutionPreset(4));
        syncResolutionFromNativeSettings();
        refreshResolutionButtons();

        // Console & Progress
        progressBar = findViewById(R.id.progressBar);
        txtProgressStatus = findViewById(R.id.txtProgressStatus);
        txtLogOutput = findViewById(R.id.txtLogOutput);
        btnClearLog = findViewById(R.id.btnClearLog);
        btnCopyStartupReport = findViewById(R.id.btnCopyStartupReport);
        btnViewLicenses = findViewById(R.id.btnViewLicenses);
        btnViewChangelog = findViewById(R.id.btnViewChangelog);

        txtLogOutput.setMovementMethod(new ScrollingMovementMethod());
        recordLaunchDiagnostic(getApplicationContext(), "MainActivity.onCreate");

        // Setup hardware info & driver status
        setupHardwareAndDriverInfo();

        checkStoragePermissions();
        autoDetectSource();
        refreshGameStatus();

        // Listeners
        btnSelectDisc.setOnClickListener(v -> openFilePicker());
        btnAutoDetect.setOnClickListener(v -> autoDetectSource());
        btnInstallDriver.setOnClickListener(v -> openDriverPicker());
        btnResetDriver.setOnClickListener(v -> resetToSystemDriver());

        if (btnClearLog != null) {
            btnClearLog.setOnClickListener(v -> txtLogOutput.setText(""));
        }
        if (btnCopyStartupReport != null) {
            btnCopyStartupReport.setOnClickListener(v -> copyStartupReport());
        }
        if (btnViewLicenses != null) {
            btnViewLicenses.setOnClickListener(v -> showBundledLicenses());
        }
        if (btnViewChangelog != null) {
            btnViewChangelog.setOnClickListener(v -> showChangelog());
        }

        btnStartCooking.setOnClickListener(v -> {
            if (mSelectedSourcePath.isEmpty()) {
                Toast.makeText(this, "Selecciona primero el archivo del juego.", Toast.LENGTH_SHORT).show();
                return;
            }
            startCookingProcess();
        });

        if (btnBrowseGameFiles != null) {
            btnBrowseGameFiles.setOnClickListener(v -> {
                Intent intent = new Intent(this, GameFilesActivity.class);
                intent.putExtra(GameFilesActivity.EXTRA_ROOT_PATH,
                        new File(Environment.getExternalStorageDirectory(), "GalaxyData").getAbsolutePath());
                startActivity(intent);
            });
        }

        btnLaunchGame.setOnClickListener(v -> launchGameDirectly());

        btnResetData.setOnClickListener(v -> {
            new AlertDialog.Builder(this)
                .setTitle("Restablecer datos del juego")
                .setMessage("¿Seguro que quieres borrar /sdcard/GalaxyData? Esto elimina los archivos instalados allí; no borra la calidad ni las preferencias locales.")
                .setPositiveButton("Borrar y restablecer", (dialog, which) -> {
                    File dataDir = new File("/sdcard/GalaxyData");
                    deleteRecursive(dataDir);
                    refreshGameStatus();
                    appendLog("[Reset] Cleared game directory: " + dataDir.getAbsolutePath());
                    Toast.makeText(this, "Datos del juego restablecidos.", Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Cancelar", null)
                .show();
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        recordLaunchDiagnostic(this, "MainActivity.onResume");
        if (txtLogOutput != null) {
            txtLogOutput.setText(buildDiagnosticReport());
        }
        syncResolutionFromNativeSettings();
        refreshGameStatus();
        refreshDriverStatus();
        refreshResolutionButtons();
    }

    private void refreshGameStatus() {
        boolean ready = isGameDataReady();
        if (ready) {
            btnLaunchGame.setVisibility(View.VISIBLE);
            txtGameStatusBadge.setText("LISTO PARA JUGAR");
            txtGameStatusBadge.setBackgroundResource(R.drawable.badge_bg_green);
            txtGameStatusBadge.setTextColor(getColor(R.color.success_green));
            txtGameStatusDetail.setText("Datos verificados en /sdcard/GalaxyData. Pulsa «Iniciar juego» para jugar.");
            txtProgressStatus.setText("Listo para jugar.");
        } else {
            btnLaunchGame.setVisibility(View.GONE);
            txtGameStatusBadge.setText("FALTA INSTALAR");
            txtGameStatusBadge.setBackgroundResource(R.drawable.badge_bg_amber);
            txtGameStatusBadge.setTextColor(getColor(R.color.warning_amber));
            txtGameStatusDetail.setText("No se encontraron datos en /sdcard/GalaxyData. Selecciona un archivo para instalar.");
            txtProgressStatus.setText("Esperando que selecciones un archivo.");
        }
    }

    private int getCurrentResolutionPreset() {
        int fromGameSettings = readRenderScaleFromNativeSettings();
        if (fromGameSettings >= 0) return fromGameSettings;
        int fallback = mPrefs == null ? 2 : mPrefs.getInt("render_scale", 2);
        return Math.max(0, Math.min(4, fallback));
    }

    private void setResolutionPreset(int scale) {
        scale = Math.max(0, Math.min(4, scale));
        if (!updateIniRenderScale(getNativeSettingsFile(), scale)) {
            refreshResolutionButtons();
            Toast.makeText(this, "No se pudo guardar la calidad. Se conservó el ajuste anterior.", Toast.LENGTH_LONG).show();
            return;
        }
        mPrefs.edit().putInt("render_scale", scale).apply();

        refreshResolutionButtons();
        String label = getResolutionLabel(scale);
        appendLog("[Calidad] Ajuste guardado para el juego: " + label);
        Toast.makeText(this, "Calidad guardada: " + label, Toast.LENGTH_SHORT).show();
    }

    private void refreshResolutionButtons() {
        int current = getCurrentResolutionPreset();
        if (txtQualityCurrent != null) {
            txtQualityCurrent.setText(getString(R.string.quality_saved_format, getResolutionLabel(current)));
        }
        if (mPrefs != null) {
            mPrefs.edit().putInt("render_scale", current).apply();
        }
        if (btnRes480 != null) {
            btnRes480.setBackgroundResource(current == 0 ? R.drawable.btn_primary : R.drawable.btn_secondary);
            btnRes480.setTextColor(current == 0 ? 0xFFFFFFFF : getColor(R.color.text_secondary));
        }
        if (btnRes720 != null) {
            btnRes720.setBackgroundResource(current == 1 ? R.drawable.btn_primary : R.drawable.btn_secondary);
            btnRes720.setTextColor(current == 1 ? 0xFFFFFFFF : getColor(R.color.text_secondary));
        }
        if (btnRes1080 != null) {
            btnRes1080.setBackgroundResource(current == 2 ? R.drawable.btn_primary : R.drawable.btn_secondary);
            btnRes1080.setTextColor(current == 2 ? 0xFFFFFFFF : getColor(R.color.text_secondary));
        }
        if (btnRes1440 != null) {
            btnRes1440.setBackgroundResource(current == 3 ? R.drawable.btn_primary : R.drawable.btn_secondary);
            btnRes1440.setTextColor(current == 3 ? 0xFFFFFFFF : getColor(R.color.text_secondary));
        }
        if (btnRes4K != null) {
            btnRes4K.setBackgroundResource(current == 4 ? R.drawable.btn_primary : R.drawable.btn_secondary);
            btnRes4K.setTextColor(current == 4 ? 0xFFFFFFFF : getColor(R.color.text_secondary));
        }
    }

    private File getNativeSettingsFile() {
        return new File(new File(getFilesDir(), "nand"), "touch_settings.ini");
    }

    private int readRenderScaleFromNativeSettings() {
        File iniFile = getNativeSettingsFile();
        if (!iniFile.isFile()) return -1;
        try (java.io.BufferedReader reader = new java.io.BufferedReader(
                new InputStreamReader(new FileInputStream(iniFile), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String trimmed = line.trim();
                if (!trimmed.startsWith("render_scale=")) continue;
                try {
                    return Math.max(0, Math.min(4,
                        Integer.parseInt(trimmed.substring("render_scale=".length()).trim())));
                } catch (NumberFormatException e) {
                    Log.w(TAG, "Invalid render_scale in shared game settings", e);
                    return -1;
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "Could not read shared game settings", e);
        }
        return -1;
    }

    private void syncResolutionFromNativeSettings() {
        if (mPrefs == null) return;
        int saved = readRenderScaleFromNativeSettings();
        if (saved >= 0 && mPrefs.getInt("render_scale", -1) != saved) {
            mPrefs.edit().putInt("render_scale", saved).apply();
            Log.i(TAG, "Launcher quality synchronized from shared game settings: " + getResolutionLabel(saved));
        }
    }

    private String getResolutionLabel(int scale) {
        switch (Math.max(0, Math.min(4, scale))) {
            case 0: return "480p";
            case 1: return "720p";
            case 2: return "1080p";
            case 3: return "1440p";
            default: return "4K";
        }
    }

    private boolean updateIniRenderScale(File iniFile, int scale) {
        AtomicFile atomicFile = new AtomicFile(iniFile);
        FileOutputStream output = null;
        try {
            StringBuilder sb = new StringBuilder();
            boolean found = false;
            if (iniFile.exists()) {
                try (java.io.BufferedReader reader = new java.io.BufferedReader(
                        new InputStreamReader(new FileInputStream(iniFile), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = reader.readLine()) != null) {
                        if (line.trim().startsWith("render_scale=")) {
                            if (!found) {
                                sb.append("render_scale=").append(scale).append('\n');
                                found = true;
                            }
                        } else {
                            sb.append(line).append('\n');
                        }
                    }
                }
            }
            if (!found) {
                sb.append("render_scale=").append(scale).append('\n');
            }
            File parent = iniFile.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw new IOException("Could not create the shared settings directory");
            }
            output = atomicFile.startWrite();
            output.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            output.flush();
            output.getFD().sync();
            atomicFile.finishWrite(output);
            return true;
        } catch (Exception e) {
            if (output != null) atomicFile.failWrite(output);
            Log.e(TAG, "Could not save shared render quality", e);
            if (txtLogOutput != null) appendLog("[Calidad] Error al guardar: " + e.getMessage());
            return false;
        }
    }

    private void showChangelog() {
        StringBuilder text = new StringBuilder();
        try (InputStream input = getAssets().open("changelog/CHANGELOG.es.md");
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            text.append(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (IOException e) {
            text.append("El registro de cambios no se incluyó en esta compilación.\nError: ")
                .append(e.getMessage());
            Log.e(TAG, "Could not read bundled changelog", e);
        }

        TextView content = new TextView(this);
        content.setText(text.toString());
        content.setTextColor(getColor(R.color.text_secondary));
        content.setTextSize(12);
        content.setLineSpacing(3f * getResources().getDisplayMetrics().density, 1f);
        int padding = (int) (16 * getResources().getDisplayMetrics().density + 0.5f);
        content.setPadding(padding, padding, padding, padding);

        ScrollView scrollView = new ScrollView(this);
        int dialogHeight = Math.min(
            (int) (getResources().getDisplayMetrics().heightPixels * 0.72f),
            (int) (560 * getResources().getDisplayMetrics().density + 0.5f));
        scrollView.setLayoutParams(new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dialogHeight));
        scrollView.addView(content, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(this)
            .setTitle("Registro de cambios")
            .setView(scrollView)
            .setPositiveButton("Cerrar", null)
            .show();
    }

    private void showBundledLicenses() {
        StringBuilder text = new StringBuilder();
        appendLicenseDocument(text, "GPL-3.0 · Galaxy Android", "licenses/GPL-3.0.txt");
        appendLicenseDocument(text, "Avisos de terceros", "licenses/THIRD_PARTY_NOTICES.md");
        appendLicenseDocument(text, "CC0 1.0 · Petari / SMGCommunity", "licenses/Petari-CC0-1.0.txt");

        TextView content = new TextView(this);
        content.setText(text.toString());
        content.setTextColor(getColor(R.color.text_secondary));
        content.setTextSize(12);
        int padding = (int) (16 * getResources().getDisplayMetrics().density + 0.5f);
        content.setPadding(padding, padding, padding, padding);

        ScrollView scrollView = new ScrollView(this);
        int dialogHeight = Math.min(
            (int) (getResources().getDisplayMetrics().heightPixels * 0.68f),
            (int) (520 * getResources().getDisplayMetrics().density + 0.5f));
        scrollView.setLayoutParams(new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dialogHeight));
        scrollView.addView(content, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(this)
            .setTitle("Licencias y avisos")
            .setView(scrollView)
            .setPositiveButton("Cerrar", null)
            .show();
    }

    private void appendLicenseDocument(StringBuilder target, String title, String assetPath) {
        target.append("\n\n──────── ").append(title).append(" ────────\n\n");
        try (InputStream input = getAssets().open(assetPath);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            target.append(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (IOException e) {
            target.append("Este texto no se incluyó en esta compilación. ")
                .append("Consulta el archivo correspondiente del código fuente.\n")
                .append("Error: ").append(e.getMessage());
            Log.e(TAG, "Could not read bundled license " + assetPath, e);
        }
    }

    private void setupHardwareAndDriverInfo() {
        String hw = Build.HARDWARE != null ? Build.HARDWARE : "unknown";
        String board = Build.BOARD != null ? Build.BOARD : "";
        txtGpuHardwareInfo.setText("Equipo: " + hw + (!board.isEmpty() ? " (" + board + ")" : ""));
        refreshDriverStatus();
    }

    private void refreshDriverStatus() {
        File driverDir = new File(getFilesDir(), "custom_driver");
        File activeMarker = new File(driverDir, "active_driver.txt");

        boolean customEnabled = mPrefs.getBoolean("custom_driver_enabled", false);
        if (customEnabled && activeMarker.exists()) {
            String driverName = mPrefs.getString("custom_driver_name", "Controlador personalizado");
            String driverPath = mPrefs.getString("custom_driver_path", "");

            txtDriverStatusBadge.setText("PERSONAL");
            txtDriverStatusBadge.setBackgroundResource(R.drawable.badge_bg_green);
            txtDriverStatusBadge.setTextColor(getColor(R.color.success_green));

            String desc = mPrefs.getString("custom_driver_desc", "");
            String display = "Activo: " + driverName;
            if (!desc.isEmpty()) display += "\n" + desc;
            txtDriverDetail.setText(display);
        } else {
            txtDriverStatusBadge.setText("SISTEMA");
            txtDriverStatusBadge.setBackgroundResource(R.drawable.badge_bg_blue);
            txtDriverStatusBadge.setTextColor(getColor(R.color.primary_accent));
            txtDriverDetail.setText(getString(R.string.driver_system_default));
        }
    }

    private void openDriverPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQ_PICK_DRIVER);
    }

    private void resetToSystemDriver() {
        File driverDir = new File(getFilesDir(), "custom_driver");
        File activeMarker = new File(driverDir, "active_driver.txt");
        if (activeMarker.exists()) activeMarker.delete();

        File sdcardMarker = new File("/sdcard/GalaxyData/custom_driver/active_driver.txt");
        if (sdcardMarker.exists()) sdcardMarker.delete();

        mPrefs.edit().putBoolean("custom_driver_enabled", false).apply();
        refreshDriverStatus();
        appendLog("[Driver] Reset graphics driver to System Default.");
        Toast.makeText(this, "Se restauró el controlador del sistema.", Toast.LENGTH_SHORT).show();
    }

    private void installCustomDriverPackage(Uri uri) {
        appendLog("[Driver] Reading custom graphics driver package...");
        txtProgressStatus.setText("Extrayendo el paquete del controlador…");

        new Thread(() -> {
            try {
                File driverDir = new File(getFilesDir(), "custom_driver");
                deleteRecursive(driverDir);
                driverDir.mkdirs();

                String driverName = "Custom Vulkan Driver";
                String driverDesc = "";
                String driverVersion = "";
                String targetLibraryName = "";

                // Extract Zip contents
                try (InputStream is = getContentResolver().openInputStream(uri);
                     BufferedInputStream bis = new BufferedInputStream(is);
                     ZipInputStream zis = new ZipInputStream(bis)) {

                    ZipEntry entry;
                    byte[] buffer = new byte[64 * 1024];
                    while ((entry = zis.getNextEntry()) != null) {
                        String name = entry.getName();
                        if (name.contains("..")) continue;

                        File outFile = new File(driverDir, name);
                        if (entry.isDirectory()) {
                            outFile.mkdirs();
                        } else {
                            File parent = outFile.getParentFile();
                            if (parent != null && !parent.exists()) parent.mkdirs();

                            try (FileOutputStream fos = new FileOutputStream(outFile)) {
                                int count;
                                while ((count = zis.read(buffer)) != -1) {
                                    fos.write(buffer, 0, count);
                                }
                            }

                            // Inspect metadata JSON if found
                            if (name.equalsIgnoreCase("meta.json") || name.equalsIgnoreCase("driver.json") || name.endsWith("/meta.json")) {
                                try (FileInputStream metaFis = new FileInputStream(outFile);
                                     InputStreamReader reader = new InputStreamReader(metaFis, StandardCharsets.UTF_8)) {
                                    StringBuilder sb = new StringBuilder();
                                    char[] cbuf = new char[1024];
                                    int read;
                                    while ((read = reader.read(cbuf)) != -1) {
                                        sb.append(cbuf, 0, read);
                                    }
                                    JSONObject json = new JSONObject(sb.toString());
                                    if (json.has("name")) driverName = json.getString("name");
                                    if (json.has("description")) driverDesc = json.getString("description");
                                    if (json.has("packageVersion")) driverVersion = json.getString("packageVersion");
                                    if (json.has("libraryName")) targetLibraryName = json.getString("libraryName");
                                } catch (Exception ignored) {}
                            }
                        }
                        zis.closeEntry();
                    }
                }

                // Locate the driver .so library
                File primarySo = null;
                if (!targetLibraryName.isEmpty()) {
                    File candidate = new File(driverDir, targetLibraryName);
                    if (candidate.exists()) primarySo = candidate;
                }

                if (primarySo == null) {
                    primarySo = findFirstSharedLib(driverDir);
                }

                if (primarySo == null) {
                    mHandler.post(() -> {
                        appendLog("[Driver] Error: No .so shared library found in the driver package.");
                        Toast.makeText(this, "El ZIP no contiene una biblioteca .so válida.", Toast.LENGTH_LONG).show();
                        txtProgressStatus.setText("No se pudo instalar el controlador.");
                    });
                    return;
                }

                // Set file permissions (readable, executable, and read-only for Android 14)
                setDriverPermissionsRecursive(driverDir);

                // Write active_driver.txt
                File activeMarker = new File(driverDir, "active_driver.txt");
                try (FileOutputStream fos = new FileOutputStream(activeMarker)) {
                    String markerContent = primarySo.getAbsolutePath() + "\n" + driverName + "\n1\n";
                    fos.write(markerContent.getBytes(StandardCharsets.UTF_8));
                }

                // Mirror to /sdcard/GalaxyData/custom_driver/
                try {
                    File sdcardDriverDir = new File("/sdcard/GalaxyData/custom_driver");
                    sdcardDriverDir.mkdirs();
                    File sdcardMarker = new File(sdcardDriverDir, "active_driver.txt");
                    try (FileOutputStream fos = new FileOutputStream(sdcardMarker)) {
                        String markerContent = primarySo.getAbsolutePath() + "\n" + driverName + "\n1\n";
                        fos.write(markerContent.getBytes(StandardCharsets.UTF_8));
                    }
                } catch (Exception ignored) {}

                final String finalName = driverName + (!driverVersion.isEmpty() ? " (v" + driverVersion + ")" : "");
                final String finalDesc = driverDesc;
                final String finalPath = primarySo.getAbsolutePath();

                mPrefs.edit()
                    .putBoolean("custom_driver_enabled", true)
                    .putString("custom_driver_name", finalName)
                    .putString("custom_driver_desc", finalDesc)
                    .putString("custom_driver_path", finalPath)
                    .apply();

                mHandler.post(() -> {
                    refreshDriverStatus();
                    appendLog("[Driver] Successfully installed: " + finalName);
                    appendLog("[Driver] Driver binary: " + finalPath);
                    txtProgressStatus.setText("Controlador personalizado instalado.");
                    Toast.makeText(this, "Instalado: " + finalName, Toast.LENGTH_SHORT).show();
                });

            } catch (Exception e) {
                Log.e(TAG, "Driver installation failed", e);
                mHandler.post(() -> {
                    appendLog("[Driver] Failed to install driver: " + e.getMessage());
                    txtProgressStatus.setText("Error al instalar el controlador.");
                    Toast.makeText(this, "No se pudo instalar: " + e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    private File findFirstSharedLib(File dir) {
        if (dir == null || !dir.exists()) return null;
        File[] files = dir.listFiles();
        if (files == null) return null;

        // First pass: look for files named vulkan.*.so
        for (File f : files) {
            if (f.isFile() && f.getName().toLowerCase().contains("vulkan") && f.getName().endsWith(".so")) {
                return f;
            }
        }
        // Second pass: any .so in current directory
        for (File f : files) {
            if (f.isFile() && f.getName().endsWith(".so")) {
                return f;
            }
        }
        // Recurse subdirectories
        for (File f : files) {
            if (f.isDirectory()) {
                File sub = findFirstSharedLib(f);
                if (sub != null) return sub;
            }
        }
        return null;
    }

    private void setDriverPermissionsRecursive(File fileOrDir) {
        if (fileOrDir == null || !fileOrDir.exists()) return;
        fileOrDir.setReadable(true, false);
        fileOrDir.setExecutable(true, false);
        if (fileOrDir.isDirectory()) {
            File[] children = fileOrDir.listFiles();
            if (children != null) {
                for (File child : children) {
                    setDriverPermissionsRecursive(child);
                }
            }
        } else if (fileOrDir.getName().endsWith(".so")) {
            if (Build.VERSION.SDK_INT >= 29) {
                fileOrDir.setReadOnly();
            }
        }
    }

    private void deleteRecursive(File fileOrDirectory) {
        if (fileOrDirectory == null || !fileOrDirectory.exists()) return;
        if (fileOrDirectory.isDirectory()) {
            File[] children = fileOrDirectory.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        fileOrDirectory.delete();
    }

    private void launchGameDirectly() {
        recordLaunchDiagnostic(this, "Launch Game pressed");
        if (!isGameDataReady()) {
            recordLaunchDiagnostic(this, "Launch blocked: required game data is missing or invalid");
            appendLog("[Launch] Faltan archivos válidos del juego; se bloqueó el arranque.");
            Toast.makeText(this, "Faltan archivos del juego. Copia el informe de inicio.", Toast.LENGTH_LONG).show();
            return;
        }

        File nativeDir = new File(getApplicationInfo().nativeLibraryDir);
        for (String library : new String[] {"libmain.so", "libgame.so", "libreserve.so"}) {
            File file = new File(nativeDir, library);
            if (!file.isFile() || !file.canRead()) {
                recordLaunchDiagnostic(this, "Launch blocked: missing native library " + file.getAbsolutePath());
                appendLog("[Launch] Missing native library: " + library);
                Toast.makeText(this, "Falta la biblioteca nativa: " + library, Toast.LENGTH_LONG).show();
                return;
            }
        }

        try {
            recordLaunchDiagnostic(this, "Starting GameActivity");
            startActivity(new Intent(this, GameActivity.class));
            // No cross-fade: the default transition shows this launcher screen
            // blended over the game's first frames ("images over images").
            overridePendingTransition(0, 0);
            recordLaunchDiagnostic(this, "GameActivity startActivity returned");
        } catch (RuntimeException | LinkageError error) {
            recordLaunchDiagnostic(this, "GameActivity start failed", error);
            appendLog("[Launch ERROR] " + error);
            Toast.makeText(this, "No se pudo abrir el juego. Copia el informe de inicio.", Toast.LENGTH_LONG).show();
        }
    }

    private void checkStoragePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                appendLog("[Permission] Requesting storage access for game data...");
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                try {
                    startActivity(intent);
                } catch (Exception e) {
                    Intent generic = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                    startActivity(generic);
                }
            }
        } else {
            if (checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                appendLog("[Permission] Requesting legacy storage permission for game data...");
                requestPermissions(new String[]{
                    android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, 2001);
            }
        }
    }

    private void autoDetectSource() {
        appendLog("[Scan] Searching for game dumps and extracted folders...");

        // 1. Check if /sdcard/GalaxyData already exists
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        if (galaxyDataDir.exists()) {
            if (isGameDataReady()) {
                appendLog("[Scan] Game data is ready in " + galaxyDataDir.getAbsolutePath());
                mHandler.post(this::refreshGameStatus);
            }
            File[] gFiles = galaxyDataDir.listFiles();
            if (gFiles != null) {
                for (File f : gFiles) {
                    String name = f.getName().toLowerCase();
                    if (name.endsWith(".rvz") || name.endsWith(".wbfs") || name.endsWith(".iso")) {
                        selectSource(f.getAbsolutePath(), "Detected disc in GalaxyData: " + f.getName());
                        return;
                    }
                }
            }
        }

        // 2. Scan Downloads folder
        File downloadDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (downloadDir != null && downloadDir.exists()) {
            File[] files = downloadDir.listFiles();
            if (files != null) {
                for (File f : files) {
                    String name = f.getName().toLowerCase();
                    if (name.endsWith(".rvz") || name.endsWith(".wbfs") || name.endsWith(".iso")) {
                        selectSource(f.getAbsolutePath(), "Detected disc in Downloads: " + f.getName());
                        return;
                    }
                }
                for (File f : files) {
                    if (f.isDirectory()) {
                        File testFiles = new File(f, "files");
                        File testData = new File(f, "DATA");
                        if (testFiles.exists() || testData.exists()) {
                            selectSource(f.getAbsolutePath(), "Detected extracted folder: " + f.getName());
                            return;
                        }
                    }
                }
            }
        }

        appendLog("[Scan] No disc image auto-detected. Use 'Select Disc File' button.");
    }

    private void selectSource(String path, String description) {
        mSelectedSourcePath = path;
        appendLog("[Selected] " + description);
        mHandler.post(() -> {
            txtSourcePath.setText(path);
        txtProgressStatus.setText("Archivo seleccionado. Listo para instalar.");
            btnStartCooking.setEnabled(true);
        });
    }

    private void openFilePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQ_PICK_DISC);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;

        Uri uri = data.getData();
        if (requestCode == REQ_PICK_DISC) {
            mSelectedSourceUri = uri;
            mSelectedDisplayName = getDisplayName(uri);
            String path = getPathFromUri(uri);
            if (path != null) {
                mSelectedSourcePath = path;
                selectSource(path, "Selected disc: " + (mSelectedDisplayName.isEmpty() ? new File(path).getName() : mSelectedDisplayName));
            } else {
                mSelectedSourcePath = uri.toString();
                selectSource(mSelectedDisplayName.isEmpty() ? uri.toString() : mSelectedDisplayName,
                        "Selected disc: " + (mSelectedDisplayName.isEmpty() ? uri.toString() : mSelectedDisplayName));
            }
        } else if (requestCode == REQ_PICK_DRIVER) {
            installCustomDriverPackage(uri);
        }
    }

    private String getDisplayName(Uri uri) {
        String displayName = null;
        if ("content".equalsIgnoreCase(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (nameIndex >= 0) {
                        displayName = cursor.getString(nameIndex);
                    }
                }
            } catch (Exception ignored) {}
        }
        if (displayName == null || displayName.isEmpty()) {
            displayName = uri.getLastPathSegment();
            if (displayName != null && displayName.contains("/")) {
                displayName = displayName.substring(displayName.lastIndexOf('/') + 1);
            }
        }
        return displayName != null ? displayName : "";
    }

    private String getPathFromUri(Uri uri) {
        if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            try {
                if (DocumentsContract.isDocumentUri(this, uri)) {
                    String docId = DocumentsContract.getDocumentId(uri);
                    if (docId != null) {
                        if (docId.startsWith("raw:")) {
                            return docId.substring(4);
                        }
                        if (docId.startsWith("primary:")) {
                            File f = new File(Environment.getExternalStorageDirectory(), docId.substring(8));
                            if (f.exists()) return f.getAbsolutePath();
                        }
                    }
                }
            } catch (Exception ignored) {}
        }
        return null;
    }

    private void startCookingProcess() {
        if (mIsCooking) return;
        mIsCooking = true;

        btnStartCooking.setEnabled(false);
        btnSelectDisc.setEnabled(false);
        btnAutoDetect.setEnabled(false);
        progressBar.setVisibility(View.VISIBLE);
        progressBar.setIndeterminate(true);
        txtProgressStatus.setText("Preparando la instalación…");

        final boolean includeMovies = chkIncludeMovies.isChecked();

        new Thread(() -> {
            try {
                File outputDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
                if (!outputDir.exists()) outputDir.mkdirs();

                String testName = !mSelectedDisplayName.isEmpty() ? mSelectedDisplayName : mSelectedSourcePath;
                String lowerName = testName.toLowerCase();
                boolean isDiscImage = lowerName.endsWith(".rvz") || lowerName.endsWith(".wbfs") || lowerName.endsWith(".iso") || lowerName.endsWith(".gcm");

                File resolvedDiscFile = null;
                if (isDiscImage) {
                    if (new File(mSelectedSourcePath).exists()) {
                        resolvedDiscFile = new File(mSelectedSourcePath);
                    } else if (mSelectedSourceUri != null) {
                        appendLog("[Setup] Reading disc image: " + mSelectedDisplayName);
                        mHandler.post(() -> txtProgressStatus.setText("Preparando el archivo para extraerlo…"));
                        String ext = lowerName.endsWith(".rvz") ? "rvz" : lowerName.endsWith(".wbfs") ? "wbfs" : "iso";
                        File tempDisc = new File(getFilesDir(), "temp_disc." + ext);
                        if (tempDisc.exists()) tempDisc.delete();
                        try (java.io.InputStream in = getContentResolver().openInputStream(mSelectedSourceUri);
                             java.io.FileOutputStream out = new java.io.FileOutputStream(tempDisc)) {
                            byte[] buf = new byte[1024 * 1024];
                            int read;
                            long total = 0;
                            while ((read = in.read(buf)) > 0) {
                                out.write(buf, 0, read);
                                total += read;
                                if (total % (50 * 1024 * 1024) == 0) {
                                    final long mb = total / (1024 * 1024);
                                    mHandler.post(() -> txtProgressStatus.setText("Leídos " + mb + " MB…"));
                                }
                            }
                        }
                        resolvedDiscFile = tempDisc;
                    }
                }

                File extractionDir = outputDir;
                File tempExtractionDir = null;

                // Step 1: If source is disc image, extract with native nodtool
                if (isDiscImage && resolvedDiscFile != null && resolvedDiscFile.exists()) {
                    tempExtractionDir = new File(getFilesDir(), "temp_extracted");
                    CookerEngine.deleteRecursive(tempExtractionDir);
                    tempExtractionDir.mkdirs();

                    final String discPathToExtract = resolvedDiscFile.getAbsolutePath();
                    mHandler.post(() -> txtProgressStatus.setText("Extrayendo la partición del archivo…"));
                    boolean extractSuccess = CookerEngine.extractWithNodtool(
                        this, discPathToExtract, tempExtractionDir.getAbsolutePath(), includeMovies,
                        new CookerEngine.CookerCallback() {
                            @Override public void onLog(String line) { appendLog(line); }
                            @Override public void onProgress(int percent, String message) {
                                mHandler.post(() -> {
                                    if (message != null) txtProgressStatus.setText(message);
                                });
                            }
                            @Override public void onFinished(boolean success, String message) {}
                        }
                    );

                    if (resolvedDiscFile.getAbsolutePath().startsWith(getFilesDir().getAbsolutePath())) {
                        resolvedDiscFile.delete();
                    }

                    if (!extractSuccess) {
                        mHandler.post(() -> onCookingCompleted(false, "Native disc extraction failed."));
                        return;
                    }
                    extractionDir = tempExtractionDir;
                }

                // Step 2: Prepare converter environment and python runtime
                mHandler.post(() -> txtProgressStatus.setText("Preparando el conversor…"));
                CookerEngine.prepareAssets(this, new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int percent, String message) {}
                    @Override public void onFinished(boolean success, String message) {}
                });

                File filesDir = getFilesDir();
                File pythonHome = new File(filesDir, "pythonhome");
                File scriptsDir = new File(filesDir, "scripts");

                // Step 3: Run asset converter into GalaxyData
                mHandler.post(() -> txtProgressStatus.setText("Convirtiendo recursos para ARM64…"));
                int ret = CookerEngine.runCooker(
                    pythonHome.getAbsolutePath(),
                    scriptsDir.getAbsolutePath(),
                    extractionDir.getAbsolutePath(),
                    outputDir.getAbsolutePath(),
                    includeMovies,
                    new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (percent >= 0) {
                                    progressBar.setIndeterminate(false);
                                    progressBar.setProgress(percent);
                                }
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {
                            mHandler.post(() -> onCookingCompleted(success, message));
                        }
                    }
                );

                if (ret != 0) {
                    mHandler.post(() -> onCookingCompleted(false, "Converter returned code: " + ret));
                } else {
                    if (tempExtractionDir != null && tempExtractionDir.exists()) {
                        appendLog("[Clean] Freeing temporary extraction cache...");
                        CookerEngine.deleteRecursive(tempExtractionDir);
                    }
                    mHandler.post(() -> onCookingCompleted(true, "Installation complete!"));
                }

            } catch (Exception e) {
                Log.e(TAG, "Cooking failed with exception", e);
                appendLog("[ERROR] " + e.getMessage());
                mHandler.post(() -> onCookingCompleted(false, e.getMessage()));
            }
        }).start();
    }

    private void onCookingCompleted(boolean success, String message) {
        mIsCooking = false;
        progressBar.setVisibility(View.GONE);
        btnStartCooking.setEnabled(true);
        btnSelectDisc.setEnabled(true);
        btnAutoDetect.setEnabled(true);

        if (success) {
            try {
                new File(Environment.getExternalStorageDirectory(), "GalaxyData/.installed_ok").createNewFile();
            } catch (Exception ignored) {}
            txtProgressStatus.setText(R.string.status_done);
            appendLog("[SUCCESS] " + (message != null ? message : "Conversion complete!"));
            refreshGameStatus();
            Toast.makeText(this, "Juego preparado. Iniciando…", Toast.LENGTH_SHORT).show();
            mHandler.postDelayed(this::launchGameDirectly, 1000);
        } else {
            try {
                new File(Environment.getExternalStorageDirectory(), "GalaxyData/.installed_ok").delete();
            } catch (Exception ignored) {}
            txtProgressStatus.setText(R.string.status_error);
            appendLog("[FAILURE] " + (message != null ? message : "Error during installation."));
            Toast.makeText(this, "Falló la instalación. Revisa el registro.", Toast.LENGTH_LONG).show();
        }
    }

    private void appendLog(String line) {
        mHandler.post(() -> {
            txtLogOutput.append(line + "\n");
            final int scrollAmount = txtLogOutput.getLayout() != null ?
                    txtLogOutput.getLayout().getLineTop(txtLogOutput.getLineCount()) - txtLogOutput.getHeight() : 0;
            if (scrollAmount > 0) {
                txtLogOutput.scrollTo(0, scrollAmount);
            }
        });
    }
}
