package com.openport.galaxyandroid;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Imports resource-only ZIP overlays. ZIP contents are never executed. */
public final class ModZipImporter {
    private static final long MAX_ARCHIVE_BYTES = 256L * 1024L * 1024L;
    private static final long MAX_TOTAL_EXPANDED_BYTES = 512L * 1024L * 1024L;
    private static final long MAX_FILE_BYTES = 128L * 1024L * 1024L;
    private static final int MAX_ENTRIES = 4096;
    private static final int MAX_ARCHIVE_DEPTH = 16;
    private static final int MAX_RESOURCE_FILES = 4096;

    public static final class Result {
        public final String id;
        public final String name;
        public final int resourceFiles;
        public final boolean compatibilityChecked;
        public final int matchingBaseFiles;
        public final int missingBaseFiles;

        Result(String id, String name, int resourceFiles, boolean compatibilityChecked,
               int matchingBaseFiles, int missingBaseFiles) {
            this.id = id;
            this.name = name;
            this.resourceFiles = resourceFiles;
            this.compatibilityChecked = compatibilityChecked;
            this.matchingBaseFiles = matchingBaseFiles;
            this.missingBaseFiles = missingBaseFiles;
        }
    }

    private static final class Compatibility {
        final boolean checked;
        final int matching;
        final int missing;

        Compatibility(boolean checked, int matching, int missing) {
            this.checked = checked;
            this.matching = matching;
            this.missing = missing;
        }
    }

    private static final class Metadata {
        String name = "";
        String version = "";
        int priority = 0;
    }

    private ModZipImporter() {}

    public static synchronized Result install(InputStream input, File modsDirectory,
                                              String archiveName) throws IOException {
        if (input == null || modsDirectory == null) throw new IOException("Falta el archivo ZIP o la carpeta de mods.");

        File modsInput = modsDirectory.getAbsoluteFile();
        if (Files.isSymbolicLink(modsInput.toPath())) {
            throw new IOException("La ubicación de mods no puede ser un enlace.");
        }
        File mods = modsDirectory.getCanonicalFile();
        if (!mods.exists() && !mods.mkdirs() && !mods.isDirectory()) {
            throw new IOException("No se pudo crear la carpeta local de mods.");
        }
        if (!mods.isDirectory() || Files.isSymbolicLink(mods.toPath())) {
            throw new IOException("La ubicación de mods no es una carpeta segura.");
        }

        File stage = new File(mods, ".import-" + UUID.randomUUID());
        if (!stage.mkdir()) throw new IOException("No se pudo preparar una importación temporal.");
        try {
            File unpackRoot = new File(stage, "unpacked");
            if (!unpackRoot.mkdir()) throw new IOException("No se pudo preparar la extracción.");
            extract(input, unpackRoot);

            List<File> candidates = new ArrayList<>();
            findFilesDirectories(unpackRoot, 0, candidates);
            if (candidates.size() != 1) {
                throw new IOException(candidates.isEmpty()
                        ? "El ZIP no contiene files/ con recursos compatibles."
                        : "El ZIP contiene varias carpetas files/; no se puede elegir una sin ambigüedad.");
            }

            File filesDirectory = candidates.get(0);
            int resourceCount = countResources(filesDirectory, 0, new int[] {0});
            if (resourceCount == 0) throw new IOException("El ZIP no contiene recursos utilizables en files/.");
            Compatibility compatibility = checkGamePaths(mods, filesDirectory);

            File packRoot = filesDirectory.getParentFile();
            File manifest = new File(packRoot, "mod.ini");
            File metadataSource = manifest.exists() ? manifest : new File(unpackRoot, "mod.ini");
            Metadata metadata = readMetadata(metadataSource);
            String displayName = safeField(metadata.name, 48);
            if (displayName.isEmpty()) displayName = safeField(stripZipSuffix(archiveName), 48);
            if (displayName.isEmpty()) displayName = "Mod local";
            String version = safeField(metadata.version, 20);
            String id = uniqueId(mods, displayName);

            if (!"files".equals(filesDirectory.getName())) {
                File canonicalFiles = new File(packRoot, "files");
                if (canonicalFiles.exists() || !filesDirectory.renameTo(canonicalFiles)) {
                    throw new IOException("No se pudo normalizar la carpeta files/ del ZIP.");
                }
            }
            writeManifest(manifest, displayName, version, metadata.priority);

            File destination = new File(mods, id);
            if (destination.exists() || !packRoot.renameTo(destination)) {
                throw new IOException("No se pudo guardar el mod sin reemplazar otro paquete.");
            }
            return new Result(id, displayName, resourceCount, compatibility.checked,
                    compatibility.matching, compatibility.missing);
        } finally {
            deleteTree(stage);
        }
    }

    private static void extract(InputStream source, File root) throws IOException {
        String canonicalRoot = root.getCanonicalPath() + File.separator;
        long expandedTotal = 0;
        int entryCount = 0;
        try (ZipInputStream zip = new ZipInputStream(
                new BufferedInputStream(new BoundedInputStream(source, MAX_ARCHIVE_BYTES)))) {
            ZipEntry entry;
            byte[] buffer = new byte[32 * 1024];
            while ((entry = zip.getNextEntry()) != null) {
                if (++entryCount > MAX_ENTRIES) throw new IOException("El ZIP supera el límite de archivos.");
                String relative = normalizeEntryName(entry.getName(), entry.isDirectory());
                if (relative == null) throw new IOException("El ZIP contiene una ruta absoluta o insegura.");
                File target = new File(root, relative);
                String canonicalTarget = target.getCanonicalPath();
                if (!canonicalTarget.startsWith(canonicalRoot)) {
                    throw new IOException("El ZIP intenta escribir fuera de su carpeta temporal.");
                }
                if (entry.isDirectory()) {
                    ensureDirectory(root, relative);
                    zip.closeEntry();
                    continue;
                }

                long declaredSize = entry.getSize();
                if (declaredSize > MAX_FILE_BYTES) throw new IOException("Un archivo del ZIP supera el tamaño permitido.");
                ensureDirectory(root, parentPath(relative));
                if (Files.isSymbolicLink(target.toPath()) || target.exists()) {
                    throw new IOException("El ZIP repite una ruta o contiene una entrada no permitida.");
                }

                long fileBytes = 0;
                try (BufferedOutputStream output = new BufferedOutputStream(new FileOutputStream(target, false))) {
                    int read;
                    while ((read = zip.read(buffer)) != -1) {
                        fileBytes += read;
                        expandedTotal += read;
                        if (fileBytes > MAX_FILE_BYTES || expandedTotal > MAX_TOTAL_EXPANDED_BYTES) {
                            throw new IOException("El ZIP expandido supera el límite seguro.");
                        }
                        output.write(buffer, 0, read);
                    }
                }
                zip.closeEntry();
            }
        }
    }

    private static String normalizeEntryName(String entryName, boolean directory) {
        if (entryName == null || entryName.isEmpty() || entryName.length() > 1024 ||
                entryName.charAt(0) == '/' || entryName.charAt(0) == '\\' ||
                entryName.indexOf('\\') >= 0 || entryName.indexOf(':') >= 0) {
            return null;
        }
        String name = directory && entryName.endsWith("/")
                ? entryName.substring(0, entryName.length() - 1) : entryName;
        String[] parts = name.split("/", -1);
        if (parts.length == 0 || parts.length > MAX_ARCHIVE_DEPTH) return null;
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part)) return null;
            for (int i = 0; i < part.length(); i++) {
                char c = part.charAt(i);
                if (c < 0x20 || c == 0x7f) return null;
            }
        }
        return name;
    }

    private static void ensureDirectory(File root, String relative) throws IOException {
        if (relative == null || relative.isEmpty()) return;
        File current = root;
        String[] parts = relative.split("/");
        for (String part : parts) {
            current = new File(current, part);
            if (Files.isSymbolicLink(current.toPath())) throw new IOException("El ZIP contiene un enlace no permitido.");
            if (!current.exists() && !current.mkdir()) throw new IOException("No se pudo crear una carpeta del mod.");
            if (!current.isDirectory()) throw new IOException("El ZIP mezcla una carpeta y un archivo con la misma ruta.");
        }
    }

    private static String parentPath(String relative) {
        int slash = relative.lastIndexOf('/');
        return slash < 0 ? "" : relative.substring(0, slash);
    }

    private static void findFilesDirectories(File current, int depth, List<File> found) throws IOException {
        if (depth > 4 || found.size() > 1) return;
        File[] children = current.listFiles();
        if (children == null) throw new IOException("No se pudo examinar el ZIP.");
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) throw new IOException("El ZIP creó una entrada no permitida.");
            if (!child.isDirectory()) continue;
            if ("files".equalsIgnoreCase(child.getName())) {
                found.add(child);
                if (found.size() > 1) return;
            } else {
                findFilesDirectories(child, depth + 1, found);
            }
        }
    }

    private static int countResources(File directory, int depth, int[] total) throws IOException {
        if (depth > 10 || total[0] > MAX_RESOURCE_FILES) {
            throw new IOException("La estructura del mod supera los límites de profundidad o archivos.");
        }
        File[] children = directory.listFiles();
        if (children == null) throw new IOException("No se pudo leer files/.");
        int local = 0;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) throw new IOException("El mod contiene un enlace no permitido.");
            if (child.isDirectory()) {
                local += countResources(child, depth + 1, total);
            } else if (child.isFile() && child.length() > 0) {
                total[0]++;
                if (total[0] > MAX_RESOURCE_FILES) {
                    throw new IOException("El mod supera el límite de archivos.");
                }
                local++;
            }
        }
        return local;
    }

    private static Compatibility checkGamePaths(File modsDirectory, File modFiles)
            throws IOException {
        File gameRoot = modsDirectory.getParentFile();
        if (gameRoot == null) return new Compatibility(false, 0, 0);
        File gameFiles = new File(gameRoot, "files");
        if (Files.isSymbolicLink(gameFiles.toPath()) || !gameFiles.isDirectory()) {
            return new Compatibility(false, 0, 0);
        }

        int[] counts = new int[2];
        countMatchingGamePaths(gameFiles, modFiles, modFiles, counts, 0);
        return new Compatibility(true, counts[0], counts[1]);
    }

    private static void countMatchingGamePaths(File gameFiles, File modFiles, File current,
                                               int[] counts, int depth) throws IOException {
        if (depth > 10) throw new IOException("La estructura del mod supera la profundidad permitida.");
        File[] children = current.listFiles();
        if (children == null) throw new IOException("No se pudo leer files/ para comprobar sus rutas.");
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) {
                throw new IOException("El mod contiene un enlace no permitido.");
            }
            if (child.isDirectory()) {
                countMatchingGamePaths(gameFiles, modFiles, child, counts, depth + 1);
            } else if (child.isFile() && child.length() > 0) {
                String relative = modFiles.toPath().relativize(child.toPath())
                        .toString().replace(File.separatorChar, '/');
                if (isExistingGameResource(gameFiles, relative)) counts[0]++;
                else counts[1]++;
            }
        }
    }

    private static boolean isExistingGameResource(File gameFiles, String relative)
            throws IOException {
        if (!isSafeResourcePath(relative) || Files.isSymbolicLink(gameFiles.toPath())) return false;
        String basePath = gameFiles.getCanonicalPath();
        File current = gameFiles;
        String[] parts = relative.split("/");
        for (int i = 0; i < parts.length; i++) {
            current = new File(current, parts[i]);
            if (Files.isSymbolicLink(current.toPath())) return false;
            String canonical = current.getCanonicalPath();
            if (!canonical.startsWith(basePath + File.separator) ||
                    !current.exists()) return false;
            if (i == parts.length - 1) return current.isFile() && current.length() > 0;
            if (!current.isDirectory()) return false;
        }
        return false;
    }

    private static boolean isSafeResourcePath(String path) {
        if (path == null || path.isEmpty() || path.length() > 1024 ||
                path.startsWith("/") || path.startsWith("\\")) return false;
        String[] parts = path.split("/", -1);
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part) ||
                    part.indexOf('\\') >= 0 || part.indexOf(':') >= 0) return false;
            for (int i = 0; i < part.length(); i++) {
                char c = part.charAt(i);
                if (c < 0x20 || c == 0x7f) return false;
            }
        }
        return true;
    }

    private static Metadata readMetadata(File manifest) throws IOException {
        Metadata metadata = new Metadata();
        if (!manifest.exists()) return metadata;
        if (!manifest.isFile() || Files.isSymbolicLink(manifest.toPath()) || manifest.length() > 8192) {
            throw new IOException("El mod.ini está dañado o supera el tamaño permitido.");
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(manifest), StandardCharsets.UTF_8))) {
            String line;
            int bytes = 0;
            while ((line = reader.readLine()) != null) {
                bytes += line.length() + 1;
                if (bytes > 8192) throw new IOException("El mod.ini supera el tamaño permitido.");
                String text = line.trim();
                if (text.isEmpty() || text.startsWith("#") || text.startsWith(";")) continue;
                int equal = text.indexOf('=');
                if (equal < 0) continue;
                String key = text.substring(0, equal).trim().toLowerCase(Locale.ROOT);
                String value = text.substring(equal + 1).trim();
                if ("name".equals(key)) metadata.name = safeField(value, 48);
                else if ("version".equals(key)) metadata.version = safeField(value, 20);
                else if ("priority".equals(key)) {
                    try {
                        metadata.priority = Math.max(-100, Math.min(100, Integer.parseInt(value)));
                    } catch (NumberFormatException ignored) {
                        metadata.priority = 0;
                    }
                }
            }
        }
        return metadata;
    }

    private static void writeManifest(File manifest, String name, String version, int priority)
            throws IOException {
        String content = "name=" + safeField(name, 48) + "\n" +
                "version=" + safeField(version, 20) + "\n" +
                "enabled=0\npriority=" + Math.max(-100, Math.min(100, priority)) + "\n";
        try (FileOutputStream output = new FileOutputStream(manifest, false)) {
            output.write(content.getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
    }

    private static String safeField(String value, int maxLength) {
        if (value == null) return "";
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < value.length() && result.length() < maxLength; i++) {
            char c = value.charAt(i);
            if (c < 0x20 || c == 0x7f || c == '\n' || c == '\r') continue;
            result.append(c);
        }
        return result.toString().trim();
    }

    private static String stripZipSuffix(String name) {
        if (name == null) return "";
        String trimmed = name.trim();
        return trimmed.toLowerCase(Locale.ROOT).endsWith(".zip")
                ? trimmed.substring(0, trimmed.length() - 4) : trimmed;
    }

    private static String uniqueId(File mods, String displayName) {
        String base = displayName.toLowerCase(Locale.ROOT)
                .replaceAll("[^a-z0-9]+", "-")
                .replaceAll("^-|-$", "");
        if (base.isEmpty()) base = "local-mod";
        if (base.length() > 32) base = base.substring(0, 32).replaceAll("-+$", "");
        String candidate = base;
        for (int suffix = 2; new File(mods, candidate).exists(); suffix++) {
            String tail = "-" + suffix;
            candidate = base.substring(0, Math.min(base.length(), 48 - tail.length())) + tail;
        }
        return candidate;
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        if (Files.isSymbolicLink(file.toPath())) {
            file.delete();
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) deleteTree(child);
        }
        file.delete();
    }

    private static final class BoundedInputStream extends FilterInputStream {
        private final long limit;
        private long count;

        BoundedInputStream(InputStream input, long limit) {
            super(input);
            this.limit = limit;
        }

        @Override
        public int read() throws IOException {
            if (count >= limit) {
                int extra = in.read();
                if (extra < 0) return -1;
                throw new IOException("El ZIP supera el tamaño comprimido permitido.");
            }
            int value = in.read();
            if (value >= 0) count++;
            return value;
        }

        @Override
        public int read(byte[] buffer, int offset, int length) throws IOException {
            if (length == 0) return 0;
            if (count >= limit) {
                int extra = in.read();
                if (extra < 0) return -1;
                throw new IOException("El ZIP supera el tamaño comprimido permitido.");
            }
            int allowed = (int) Math.min((long) length, limit - count);
            int read = in.read(buffer, offset, allowed);
            if (read > 0) count += read;
            return read;
        }
    }
}
