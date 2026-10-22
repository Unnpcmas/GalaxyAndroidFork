package com.openport.galaxyandroid;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.InputDevice;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.OpenableColumns;
import android.widget.Toast;

import java.util.ArrayDeque;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

public class GameActivity extends NativeActivity {
    private static final String TAG = "GalaxyGameActivity";
    private static final int REQ_PICK_MOD_ZIP = 3107;
    private static GameActivity sInstance;
    private AudioManager audioManager;
    private AudioFocusRequest audioFocusRequest;
    private String pendingModsDirectory = "";
    private static final ArrayDeque<String> androidDiagnostics = new ArrayDeque<>();
    private static final int MAX_ANDROID_DIAGNOSTICS = 128;
    private static final Object diagnosticFileLock = new Object();
    private static final long MAX_DIAGNOSTIC_FILE_BYTES = 2L * 1024L * 1024L;
    private static boolean crashHandlerInstalled;
    private static Thread.UncaughtExceptionHandler previousUncaughtExceptionHandler;

    private static boolean appendDiagnosticFile(File file, byte[] bytes) {
        if (file == null) return false;
        try {
            File parent = file.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs() && !parent.exists()) {
                return false;
            }
            if (file.length() + bytes.length > MAX_DIAGNOSTIC_FILE_BYTES) {
                File previous = new File(file.getParentFile(), file.getName() + ".prev");
                previous.delete();
                if (!file.renameTo(previous)) file.delete();
            }
            try (FileOutputStream stream = new FileOutputStream(file, true)) {
                stream.write(bytes);
                stream.flush();
                return true;
            }
        } catch (IOException | SecurityException ignored) {
            return false;
        }
    }

    private static void persistDiagnostic(String entry) {
        GameActivity activity = sInstance;
        if (activity == null) return;
        String record = System.currentTimeMillis() + " " + entry + "\n";
        byte[] bytes = record.getBytes(StandardCharsets.UTF_8);
        synchronized (diagnosticFileLock) {
            appendDiagnosticFile(new File(activity.getFilesDir(), "android_diagnostics.log"), bytes);
        }
    }

    private static synchronized void installCrashHandler() {
        if (crashHandlerInstalled) return;
        previousUncaughtExceptionHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            String stack = Log.getStackTraceString(error);
            recordDiagnostic("CRASH", "UNCAUGHT Java exception thread=" +
                thread.getName() + "\n" + stack, null);
            Thread.UncaughtExceptionHandler previous = previousUncaughtExceptionHandler;
            if (previous != null) previous.uncaughtException(thread, error);
        });
        crashHandlerInstalled = true;
    }

    private static void recordDiagnostic(String level, String message, Throwable error) {
        StringBuilder entry = new StringBuilder(level).append(" GameActivity: ").append(message);
        if (error != null) {
            String stack = Log.getStackTraceString(error);
            if (stack != null && !stack.isEmpty()) entry.append('\n').append(stack);
        }
        synchronized (androidDiagnostics) {
            if (androidDiagnostics.size() >= MAX_ANDROID_DIAGNOSTICS) {
                androidDiagnostics.removeFirst();
            }
            androidDiagnostics.addLast(entry.toString());
        }
        persistDiagnostic(entry.toString());
    }

    private final AudioManager.OnAudioFocusChangeListener audioFocusChangeListener = change -> {
        if (change == AudioManager.AUDIOFOCUS_LOSS ||
            change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT ||
            change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) {
            String message = "Game audio focus lost: " + change;
            Log.w(TAG, message);
            recordDiagnostic("WARN", message, null);
        } else if (change == AudioManager.AUDIOFOCUS_GAIN) {
            recordDiagnostic("INFO", "Game audio focus gained", null);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        installCrashHandler();
        MainActivity.recordLaunchDiagnostic(this, "GameActivity.onCreate before NativeActivity.super");
        try {
            super.onCreate(savedInstanceState);
        } catch (RuntimeException | LinkageError error) {
            MainActivity.recordLaunchDiagnostic(this, "NativeActivity.super.onCreate failed", error);
            throw error;
        }
        MainActivity.recordLaunchDiagnostic(this, "NativeActivity.super.onCreate returned");
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        applyFullscreen();
        setPreferredRefreshRate(144.0f);

        if (!MainActivity.isGameDataReady()) {
            returnToLauncher();
            return;
        }
    }

    // Queried from the native render/input thread at a low rate so the touch
    // HUD follows controller attach/detach without polling Android every frame.
    public boolean hasConnectedGamepad() {
        for (int deviceId : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(deviceId);
            if (device == null) continue;
            int sources = device.getSources();
            boolean gamepad = (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
            boolean joystick = (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
            if (gamepad || joystick) return true;
        }
        return false;
    }

    public static void requestModsZipImport(String modsDirectory) {
        GameActivity activity = sInstance;
        if (activity == null || modsDirectory == null || modsDirectory.isEmpty()) return;
        activity.runOnUiThread(() -> activity.openModsZipPicker(modsDirectory));
    }

    private void openModsZipPicker(String modsDirectory) {
        if (pendingModsDirectory != null && !pendingModsDirectory.isEmpty()) {
            Toast.makeText(this, "Ya hay una selección de mod abierta.", Toast.LENGTH_SHORT).show();
            return;
        }
        pendingModsDirectory = modsDirectory;
        try {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                    "application/zip", "application/x-zip-compressed", "application/octet-stream"
            });
            startActivityForResult(intent, REQ_PICK_MOD_ZIP);
        } catch (RuntimeException error) {
            pendingModsDirectory = "";
            recordDiagnostic("ERROR", "Could not open the local mod ZIP picker", error);
            Toast.makeText(this, "No se pudo abrir el selector de archivos.", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_MOD_ZIP) return;

        String modsDirectory = pendingModsDirectory;
        pendingModsDirectory = "";
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            Toast.makeText(this, "Importación de mod cancelada.", Toast.LENGTH_SHORT).show();
            return;
        }
        Uri uri = data.getData();
        new Thread(() -> importModsZip(uri, modsDirectory), "GalaxyModZipImport").start();
    }

    private void importModsZip(Uri uri, String modsDirectory) {
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("No se pudo leer el archivo seleccionado.");
            String archiveName = getDisplayName(uri);
            ModZipImporter.Result result = ModZipImporter.install(
                    input, new File(modsDirectory), archiveName);
            recordDiagnostic("INFO", "Imported local resource mod id=" + result.id +
                    " resources=" + result.resourceFiles +
                    " pathCheck=" + (result.compatibilityChecked ? "checked" : "unavailable") +
                    " matchingBaseFiles=" + result.matchingBaseFiles +
                    " missingBaseFiles=" + result.missingBaseFiles, null);
            String pathSummary;
            if (!result.compatibilityChecked) {
                pathSummary = "No se pudieron comparar las rutas con la carpeta files/ del juego.";
            } else if (result.missingBaseFiles == 0) {
                pathSummary = "Las " + result.matchingBaseFiles +
                        " rutas existen en la carpeta files/ del juego.";
            } else {
                pathSummary = result.matchingBaseFiles + " de " + result.resourceFiles +
                        " rutas existen en files/; " + result.missingBaseFiles +
                        " no aparecen ahí.";
            }
            String message = "Mod importado: " + result.name + ". " + pathSummary +
                    " Esto no valida el índice del juego ni el formato del recurso. Pulsa Escanear para activarlo.";
            runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_LONG).show());
        } catch (Exception error) {
            Log.e(TAG, "Local mod ZIP import failed", error);
            recordDiagnostic("ERROR", "Local mod ZIP import failed", error);
            String detail = error.getMessage();
            if (detail == null || detail.isEmpty()) detail = "ZIP inválido o no compatible.";
            final String message = detail;
            runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_LONG).show());
        }
    }

    private String getDisplayName(Uri uri) {
        if (uri == null) return "";
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String value = cursor.getString(column);
                    if (value != null && !value.isEmpty()) return value;
                }
            }
        } catch (RuntimeException error) {
            recordDiagnostic("WARN", "Could not read the selected ZIP display name", error);
        }
        String segment = uri.getLastPathSegment();
        return segment == null ? "mod-local.zip" : segment;
    }

    @Override
    protected void onResume() {
        super.onResume();
        requestGameAudioFocus();
        applyFullscreen();
    }

    @Override
    protected void onPause() {
        releaseGameAudioFocus();
        super.onPause();
    }

    public static void setPreferredRefreshRate(float maxFps) {
        if (sInstance != null) {
            sInstance.runOnUiThread(() -> {
                try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                        WindowManager.LayoutParams lp = sInstance.getWindow().getAttributes();
                        android.view.Display display = null;
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                            display = sInstance.getDisplay();
                        } else {
                            WindowManager wm = (WindowManager) sInstance.getSystemService(Context.WINDOW_SERVICE);
                            if (wm != null) display = wm.getDefaultDisplay();
                        }
                        if (display != null) {
                            android.view.Display.Mode[] modes = display.getSupportedModes();
                            android.view.Display.Mode currentMode = display.getMode();
                            android.view.Display.Mode bestMode = null;
                            for (android.view.Display.Mode m : modes) {
                                if (currentMode != null &&
                                    (m.getPhysicalWidth() != currentMode.getPhysicalWidth() ||
                                     m.getPhysicalHeight() != currentMode.getPhysicalHeight())) {
                                    continue;
                                }
                                if (m.getRefreshRate() <= maxFps + 1.0f) {
                                    if (bestMode == null || m.getRefreshRate() > bestMode.getRefreshRate()) {
                                        bestMode = m;
                                    }
                                }
                            }
                            if (bestMode != null) {
                                lp.preferredDisplayModeId = bestMode.getModeId();
                            }
                        }
                        lp.preferredRefreshRate = (maxFps < 60.0f) ? 60.0f : maxFps;
                        sInstance.getWindow().setAttributes(lp);
                    }
                } catch (Throwable ignored) {}
            });
        }
    }

    public static void copyDiagnosticsToClipboard(byte[] utf8Bytes) {
        GameActivity activity = sInstance;
        if (activity == null) return;
        StringBuilder complete = new StringBuilder(
            utf8Bytes == null ? "" : new String(utf8Bytes, StandardCharsets.UTF_8));
        synchronized (androidDiagnostics) {
            if (!androidDiagnostics.isEmpty()) {
                complete.append("\n--- Android activity events ---\n");
                for (String entry : androidDiagnostics) {
                    complete.append(entry).append('\n');
                }
            }
        }
        final String text = complete.toString();
        activity.runOnUiThread(() -> {
            try {
                ClipboardManager clipboard =
                    (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                if (clipboard == null) {
                    recordDiagnostic("ERROR", "Android clipboard service unavailable", null);
                    return;
                }
                clipboard.setPrimaryClip(ClipData.newPlainText("Galaxy Android diagnostics", text));
                Toast.makeText(activity, "Registro completo copiado", Toast.LENGTH_SHORT).show();
            } catch (RuntimeException error) {
                Log.e(TAG, "Could not copy diagnostics to clipboard", error);
                recordDiagnostic("ERROR", "Could not copy diagnostics to clipboard", error);
            }
        });
    }

    @Override
    protected void onDestroy() {
        releaseGameAudioFocus();
        if (sInstance == this) sInstance = null;
        super.onDestroy();
    }

    private void requestGameAudioFocus() {
        if (audioManager == null) {
            audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        }
        if (audioManager == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }

        try {
            AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();
            audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attributes)
                .setAcceptsDelayedFocusGain(false)
                .setWillPauseWhenDucked(false)
                .setOnAudioFocusChangeListener(audioFocusChangeListener)
                .build();
            int result = audioManager.requestAudioFocus(audioFocusRequest);
            if (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
                String message = "Android did not grant game audio focus: " + result;
                Log.e(TAG, message);
                recordDiagnostic("ERROR", message, null);
            } else {
                StringBuilder outputs = new StringBuilder();
                for (AudioDeviceInfo device : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                    if (outputs.length() > 0) outputs.append(", ");
                    outputs.append(device.getId()).append(':').append(device.getType())
                        .append(':').append(device.getProductName());
                }
                int musicVolume = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
                int maxMusicVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
                recordDiagnostic("INFO", "Game audio focus granted; music volume=" +
                    musicVolume + "/" + maxMusicVolume + "; mode=" + audioManager.getMode() +
                    "; output devices=[" + outputs + "]", null);
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "Could not request game audio focus", e);
            recordDiagnostic("ERROR", "Could not request game audio focus", e);
        }
    }

    private void releaseGameAudioFocus() {
        if (audioManager != null && audioFocusRequest != null &&
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                audioManager.abandonAudioFocusRequest(audioFocusRequest);
            } catch (RuntimeException e) {
                Log.w(TAG, "Could not release game audio focus", e);
                recordDiagnostic("WARN", "Could not release game audio focus", e);
            } finally {
                audioFocusRequest = null;
            }
        }
    }

    public static void triggerHaptic(int ms) {
        if (sInstance != null) {
            sInstance.runOnUiThread(() -> {
                try {
                    Vibrator v = (Vibrator) sInstance.getSystemService(Context.VIBRATOR_SERVICE);
                    if (v != null && v.hasVibrator()) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                            v.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
                        } else {
                            v.vibrate(ms);
                        }
                    }
                } catch (Throwable error) {
                    Log.w(TAG, "Could not trigger haptic feedback", error);
                    recordDiagnostic("WARN", "Could not trigger haptic feedback", error);
                }
            });
        }
    }

    public static void returnToLauncher() {
        if (sInstance != null) {
            sInstance.runOnUiThread(() -> {
                try {
                    Intent intent = new Intent(sInstance, MainActivity.class);
                    intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_NEW_TASK);
                    sInstance.startActivity(intent);
                    sInstance.finish();
                } catch (Exception ignored) {}
            });
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyFullscreen();
        }
    }

    private void applyFullscreen() {
        Window window = getWindow();
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                WindowManager.LayoutParams params = window.getAttributes();
                // Extend edge-to-edge across the short edge; only the physical
                // camera hole can obscure pixels. System bars remain immersive.
                params.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                window.setAttributes(params);
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.setDecorFitsSystemWindows(false);
                WindowInsetsController controller = window.getInsetsController();
                if (controller != null) {
                    controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                    controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                window.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                );
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Falling back to legacy fullscreen flags", e);
            recordDiagnostic("WARN", "Falling back to legacy fullscreen flags", e);
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            );
        }
    }
}
