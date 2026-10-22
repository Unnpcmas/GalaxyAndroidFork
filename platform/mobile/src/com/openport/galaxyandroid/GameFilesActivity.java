package com.openport.galaxyandroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;

/** Local-only file browser restricted to GalaxyData. */
public final class GameFilesActivity extends Activity {
    public static final String EXTRA_ROOT_PATH = "com.openport.galaxyandroid.GAME_FILES_ROOT";
    private static final String PREFS = "GameFilesBrowser";
    private static final String PREF_COPIED_FILE = "copied_file_path";
    private static final int REQ_SAVE_FILE = 4301;
    private static final int REQ_SAVE_FOLDER = 4302;
    private static final int MAX_SEARCH_VISITED = 50000;
    private static final int MAX_SEARCH_RESULTS = 2500;
    private static final int MAX_SEARCH_DEPTH = 32;
    private static final int MAX_EXPORT_FILES = 50000;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicInteger operationToken = new AtomicInteger();
    private final ArrayList<Entry> entries = new ArrayList<>();
    private File rootDirectory;
    private File currentDirectory;
    private EditText pathField;
    private EditText searchField;
    private TextView statusText;
    private ListView fileList;
    private Button upButton;
    private Button pasteFileButton;
    private Button saveCurrentFolderButton;
    private ProgressBar exportProgress;
    private FileListAdapter adapter;
    private File pendingExportSource;
    private boolean exportInProgress;

    private static final class Entry {
        final File file;
        final boolean directory;
        final String detail;

        Entry(File file, boolean directory, String detail) {
            this.file = file;
            this.directory = directory;
            this.detail = detail;
        }
    }

    private static final class SearchState {
        int visited;
        boolean truncated;
    }

    private static final class ExportState {
        int filesCopied;
        int entriesVisited;
        long bytesCopied;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        String suppliedRoot = getIntent().getStringExtra(EXTRA_ROOT_PATH);
        if (suppliedRoot == null || suppliedRoot.trim().isEmpty()) {
            suppliedRoot = new File(Environment.getExternalStorageDirectory(), "GalaxyData").getPath();
        }
        try {
            rootDirectory = new File(suppliedRoot).getCanonicalFile();
        } catch (IOException error) {
            rootDirectory = new File(Environment.getExternalStorageDirectory(), "GalaxyData").getAbsoluteFile();
        }
        currentDirectory = rootDirectory;
        buildInterface();
        refreshClipboardButton();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (rootDirectory != null && fileList != null) {
            loadDirectory(currentDirectory == null ? rootDirectory : currentDirectory);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_SAVE_FILE && requestCode != REQ_SAVE_FOLDER) return;

        File source = pendingExportSource;
        pendingExportSource = null;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            statusText.setText("Guardado cancelado.");
            return;
        }
        if (source == null || !isSafePath(source) ||
                (requestCode == REQ_SAVE_FILE ? !source.isFile() : !source.isDirectory())) {
            showError("El archivo o carpeta de origen ya no está disponible.");
            return;
        }

        Uri destination = data.getData();
        if (!isLocalStorageDestination(destination)) {
            showError("Elige Almacenamiento interno, Descargas o una tarjeta SD. No se guardan archivos en la nube.");
            return;
        }
        if (requestCode == REQ_SAVE_FOLDER &&
                !"com.android.externalstorage.documents".equals(destination.getAuthority())) {
            showError("Para guardar una carpeta, elige Almacenamiento interno o una tarjeta SD.");
            return;
        }
        keepUriPermission(destination, data.getFlags());
        if (requestCode == REQ_SAVE_FILE) exportSingleFile(source, destination);
        else exportFolder(source, destination);
    }

    private void buildInterface() {
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setPadding(dp(16), dp(12), dp(16), dp(12));
        screen.setBackgroundColor(0xFF080A10);

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("Archivos del juego", 20, 0xFFF8FAFC, true);
        header.addView(title, new LinearLayout.LayoutParams(0, dp(40), 1));
        Button close = button("Cerrar", false);
        close.setOnClickListener(view -> finish());
        header.addView(close, new LinearLayout.LayoutParams(dp(78), dp(40)));
        screen.addView(header);

        TextView description = text("Carpeta local GalaxyData · los cambios se quedan en este dispositivo",
                12, 0xFF9AA4B2, false);
        description.setPadding(0, 0, 0, dp(8));
        screen.addView(description);

        LinearLayout pathRow = new LinearLayout(this);
        pathRow.setGravity(Gravity.CENTER_VERTICAL);
        pathField = editText("Ruta dentro de GalaxyData", InputType.TYPE_CLASS_TEXT);
        pathField.setSingleLine(true);
        pathField.setImeOptions(EditorInfo.IME_ACTION_GO);
        pathRow.addView(pathField, new LinearLayout.LayoutParams(0, dp(44), 1));
        Button go = button("IR", true);
        go.setOnClickListener(view -> goToPath(pathField.getText().toString()));
        LinearLayout.LayoutParams goParams = new LinearLayout.LayoutParams(dp(56), dp(44));
        goParams.leftMargin = dp(6);
        pathRow.addView(go, goParams);
        pathField.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_GO || action == EditorInfo.IME_ACTION_DONE) {
                goToPath(pathField.getText().toString());
                return true;
            }
            return false;
        });
        screen.addView(pathRow);

        LinearLayout navigation = new LinearLayout(this);
        navigation.setGravity(Gravity.CENTER_VERTICAL);
        upButton = button("Subir", false);
        upButton.setOnClickListener(view -> navigateUp());
        navigation.addView(upButton, weightedButton());
        Button copyPath = button("Copiar ruta", false);
        copyPath.setOnClickListener(view -> copyPath(currentDirectory));
        navigation.addView(copyPath, weightedButton());
        Button pastePath = button("Pegar ruta", false);
        pastePath.setOnClickListener(view -> pastePathFromClipboard());
        navigation.addView(pastePath, weightedButton());
        screen.addView(navigation, marginsTop(-1, dp(38), 0, 0, 6));

        LinearLayout searchRow = new LinearLayout(this);
        searchRow.setGravity(Gravity.CENTER_VERTICAL);
        searchField = editText("Buscar en toda la carpeta del juego", InputType.TYPE_CLASS_TEXT);
        searchField.setSingleLine(true);
        searchField.setImeOptions(EditorInfo.IME_ACTION_SEARCH);
        searchRow.addView(searchField, new LinearLayout.LayoutParams(0, dp(44), 1));
        Button search = button("Buscar", true);
        search.setOnClickListener(view -> searchAll(searchField.getText().toString()));
        LinearLayout.LayoutParams searchParams = new LinearLayout.LayoutParams(dp(78), dp(44));
        searchParams.leftMargin = dp(6);
        searchRow.addView(search, searchParams);
        searchField.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_SEARCH || action == EditorInfo.IME_ACTION_DONE) {
                searchAll(searchField.getText().toString());
                return true;
            }
            return false;
        });
        screen.addView(searchRow, marginsTop(-1, dp(44), 0, 0, 6));

        pasteFileButton = button("Pegar archivo aquí (sin sobrescribir)", false);
        pasteFileButton.setOnClickListener(view -> confirmPasteFile());
        screen.addView(pasteFileButton, marginsTop(-1, dp(42), 0, 0, 6));

        saveCurrentFolderButton = button("Guardar carpeta actual en el teléfono…", false);
        saveCurrentFolderButton.setOnClickListener(view -> requestFolderExport(currentDirectory));
        screen.addView(saveCurrentFolderButton, marginsTop(-1, dp(42), 0, 0, 6));

        statusText = text("Cargando…", 11, 0xFF9AA4B2, false);
        statusText.setGravity(Gravity.CENTER_VERTICAL);
        screen.addView(statusText, new LinearLayout.LayoutParams(-1, dp(28)));

        exportProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        exportProgress.setIndeterminate(true);
        exportProgress.setVisibility(View.GONE);
        screen.addView(exportProgress, new LinearLayout.LayoutParams(-1, dp(4)));

        fileList = new ListView(this);
        fileList.setDivider(new android.graphics.drawable.ColorDrawable(0xFF252B36));
        fileList.setDividerHeight(dp(1));
        fileList.setCacheColorHint(0x00000000);
        fileList.setBackgroundColor(0xFF080A10);
        adapter = new FileListAdapter();
        fileList.setAdapter(adapter);
        fileList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < 0 || position >= entries.size()) return;
            Entry entry = entries.get(position);
            if (entry.directory) navigateTo(entry.file);
            else showEntryActions(entry.file, false);
        });
        fileList.setOnItemLongClickListener((parent, view, position, id) -> {
            if (position >= 0 && position < entries.size()) {
                showEntryActions(entries.get(position).file, entries.get(position).directory);
            }
            return true;
        });
        screen.addView(fileList, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(screen);
    }

    private LinearLayout.LayoutParams weightedButton() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(38), 1);
        params.leftMargin = dp(2);
        params.rightMargin = dp(2);
        return params;
    }

    private LinearLayout.LayoutParams marginsTop(int width, int height, int left, int right, int top) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                width == -1 ? -1 : dp(width), height == -1 ? -1 : height);
        params.leftMargin = dp(left);
        params.rightMargin = dp(right);
        params.topMargin = dp(top);
        return params;
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private Button button(String label, boolean primary) {
        Button view = new Button(this);
        view.setText(label);
        view.setTextSize(11);
        view.setTextColor(0xFFF8FAFC);
        view.setAllCaps(false);
        view.setMaxLines(1);
        view.setEllipsize(android.text.TextUtils.TruncateAt.END);
        view.setMinWidth(0);
        view.setMinimumWidth(0);
        view.setPadding(dp(5), 0, dp(5), 0);
        GradientDrawable background = new GradientDrawable();
        background.setColor(primary ? 0xFF087F68 : 0xFF171D28);
        background.setCornerRadius(dp(10));
        background.setStroke(dp(1), primary ? 0xFF23B995 : 0xFF303847);
        view.setBackground(background);
        return view;
    }

    private EditText editText(String hint, int inputType) {
        EditText view = new EditText(this);
        view.setSingleLine(true);
        view.setTextSize(13);
        view.setTextColor(0xFFF8FAFC);
        view.setHintTextColor(0xFF7D8795);
        view.setHint(hint);
        view.setInputType(inputType);
        view.setPadding(dp(11), 0, dp(11), 0);
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xFF11151D);
        background.setCornerRadius(dp(10));
        background.setStroke(dp(1), 0xFF303847);
        view.setBackground(background);
        return view;
    }

    private void loadDirectory(File requested) {
        final int token = operationToken.incrementAndGet();
        final File target;
        try {
            target = requested.getCanonicalFile();
        } catch (IOException error) {
            showError("No se pudo resolver esa ruta.");
            return;
        }
        statusText.setText("Abriendo carpeta…");
        new Thread(() -> {
            ArrayList<Entry> result = new ArrayList<>();
            String errorMessage = null;
            if (!isSafePath(target) || !target.isDirectory()) {
                errorMessage = target.exists()
                        ? "Ruta fuera de GalaxyData o carpeta no válida."
                        : "La carpeta GalaxyData todavía no existe.";
            } else {
                File[] children = target.listFiles();
                if (children == null) {
                    errorMessage = "No se pudo leer la carpeta. Revisa el permiso de archivos.";
                } else {
                    for (File child : children) {
                        if (Files.isSymbolicLink(child.toPath())) continue;
                        if (!child.isDirectory() && !child.isFile()) continue;
                        result.add(new Entry(child, child.isDirectory(), child.isDirectory()
                                ? "Carpeta" : formatSize(child.length()) + " · " + formatDate(child.lastModified())));
                    }
                    sortEntries(result);
                }
            }
            final String finalError = errorMessage;
            mainHandler.post(() -> {
                if (token != operationToken.get() || isFinishing()) return;
                currentDirectory = target;
                pathField.setText(target.getAbsolutePath());
                entries.clear();
                entries.addAll(result);
                adapter.notifyDataSetChanged();
                updateNavigationButtons();
                if (finalError != null) {
                    statusText.setText(finalError);
                    if (!Environment.isExternalStorageManager() && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        statusText.setText("Falta permiso para leer GalaxyData. Toca aquí para habilitarlo.");
                        statusText.setOnClickListener(view -> openStorageSettings());
                    }
                } else {
                    statusText.setOnClickListener(null);
                    statusText.setText(result.size() + (result.size() == 1 ? " elemento" : " elementos"));
                }
            });
        }, "GalaxyFilesList").start();
    }

    private void navigateTo(File directory) {
        searchField.setText("");
        loadDirectory(directory);
    }

    private void navigateUp() {
        if (samePath(currentDirectory, rootDirectory)) return;
        File parent = currentDirectory.getParentFile();
        if (parent != null && isSafePath(parent)) navigateTo(parent);
    }

    private void goToPath(String text) {
        String value = text == null ? "" : text.trim();
        if (value.isEmpty()) {
            showError("Escribe una ruta dentro de GalaxyData.");
            return;
        }
        File candidate = new File(value);
        if (!candidate.isAbsolute()) candidate = new File(currentDirectory, value);
        try {
            candidate = candidate.getCanonicalFile();
        } catch (IOException error) {
            showError("La ruta no es válida.");
            return;
        }
        if (!isSafePath(candidate)) {
            showError("Por seguridad solo puedes abrir rutas dentro de GalaxyData.");
            pathField.setText(currentDirectory.getAbsolutePath());
            return;
        }
        if (candidate.isFile()) {
            File parent = candidate.getParentFile();
            if (parent != null) {
                currentDirectory = parent;
                pathField.setText(parent.getAbsolutePath());
                updateNavigationButtons();
                loadDirectory(parent);
            }
            showEntryActions(candidate, false);
            return;
        }
        navigateTo(candidate);
    }

    private void searchAll(String query) {
        String needle = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
        if (needle.isEmpty()) {
            loadDirectory(currentDirectory);
            return;
        }
        final int token = operationToken.incrementAndGet();
        statusText.setText("Buscando en GalaxyData…");
        new Thread(() -> {
            ArrayList<Entry> results = new ArrayList<>();
            SearchState state = new SearchState();
            searchDirectory(rootDirectory, needle, 0, results, state, token);
            sortEntries(results);
            mainHandler.post(() -> {
                if (token != operationToken.get() || isFinishing()) return;
                entries.clear();
                entries.addAll(results);
                adapter.notifyDataSetChanged();
                currentDirectory = rootDirectory;
                pathField.setText(rootDirectory.getAbsolutePath());
                updateNavigationButtons();
                String suffix = state.truncated ? " · búsqueda limitada por tamaño" : "";
                statusText.setText(results.size() + " resultado(s) en GalaxyData" + suffix);
            });
        }, "GalaxyFilesSearch").start();
    }

    private void searchDirectory(File directory, String needle, int depth,
                                 ArrayList<Entry> results, SearchState state, int token) {
        if (depth > MAX_SEARCH_DEPTH || state.visited >= MAX_SEARCH_VISITED ||
                results.size() >= MAX_SEARCH_RESULTS || token != operationToken.get()) {
            state.truncated = true;
            return;
        }
        File[] children = directory.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (token != operationToken.get() || state.visited >= MAX_SEARCH_VISITED ||
                    results.size() >= MAX_SEARCH_RESULTS) {
                state.truncated = true;
                return;
            }
            state.visited++;
            if (Files.isSymbolicLink(child.toPath())) continue;
            if (!child.isDirectory() && !child.isFile()) continue;
            String relative = relativePath(child);
            if (child.getName().toLowerCase(Locale.ROOT).contains(needle) ||
                    relative.toLowerCase(Locale.ROOT).contains(needle)) {
                boolean isDirectory = child.isDirectory();
                results.add(new Entry(child, isDirectory, isDirectory
                        ? "Carpeta · " + relative
                        : formatSize(child.length()) + " · " + relative));
            }
            if (child.isDirectory()) {
                searchDirectory(child, needle, depth + 1, results, state, token);
            }
        }
    }

    private void showEntryActions(File file, boolean directory) {
        if (!isSafePath(file)) {
            showError("No se puede acceder a esa ruta.");
            return;
        }
        String[] actions = directory
                ? new String[] {"Copiar ruta", "Guardar carpeta en el teléfono…"}
                : new String[] {"Copiar ruta", "Copiar archivo", "Guardar archivo en el teléfono…"};
        new AlertDialog.Builder(this)
                .setTitle(file.getName())
                .setItems(actions, (dialog, which) -> {
                    if (which == 0) copyPath(file);
                    else if (directory && which == 1) requestFolderExport(file);
                    else if (!directory && which == 1) copyFileToLocalClipboard(file);
                    else if (!directory && which == 2) requestFileExport(file);
                })
                .setNegativeButton("Cerrar", null)
                .show();
    }

    private void requestFileExport(File source) {
        if (exportInProgress || !source.isFile() || !isSafePath(source)) {
            showError("No se puede guardar ese archivo.");
            return;
        }
        pendingExportSource = source;
        String mimeType = android.webkit.MimeTypeMap.getSingleton()
                .getMimeTypeFromExtension(extensionOf(source.getName()));
        if (mimeType == null) mimeType = "application/octet-stream";

        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType(mimeType);
        intent.putExtra(Intent.EXTRA_TITLE, copyName(source.getName(), true));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQ_SAVE_FILE);
        } catch (Exception error) {
            pendingExportSource = null;
            showError("Android no pudo abrir el selector para guardar el archivo.");
        }
    }

    private void requestFolderExport(File source) {
        if (exportInProgress || !source.isDirectory() || !isSafePath(source)) {
            showError("No se puede guardar esa carpeta.");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Guardar carpeta en el teléfono")
                .setMessage("Se copiará esta carpeta dentro del destino que elijas. El original seguirá en GalaxyData. Elige almacenamiento interno, Descargas o una tarjeta SD; las ubicaciones en la nube no se aceptan.")
                .setPositiveButton("Elegir destino", (dialog, which) -> openFolderDestinationPicker(source))
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void openFolderDestinationPicker(File source) {
        pendingExportSource = source;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQ_SAVE_FOLDER);
        } catch (Exception error) {
            pendingExportSource = null;
            showError("Android no pudo abrir el selector de carpeta.");
        }
    }

    private boolean isLocalStorageDestination(Uri uri) {
        if (uri == null) return false;
        String authority = uri.getAuthority();
        return "com.android.externalstorage.documents".equals(authority) ||
                "com.android.providers.downloads.documents".equals(authority) ||
                "com.android.providers.media.documents".equals(authority);
    }

    private void keepUriPermission(Uri uri, int resultFlags) {
        int persistable = resultFlags & (Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if ((resultFlags & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) == 0 || persistable == 0) return;
        try {
            getContentResolver().takePersistableUriPermission(uri, persistable);
        } catch (SecurityException ignored) {
            // The temporary grant remains valid for the export operation.
        }
    }

    private String extensionOf(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 && dot < name.length() - 1 ? name.substring(dot + 1).toLowerCase(Locale.ROOT) : "";
    }

    private String copyName(String name, boolean preserveExtension) {
        int dot = preserveExtension ? name.lastIndexOf('.') : -1;
        if (dot <= 0) return name + " (copia)";
        return name.substring(0, dot) + " (copia)" + name.substring(dot);
    }

    private void copyPath(File file) {
        ClipboardManager clipboard = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            showError("Portapapeles no disponible.");
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("Ruta de GalaxyData", file.getAbsolutePath()));
        Toast.makeText(this, "Ruta copiada. Puedes pegarla en el campo de ruta.", Toast.LENGTH_SHORT).show();
    }

    private void pastePathFromClipboard() {
        ClipboardManager clipboard = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null || !clipboard.hasPrimaryClip() || clipboard.getPrimaryClip().getItemCount() == 0) {
            showError("El portapapeles está vacío.");
            return;
        }
        CharSequence value = clipboard.getPrimaryClip().getItemAt(0).coerceToText(this);
        if (value == null || value.toString().trim().isEmpty()) {
            showError("No hay una ruta de texto en el portapapeles.");
            return;
        }
        pathField.setText(value.toString().trim());
        goToPath(value.toString());
    }

    private void copyFileToLocalClipboard(File file) {
        if (!file.isFile() || !isSafePath(file)) {
            showError("Solo se pueden copiar archivos normales de GalaxyData.");
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString(PREF_COPIED_FILE, file.getAbsolutePath()).apply();
        refreshClipboardButton();
        Toast.makeText(this, "Archivo listo para pegar dentro de GalaxyData.", Toast.LENGTH_SHORT).show();
    }

    private void refreshClipboardButton() {
        if (pasteFileButton == null) return;
        String path = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_COPIED_FILE, "");
        File file = path.isEmpty() ? null : new File(path);
        boolean valid = file != null && file.isFile() && isSafePath(file);
        pasteFileButton.setEnabled(valid);
        pasteFileButton.setAlpha(valid ? 1.0f : 0.55f);
        pasteFileButton.setText(valid
                ? "Pegar archivo aquí: " + file.getName()
                : "Copiar un archivo para poder pegarlo");
    }

    private void confirmPasteFile() {
        String path = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_COPIED_FILE, "");
        File source = path.isEmpty() ? null : new File(path);
        if (source == null || !source.isFile() || !isSafePath(source)) {
            refreshClipboardButton();
            showError("El archivo copiado ya no está disponible.");
            return;
        }
        if (!isSafePath(currentDirectory) || !currentDirectory.isDirectory() || !currentDirectory.canWrite()) {
            showError("No tienes permiso para pegar en esta carpeta.");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Pegar copia del archivo")
                .setMessage(source.getName() + " · " + formatSize(source.length()) +
                        "\n\nDestino: " + currentDirectory.getAbsolutePath() +
                        "\nSe creará un nombre nuevo si ya existe uno igual. No se sobrescriben archivos.")
                .setPositiveButton("Copiar aquí", (dialog, which) -> pasteFile(source))
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void pasteFile(File source) {
        final File destinationDirectory = currentDirectory;
        final int token = operationToken.incrementAndGet();
        statusText.setText("Copiando archivo…");
        pasteFileButton.setEnabled(false);
        new Thread(() -> {
            File destination = null;
            File temporary = null;
            String errorMessage = null;
            try {
                if (!source.isFile() || !isSafePath(source) ||
                        !destinationDirectory.isDirectory() || !isSafePath(destinationDirectory)) {
                    throw new IOException("El origen o el destino dejó de ser válido.");
                }
                StatFs stat = new StatFs(destinationDirectory.getAbsolutePath());
                if (stat.getAvailableBytes() < source.length()) {
                    throw new IOException("No hay suficiente espacio libre para copiar el archivo.");
                }
                temporary = new File(destinationDirectory, ".galaxy-copy-" + UUID.randomUUID() + ".tmp");
                if (!temporary.createNewFile()) throw new IOException("No se pudo crear el archivo temporal.");
                copyFileContents(source, temporary);
                destination = moveToUniqueCopyName(temporary, destinationDirectory, source.getName());
                temporary = null;
            } catch (Exception error) {
                errorMessage = error.getMessage() == null ? "No se pudo copiar el archivo." : error.getMessage();
                if (temporary != null) temporary.delete();
            }
            final File copied = destination;
            final String finalError = errorMessage;
            mainHandler.post(() -> {
                refreshClipboardButton();
                if (finalError != null) {
                    statusText.setText(finalError);
                    Toast.makeText(this, finalError, Toast.LENGTH_LONG).show();
                } else {
                    statusText.setText("Archivo copiado: " + copied.getName());
                    Toast.makeText(this, "Copia creada sin sobrescribir el original.", Toast.LENGTH_LONG).show();
                    if (token == operationToken.get()) loadDirectory(destinationDirectory);
                }
            });
        }, "GalaxyFilesCopy").start();
    }

    private File moveToUniqueCopyName(File temporary, File directory, String originalName) throws IOException {
        int dot = originalName.lastIndexOf('.');
        String stem = dot > 0 ? originalName.substring(0, dot) : originalName;
        String extension = dot > 0 ? originalName.substring(dot) : "";
        for (int index = 1; index < 10000; index++) {
            String suffix = index == 1 ? " (copia)" : " (copia " + index + ")";
            File candidate = new File(directory, stem + suffix + extension);
            if (candidate.exists()) continue;
            try {
                Files.move(temporary.toPath(), candidate.toPath());
                return candidate;
            } catch (FileAlreadyExistsException exists) {
                // Retry with the next suffix; never replace an existing file.
            } catch (IOException error) {
                if (candidate.exists()) continue;
                throw error;
            }
        }
        throw new IOException("No se pudo crear un nombre libre para la copia.");
    }

    private void copyFileContents(File source, File destination) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(source));
             FileOutputStream rawOutput = new FileOutputStream(destination, false);
             BufferedOutputStream output = new BufferedOutputStream(rawOutput)) {
            int read;
            while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
            output.flush();
            rawOutput.getFD().sync();
        }
    }

    private void exportSingleFile(File source, Uri destination) {
        if (exportInProgress) return;
        beginExport("Guardando " + source.getName() + "…");
        new Thread(() -> {
            String errorMessage = null;
            long copiedBytes = 0;
            try {
                if (!source.isFile() || !isSafePath(source)) {
                    throw new IOException("El archivo de origen ya no está disponible.");
                }
                copiedBytes = copyFileToDocument(source, destination);
            } catch (Exception error) {
                errorMessage = (error.getMessage() == null ? "No se pudo guardar el archivo." : error.getMessage()) +
                        " Si quedó una copia incompleta en el destino, revísala antes de volver a intentarlo.";
            }
            final String finalError = errorMessage;
            final long finalBytes = copiedBytes;
            mainHandler.post(() -> finishExport(finalError == null
                    ? "Archivo guardado en el teléfono · " + formatSize(finalBytes)
                    : finalError, finalError == null));
        }, "GalaxyFileExport").start();
    }

    private long copyFileToDocument(File source, Uri destination) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        long copied = 0;
        long expectedLength = source.length();
        long expectedModified = source.lastModified();
        try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(source));
             OutputStream rawOutput = getContentResolver().openOutputStream(destination, "w")) {
            if (rawOutput == null) throw new IOException("Android no pudo abrir el archivo de destino.");
            try (BufferedOutputStream output = new BufferedOutputStream(rawOutput)) {
                int read;
                while ((read = input.read(buffer)) != -1) {
                    output.write(buffer, 0, read);
                    copied += read;
                }
                output.flush();
            }
        }
        if (copied != expectedLength || source.length() != expectedLength ||
                source.lastModified() != expectedModified) {
            throw new IOException("El archivo cambió mientras se copiaba; se canceló para evitar una copia incompleta.");
        }
        return copied;
    }

    private void exportFolder(File source, Uri treeUri) {
        if (exportInProgress) return;
        beginExport("Preparando copia de " + source.getName() + "…");
        new Thread(() -> {
            ArrayList<Uri> created = new ArrayList<>();
            ExportState state = new ExportState();
            String errorMessage = null;
            try {
                if (!source.isDirectory() || !isSafePath(source)) {
                    throw new IOException("La carpeta de origen ya no está disponible.");
                }
                String rootId = DocumentsContract.getTreeDocumentId(treeUri);
                Uri destinationRoot = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId);
                ensureFolderDestinationOutsideSource(source, treeUri);
                Uri copiedRoot = createUniqueDocument(treeUri, destinationRoot,
                        source.getName(), DocumentsContract.Document.MIME_TYPE_DIR, created, new HashMap<>());
                Map<String, Set<String>> knownChildren = new HashMap<>();
                copyDirectoryContents(source, treeUri, copiedRoot, created, state, knownChildren, 0);
            } catch (Exception error) {
                errorMessage = error.getMessage() == null ? "No se pudo copiar la carpeta." : error.getMessage();
                deleteCreatedDocuments(created);
            }
            final String finalError = errorMessage;
            final ExportState finalState = state;
            mainHandler.post(() -> {
                String message = finalError == null
                        ? "Carpeta guardada · " + finalState.filesCopied + " archivo(s) · " +
                                formatSize(finalState.bytesCopied)
                        : finalError;
                finishExport(message, finalError == null);
            });
        }, "GalaxyFolderExport").start();
    }

    private void copyDirectoryContents(File sourceDirectory, Uri treeUri, Uri destinationDirectory,
                                       ArrayList<Uri> created, ExportState state,
                                       Map<String, Set<String>> knownChildren, int depth) throws IOException {
        if (depth > MAX_SEARCH_DEPTH) {
            throw new IOException("La carpeta supera el límite de profundidad permitido.");
        }
        if (!sourceDirectory.isDirectory() || !isSafePath(sourceDirectory)) {
            throw new IOException("Se encontró una carpeta inválida dentro del origen.");
        }
        File[] children = sourceDirectory.listFiles();
        if (children == null) throw new IOException("Android no pudo leer " + sourceDirectory.getName() + ".");
        java.util.Arrays.sort(children, (left, right) ->
                left.getName().compareToIgnoreCase(right.getName()));

        for (File child : children) {
            if (++state.entriesVisited > MAX_EXPORT_FILES) {
                throw new IOException("La carpeta supera el límite de elementos permitido.");
            }
            if (Files.isSymbolicLink(child.toPath())) continue;
            if (!isSafePath(child)) throw new IOException("Se bloqueó una ruta fuera de GalaxyData.");
            if (child.isDirectory()) {
                Uri childDirectory = createUniqueDocument(treeUri, destinationDirectory,
                        child.getName(), DocumentsContract.Document.MIME_TYPE_DIR, created, knownChildren);
                copyDirectoryContents(child, treeUri, childDirectory, created, state, knownChildren, depth + 1);
            } else if (child.isFile()) {
                if (state.filesCopied >= MAX_EXPORT_FILES) {
                    throw new IOException("Se alcanzó el límite de " + MAX_EXPORT_FILES + " archivos por copia.");
                }
                String mimeType = android.webkit.MimeTypeMap.getSingleton()
                        .getMimeTypeFromExtension(extensionOf(child.getName()));
                if (mimeType == null) mimeType = "application/octet-stream";
                Uri target = createUniqueDocument(treeUri, destinationDirectory,
                        child.getName(), mimeType, created, knownChildren);
                long bytes = copyFileToDocument(child, target);
                state.filesCopied++;
                state.bytesCopied += bytes;
                if (state.filesCopied == 1 || state.filesCopied % 10 == 0) {
                    final int copiedFiles = state.filesCopied;
                    final long copiedBytes = state.bytesCopied;
                    mainHandler.post(() -> {
                        if (exportInProgress && statusText != null) {
                            statusText.setText("Copiando carpeta… " + copiedFiles + " archivo(s) · " +
                                    formatSize(copiedBytes));
                        }
                    });
                }
            }
        }
    }

    private Uri createUniqueDocument(Uri treeUri, Uri parent, String originalName, String mimeType,
                                     ArrayList<Uri> created, Map<String, Set<String>> knownChildren)
            throws IOException {
        String parentId = DocumentsContract.getDocumentId(parent);
        Set<String> existingNames = knownChildren.get(parentId);
        if (existingNames == null) {
            existingNames = queryChildNames(treeUri, parent);
            knownChildren.put(parentId, existingNames);
        }
        boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType);
        for (int index = 0; index < 10000; index++) {
            String candidate = index == 0 ? originalName : index == 1
                    ? copyName(originalName, !directory)
                    : copyNameWithIndex(originalName, index, !directory);
            if (existingNames.contains(candidate)) continue;
            Uri document;
            try {
                document = DocumentsContract.createDocument(getContentResolver(), parent, mimeType, candidate);
            } catch (Exception error) {
                Set<String> refreshedNames = queryChildNames(treeUri, parent);
                if (refreshedNames.contains(candidate)) {
                    existingNames = refreshedNames;
                    knownChildren.put(parentId, refreshedNames);
                    continue;
                }
                throw new IOException("Android no pudo crear " + candidate + ".", error);
            }
            if (document == null) throw new IOException("Android no pudo crear " + candidate + ".");
            existingNames.add(candidate);
            created.add(document);
            return document;
        }
        throw new IOException("No se encontró un nombre libre en el destino.");
    }

    private Set<String> queryChildNames(Uri treeUri, Uri parent) throws IOException {
        HashSet<String> names = new HashSet<>();
        String parentId = DocumentsContract.getDocumentId(parent);
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId);
        try (Cursor cursor = getContentResolver().query(childrenUri,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null)) {
            if (cursor == null) throw new IOException("Android no pudo consultar la carpeta de destino.");
            int nameColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            if (nameColumn < 0) throw new IOException("Android no pudo leer los nombres del destino.");
            while (cursor.moveToNext()) {
                names.add(cursor.getString(nameColumn));
            }
        } catch (SecurityException error) {
            throw new IOException("Android no permitió leer la carpeta de destino.", error);
        }
        return names;
    }

    private String copyNameWithIndex(String name, int index, boolean preserveExtension) {
        String suffix = " (copia " + index + ")";
        int dot = preserveExtension ? name.lastIndexOf('.') : -1;
        if (dot <= 0) return name + suffix;
        return name.substring(0, dot) + suffix + name.substring(dot);
    }

    private void deleteSafDocument(Uri uri) {
        if (uri == null) return;
        try {
            DocumentsContract.deleteDocument(getContentResolver(), uri);
        } catch (Exception ignored) {
            // Best-effort cleanup if the selected provider refuses deletion.
        }
    }

    private void deleteCreatedDocuments(ArrayList<Uri> created) {
        for (int index = created.size() - 1; index >= 0; index--) {
            deleteSafDocument(created.get(index));
        }
    }

    private void ensureFolderDestinationOutsideSource(File source, Uri treeUri) throws IOException {
        String treeId = DocumentsContract.getTreeDocumentId(treeUri);
        int separator = treeId.indexOf(':');
        if (separator < 0) throw new IOException("No se pudo confirmar que el destino sea almacenamiento local.");
        String volume = treeId.substring(0, separator);
        String relative = treeId.substring(separator + 1);
        File volumeRoot = "primary".equalsIgnoreCase(volume)
                ? Environment.getExternalStorageDirectory()
                : new File("/storage", volume);
        File destination = new File(volumeRoot, relative).getCanonicalFile();
        String sourcePath = source.getCanonicalPath();
        String destinationPath = destination.getPath();
        if (destinationPath.equals(sourcePath) ||
                destinationPath.startsWith(sourcePath + File.separator)) {
            throw new IOException("El destino está dentro de la carpeta de origen. Elige otra carpeta para evitar copiarla dentro de sí misma.");
        }
    }

    private void beginExport(String message) {
        exportInProgress = true;
        statusText.setText(message);
        exportProgress.setVisibility(View.VISIBLE);
        saveCurrentFolderButton.setEnabled(false);
        pasteFileButton.setEnabled(false);
        fileList.setEnabled(false);
    }

    private void finishExport(String message, boolean success) {
        exportInProgress = false;
        exportProgress.setVisibility(View.GONE);
        saveCurrentFolderButton.setEnabled(true);
        fileList.setEnabled(true);
        refreshClipboardButton();
        statusText.setText(message);
        Toast.makeText(this, message, success ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT).show();
    }

    private void updateNavigationButtons() {
        if (upButton != null) upButton.setEnabled(!samePath(currentDirectory, rootDirectory));
        refreshClipboardButton();
    }

    private void openStorageSettings() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(android.net.Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception error) {
            Toast.makeText(this, "Abre el permiso de acceso a archivos desde Ajustes de Android.",
                    Toast.LENGTH_LONG).show();
        }
    }

    private boolean isSafePath(File file) {
        try {
            String rootPath = rootDirectory.getCanonicalPath();
            File canonical = file.getCanonicalFile();
            String path = canonical.getPath();
            if (!path.equals(rootPath) && !path.startsWith(rootPath + File.separator)) return false;
            if (Files.isSymbolicLink(rootDirectory.toPath())) return false;
            String relative = rootDirectory.toPath().relativize(canonical.toPath()).toString();
            File cursor = rootDirectory;
            if (!relative.isEmpty()) {
                String[] parts = relative.split(java.util.regex.Pattern.quote(File.separator));
                for (String part : parts) {
                    cursor = new File(cursor, part);
                    if (Files.isSymbolicLink(cursor.toPath())) return false;
                }
            }
            return true;
        } catch (IOException | IllegalArgumentException error) {
            return false;
        }
    }

    private String relativePath(File file) {
        try {
            return rootDirectory.toPath().relativize(file.getCanonicalFile().toPath()).toString();
        } catch (IOException | IllegalArgumentException error) {
            return file.getName();
        }
    }

    private void sortEntries(ArrayList<Entry> values) {
        Collections.sort(values, (left, right) -> {
            if (left.directory != right.directory) return left.directory ? -1 : 1;
            return left.file.getName().compareToIgnoreCase(right.file.getName());
        });
    }

    private boolean samePath(File left, File right) {
        try {
            return left != null && right != null &&
                    left.getCanonicalPath().equals(right.getCanonicalPath());
        } catch (IOException error) {
            return false;
        }
    }

    private String formatSize(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024L * 1024L) return String.format(Locale.getDefault(), "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024L * 1024L) {
            return String.format(Locale.getDefault(), "%.1f MB", bytes / (1024.0 * 1024.0));
        }
        return String.format(Locale.getDefault(), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }

    private String formatDate(long time) {
        return DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT, Locale.getDefault())
                .format(new Date(time));
    }

    private void showError(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    public void onBackPressed() {
        if (currentDirectory != null && !samePath(currentDirectory, rootDirectory)) navigateUp();
        else super.onBackPressed();
    }

    private final class FileListAdapter extends BaseAdapter {
        @Override public int getCount() { return entries.size(); }
        @Override public Object getItem(int position) { return entries.get(position); }
        @Override public long getItemId(int position) { return position; }

        @Override
        public View getView(int position, View recycled, android.view.ViewGroup parent) {
            LinearLayout row;
            TextView type;
            TextView name;
            TextView detail;
            if (recycled instanceof LinearLayout) {
                row = (LinearLayout)recycled;
                type = (TextView)row.getChildAt(0);
                LinearLayout labels = (LinearLayout)row.getChildAt(1);
                name = (TextView)labels.getChildAt(0);
                detail = (TextView)labels.getChildAt(1);
            } else {
                row = new LinearLayout(GameFilesActivity.this);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(7), dp(5), dp(7), dp(5));
                type = text("", 10, 0xFF6DD6BE, true);
                type.setGravity(Gravity.CENTER);
                row.addView(type, new LinearLayout.LayoutParams(dp(48), dp(42)));
                LinearLayout labels = new LinearLayout(GameFilesActivity.this);
                labels.setOrientation(LinearLayout.VERTICAL);
                labels.setPadding(dp(8), 0, 0, 0);
                name = text("", 13, 0xFFF8FAFC, true);
                name.setMaxLines(1);
                name.setEllipsize(android.text.TextUtils.TruncateAt.END);
                detail = text("", 10, 0xFF9AA4B2, false);
                detail.setMaxLines(1);
                detail.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
                labels.addView(name);
                labels.addView(detail);
                row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
            }
            Entry entry = entries.get(position);
            type.setText(entry.directory ? "CARPETA" : "ARCHIVO");
            name.setText(entry.file.getName());
            detail.setText(entry.detail);
            return row;
        }
    }
}
