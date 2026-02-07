package com.winlator.xconnector;

import com.winlator.core.FileUtils;
import java.io.File;

/* loaded from: classes.dex */
public class UnixSocketConfig {
    public final String path;

    private UnixSocketConfig(String path) {
        this.path = path;
    }

    public static UnixSocketConfig create(String rootPath, String relativePath) {
        File socketFile = new File(rootPath, relativePath);
        String dirname = FileUtils.getDirname(relativePath);
        if (dirname.lastIndexOf("/") > 0) {
            File socketDir = new File(rootPath, FileUtils.getDirname(relativePath));
            FileUtils.delete(socketDir);
            socketDir.mkdirs();
        } else {
            socketFile.delete();
        }
        return new UnixSocketConfig(socketFile.getPath());
    }
}
