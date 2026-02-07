package com.winlator.core;

import java.io.File;
import java.nio.file.Files;

/**
 * Minimal FileUtils with just the methods needed by XConnector.
 */
public abstract class FileUtils {

    public static String getDirname(String path) {
        if (path == null) {
            return "";
        }
        // Remove trailing slashes
        while (path.endsWith("/") || path.endsWith("\\")) {
            path = path.substring(0, path.length() - 1);
        }
        int index = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
        if (index <= 0) {
            return "";
        }
        return path.substring(0, index);
    }

    public static boolean isSymlink(File file) {
        return Files.isSymbolicLink(file.toPath());
    }

    public static boolean delete(File targetFile) {
        if (targetFile == null) {
            return false;
        }
        if (targetFile.isDirectory() && !isSymlink(targetFile)) {
            File[] files = targetFile.listFiles();
            if (files != null) {
                for (File file : files) {
                    delete(file);
                }
            }
        }
        return targetFile.delete();
    }
}
