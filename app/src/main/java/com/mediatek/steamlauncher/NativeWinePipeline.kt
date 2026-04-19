package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Launcher for the GameNative-style native ARM64 Wine pipeline.
 *
 * Runs Pepelespooder's Bionic Wine binary (shipped in jniLibs as libwine_native.so)
 * directly from the app process, mirroring GameNative's BionicProgramLauncherComponent.
 *
 * First-iteration scope: wineVersion(). No prefix, no X11, no wineserver — just
 * the wine binary printing its compiled-in version. If this works, we scaffold
 * forward to wineserver + notepad.
 */
class NativeWinePipeline(private val context: Context) {

    companion object {
        private const val TAG = "NativeWinePipeline"
        private const val WINE_LIB = "libwine_native.so"
    }

    private val nativeLibDir: String
        get() = context.applicationInfo.nativeLibraryDir

    private val dataDir: String
        get() = context.filesDir.absolutePath

    private val winePath: String
        get() = "$nativeLibDir/$WINE_LIB"

    data class Result(val exitCode: Int, val stdout: String, val stderr: String) {
        override fun toString() = "exit=$exitCode\n--- stdout ---\n$stdout--- stderr ---\n$stderr"
    }

    fun wineBinaryExists(): Boolean = File(winePath).exists()

    /**
     * Run `wine --version`. Does not need a WINEPREFIX or wineserver — wine
     * prints its compiled-in version string and exits.
     */
    fun wineVersion(): Result {
        if (!wineBinaryExists()) {
            return Result(-1, "", "wine binary missing: $winePath")
        }

        val env = buildEnv()
        Log.i(TAG, "Running $winePath --version with env keys: ${env.keys}")

        val pb = ProcessBuilder(winePath, "--version")
            .directory(File(dataDir))
            .redirectErrorStream(false)
        pb.environment().clear()
        pb.environment().putAll(env)

        return try {
            val proc = pb.start()
            val out = proc.inputStream.bufferedReader().readText()
            val err = proc.errorStream.bufferedReader().readText()
            val code = proc.waitFor()
            Result(code, out, err)
        } catch (t: Throwable) {
            Log.e(TAG, "wine --version failed", t)
            Result(-2, "", t.stackTraceToString())
        }
    }

    /**
     * Env var recipe derived from GameNative's BionicProgramLauncherComponent.
     * Minimal subset — no LD_PRELOAD yet (skipped until we add sysvshm/evshim
     * from jniLibs). `wine --version` does not exercise the hooks, so this is
     * enough for the first iteration.
     */
    private fun buildEnv(): Map<String, String> {
        val wineTree = "$dataDir/proton11"
        val ldPath = listOf(
            nativeLibDir,
            "$wineTree/lib/wine/aarch64-unix",
            "$wineTree/lib",
            "/system/lib64",
        ).joinToString(":")
        return mapOf(
            "PATH" to "$nativeLibDir:$wineTree/bin:/system/bin",
            "LD_LIBRARY_PATH" to ldPath,
            "WINELOADER" to "$wineTree/bin/wine",
            "WINEDLLPATH" to "$wineTree/lib/wine",
            "HOME" to dataDir,
            "TMPDIR" to "$dataDir/tmp",
            "WINEDEBUG" to "-all",
        )
    }
}
