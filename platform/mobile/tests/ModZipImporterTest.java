package com.openport.galaxyandroid;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class ModZipImporterTest {
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("galaxy-mod-import-test").toFile();
        File mods = new File(root, "mods");
        try {
            File baseResource = new File(root, "files/StageData/Test.arc");
            check(baseResource.getParentFile().mkdirs(), "could not create base game files");
            Files.write(baseResource.toPath(), "original resource".getBytes(StandardCharsets.UTF_8));
            byte[] wrapped = zip(
                    "Readme.txt", "ignored",
                    "pack/mod.ini", "name=Texture Pack\nversion=1.2\nenabled=1\npriority=4\n",
                    "pack/files/StageData/Test.arc", "replacement",
                    "pack/files/StageData/Missing.arc", "not in base game");
            ModZipImporter.Result first = ModZipImporter.install(
                    new ByteArrayInputStream(wrapped), mods, "Texture Pack.zip");
            check(first.id.equals("texture-pack"), "id was not normalized");
            check(first.resourceFiles == 2, "resource count mismatch");
            check(first.compatibilityChecked, "base game path check was not run");
            check(first.matchingBaseFiles == 1, "matching base file count mismatch");
            check(first.missingBaseFiles == 1, "missing base file count mismatch");
            File installed = new File(mods, first.id);
            check(new File(installed, "files/StageData/Test.arc").isFile(), "resource not installed");
            check(new File(installed, "files/StageData/Missing.arc").isFile(),
                    "unmatched resource should still import without altering the base game");
            String manifest = new String(Files.readAllBytes(new File(installed, "mod.ini").toPath()),
                    StandardCharsets.UTF_8);
            check(manifest.contains("enabled=0"), "imported mod must start disabled");
            check(manifest.contains("priority=4"), "priority was not preserved");

            byte[] rootFiles = zip("files/Textures/T.arc", "texture");
            ModZipImporter.Result second = ModZipImporter.install(
                    new ByteArrayInputStream(rootFiles), mods, "Texture Pack.zip");
            check(second.id.equals("texture-pack-2"), "existing mod was overwritten");
            check(new File(mods, second.id + "/mod.ini").isFile(), "default manifest missing");

            byte[] rootManifest = zip(
                    "mod.ini", "name=Wrapper Metadata\nversion=2\n",
                    "outer/files/StageData/Other.arc", "another resource");
            ModZipImporter.Result third = ModZipImporter.install(
                    new ByteArrayInputStream(rootManifest), mods, "wrapper.zip");
            check(third.name.equals("Wrapper Metadata"), "root metadata was not used for wrapper layout");
            check(new File(mods, third.id + "/files/StageData/Other.arc").isFile(),
                    "wrapped resource was not installed");

            File noGameRoot = new File(root, "not-installed-yet");
            File noGameMods = new File(noGameRoot, "mods");
            ModZipImporter.Result unverified = ModZipImporter.install(
                    new ByteArrayInputStream(zip("files/ObjectData/Example.arc", "resource")),
                    noGameMods, "Unverified.zip");
            check(!unverified.compatibilityChecked,
                    "missing game data should not block a valid resource-only import");

            expectFailure(zip("files/../../outside.bin", "escape"), mods,
                    "path traversal must be rejected");
            check(!new File(root, "outside.bin").exists(), "path traversal wrote outside mods");
            expectFailure(zip("readme.txt", "not a resource mod"), mods,
                    "ZIP without files/ must be rejected");
            check(mods.listFiles().length == 3, "failed import left staging files behind");
            System.out.println("ModZipImporterTest: PASS");
        } finally {
            delete(root);
        }
    }

    private static byte[] zip(String... pairs) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (int i = 0; i < pairs.length; i += 2) {
                zip.putNextEntry(new ZipEntry(pairs[i]));
                zip.write(pairs[i + 1].getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    private static void expectFailure(byte[] archive, File mods, String message) throws Exception {
        boolean failed = false;
        try {
            ModZipImporter.install(new ByteArrayInputStream(archive), mods, "invalid.zip");
        } catch (java.io.IOException expected) {
            failed = true;
        }
        check(failed, message);
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void delete(File file) throws Exception {
        if (!file.exists()) return;
        if (Files.isSymbolicLink(file.toPath())) {
            Files.delete(file.toPath());
        } else if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) delete(child);
            Files.delete(file.toPath());
        } else {
            Files.delete(file.toPath());
        }
    }
}
