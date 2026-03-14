package com.mediatek.steamlauncher

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import `in`.dragonbra.javasteam.depotdownloader.DepotDownloader
import `in`.dragonbra.javasteam.depotdownloader.IDownloadListener
import `in`.dragonbra.javasteam.depotdownloader.data.AppItem
import `in`.dragonbra.javasteam.depotdownloader.data.DownloadItem
import `in`.dragonbra.javasteam.enums.EResult
import `in`.dragonbra.javasteam.steam.authentication.AuthPollResult
import `in`.dragonbra.javasteam.steam.authentication.AuthSessionDetails
import `in`.dragonbra.javasteam.steam.authentication.IAuthenticator
import `in`.dragonbra.javasteam.steam.handlers.steamapps.License
import `in`.dragonbra.javasteam.steam.handlers.steamapps.callback.LicenseListCallback
import `in`.dragonbra.javasteam.steam.handlers.steamuser.LogOnDetails
import `in`.dragonbra.javasteam.steam.handlers.steamuser.SteamUser
import `in`.dragonbra.javasteam.steam.handlers.steamuser.callback.LoggedOnCallback
import `in`.dragonbra.javasteam.steam.steamclient.SteamClient
import `in`.dragonbra.javasteam.steam.steamclient.callbackmgr.CallbackManager
import `in`.dragonbra.javasteam.steam.steamclient.callbacks.ConnectedCallback
import `in`.dragonbra.javasteam.steam.steamclient.callbacks.DisconnectedCallback
import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Native Android depot downloader using JavaSteam.
 * Downloads Steam content (e.g. AppID 228980 Steamworks Common Redistributables)
 * directly on ARM64 without FEX-Emu, bypassing the filesystem overlay issues
 * that prevent Steam's own download pipeline from completing under emulation.
 */
class SteamContentDownloader(private val context: Context) {

    companion object {
        private const val TAG = "SteamContentDL"
        private const val PREFS_NAME = "steam_content_dl"
        private const val KEY_REFRESH_TOKEN = "refresh_token"
        private const val KEY_ACCOUNT_NAME = "account_name"
        private const val CONNECT_TIMEOUT_SEC = 30L
        private const val LOGIN_TIMEOUT_SEC = 60L
        private const val LICENSE_TIMEOUT_SEC = 30L
    }

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    private var steamClient: SteamClient? = null
    private var callbackManager: CallbackManager? = null
    private var callbackThread: Thread? = null
    private val callbackRunning = AtomicBoolean(false)
    private var licenses: List<License>? = null
    private val isRunning = AtomicBoolean(false)

    /** Listener for progress updates from the UI */
    var onProgress: ((String) -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onComplete: (() -> Unit)? = null

    /**
     * Download an app's content natively.
     * Call from a background thread.
     *
     * @param appId Steam AppID to download (e.g. 228980)
     * @param installDir Install directory name (e.g. "Steamworks Shared")
     * @param username Steam username (only needed if no saved refresh token)
     * @param password Steam password (only needed if no saved refresh token)
     * @param authenticator IAuthenticator for 2FA prompts
     */
    fun downloadApp(
        appId: Int,
        installDir: String,
        username: String,
        password: String,
        authenticator: IAuthenticator
    ) {
        if (isRunning.getAndSet(true)) {
            onError?.invoke("Download already in progress")
            return
        }

        try {
            val steamappsDir = getSteamappsDir()
            val commonDir = File(steamappsDir, "common/$installDir")
            commonDir.mkdirs()

            notify("Connecting to Steam...")
            connect()

            notify("Authenticating...")
            login(username, password, authenticator)

            notify("Waiting for license list...")
            waitForLicenses()

            notify("Starting download of AppID $appId...")
            download(appId, installDir, steamappsDir)

            notify("Generating appmanifest_$appId.acf...")
            generateAppManifest(appId, installDir, steamappsDir)

            notify("Download complete!")
            onComplete?.invoke()
        } catch (e: Exception) {
            Log.e(TAG, "Download failed", e)
            onError?.invoke("Download failed: ${e.message}")
        } finally {
            disconnect()
            isRunning.set(false)
        }
    }

    /**
     * Check if an app is already installed (has valid appmanifest AND actual content files).
     */
    fun isAppInstalled(appId: Int): Boolean {
        val manifest = File(getSteamappsDir(), "appmanifest_$appId.acf")
        if (!manifest.exists()) return false
        val content = manifest.readText()
        if (!content.contains("\"StateFlags\"\t\t\"4\"")) return false

        // Extract installdir from manifest and verify actual files exist
        val installDirMatch = Regex("\"installdir\"\\s+\"([^\"]+)\"").find(content)
        val installDir = installDirMatch?.groupValues?.get(1) ?: return false
        val contentDir = File(getSteamappsDir(), "common/$installDir")
        if (!contentDir.exists()) return false

        // Must have at least some files (empty dir = fake manifest from failed download)
        val fileCount = contentDir.walkTopDown().filter { it.isFile }.take(3).count()
        if (fileCount == 0) {
            Log.w(TAG, "Manifest exists for $appId but content dir is empty, needs re-download")
            return false
        }
        return true
    }

    private fun getSteamappsDir(): String {
        val app = SteamLauncherApp.instance
        return "${app.getFexRootfsDir()}/home/user/.steam/debian-installation/steamapps"
    }

    // ---- Connection ----

    private fun connect() {
        val client = SteamClient()
        val manager = CallbackManager(client)

        steamClient = client
        callbackManager = manager

        val connectedLatch = CountDownLatch(1)

        // Start callback pump thread
        callbackRunning.set(true)
        callbackThread = Thread({
            while (callbackRunning.get()) {
                try {
                    manager.runWaitCallbacks(500)
                } catch (e: Exception) {
                    if (!callbackRunning.get()) break
                    Log.w(TAG, "Callback error", e)
                }
            }
        }, "JavaSteam-Callbacks").also { it.isDaemon = true; it.start() }

        manager.subscribe(ConnectedCallback::class.java) {
            Log.i(TAG, "Connected to Steam")
            connectedLatch.countDown()
        }

        manager.subscribe(DisconnectedCallback::class.java) {
            Log.w(TAG, "Disconnected from Steam")
        }

        client.connect()

        if (!connectedLatch.await(CONNECT_TIMEOUT_SEC, TimeUnit.SECONDS)) {
            throw RuntimeException("Connection to Steam timed out")
        }
    }

    private fun disconnect() {
        callbackRunning.set(false)
        try {
            steamClient?.disconnect()
        } catch (e: Exception) {
            Log.w(TAG, "Error disconnecting", e)
        }
        // Wait for callback thread to finish gracefully (max 2s)
        try {
            callbackThread?.join(2000)
        } catch (_: InterruptedException) {}
        callbackThread = null
        steamClient = null
        callbackManager = null
        licenses = null
    }

    // ---- Authentication ----

    private fun login(username: String, password: String, authenticator: IAuthenticator) {
        val client = steamClient ?: throw IllegalStateException("Not connected")
        val steamUser = client.getHandler(SteamUser::class.java)
            ?: throw IllegalStateException("SteamUser handler not found")

        val savedToken = prefs.getString(KEY_REFRESH_TOKEN, null)
        val savedAccount = prefs.getString(KEY_ACCOUNT_NAME, null)

        if (savedToken != null && savedAccount != null) {
            Log.i(TAG, "Using saved refresh token for $savedAccount")
            loginWithToken(steamUser, savedAccount, savedToken)
            return
        }

        // Fresh login via credentials + 2FA
        loginWithCredentials(client, steamUser, username, password, authenticator)
    }

    private fun loginWithToken(steamUser: SteamUser, accountName: String, refreshToken: String) {
        val loggedOnLatch = CountDownLatch(1)
        var loginResult: EResult? = null

        callbackManager?.subscribe(LoggedOnCallback::class.java) { cb ->
            loginResult = cb.result
            loggedOnLatch.countDown()
        }

        val details = LogOnDetails().apply {
            this.username = accountName
            this.accessToken = refreshToken
            shouldRememberPassword = true
        }
        steamUser.logOn(details)

        if (!loggedOnLatch.await(LOGIN_TIMEOUT_SEC, TimeUnit.SECONDS)) {
            throw RuntimeException("Login timed out")
        }

        if (loginResult != EResult.OK) {
            Log.w(TAG, "Token login failed ($loginResult), clearing saved token")
            prefs.edit().remove(KEY_REFRESH_TOKEN).remove(KEY_ACCOUNT_NAME).apply()
            throw RuntimeException("Token login failed: $loginResult")
        }

        Log.i(TAG, "Logged in with saved token as $accountName")
    }

    private fun loginWithCredentials(
        client: SteamClient,
        steamUser: SteamUser,
        username: String,
        password: String,
        authenticator: IAuthenticator
    ) {
        val authDetails = AuthSessionDetails().apply {
            this.username = username
            this.password = password
            this.authenticator = authenticator
            this.persistentSession = true
        }

        // Begin auth session — this handles 2FA via the authenticator
        val authSession = client.authentication
            .beginAuthSessionViaCredentials(authDetails)
            .get(LOGIN_TIMEOUT_SEC, TimeUnit.SECONDS)

        // Poll for approval (2FA code entry, mobile confirm, etc.)
        val pollResult: AuthPollResult = authSession
            .pollingWaitForResult()
            .get(LOGIN_TIMEOUT_SEC * 2, TimeUnit.SECONDS)

        // Save refresh token for next time
        prefs.edit()
            .putString(KEY_REFRESH_TOKEN, pollResult.refreshToken)
            .putString(KEY_ACCOUNT_NAME, pollResult.accountName)
            .apply()
        Log.i(TAG, "Saved refresh token for ${pollResult.accountName}")

        // Now log on with the access token
        val loggedOnLatch = CountDownLatch(1)
        var loginResult: EResult? = null

        callbackManager?.subscribe(LoggedOnCallback::class.java) { cb ->
            loginResult = cb.result
            loggedOnLatch.countDown()
        }

        val details = LogOnDetails().apply {
            this.username = pollResult.accountName
            this.accessToken = pollResult.refreshToken
            shouldRememberPassword = true
        }
        steamUser.logOn(details)

        if (!loggedOnLatch.await(LOGIN_TIMEOUT_SEC, TimeUnit.SECONDS)) {
            throw RuntimeException("Login timed out after auth")
        }

        if (loginResult != EResult.OK) {
            throw RuntimeException("Login failed after auth: $loginResult")
        }

        Log.i(TAG, "Logged in as ${pollResult.accountName}")
    }

    // ---- License list ----

    private fun waitForLicenses() {
        val licenseLatch = CountDownLatch(1)

        callbackManager?.subscribe(LicenseListCallback::class.java) { cb ->
            if (cb.result == EResult.OK) {
                licenses = cb.licenseList
                Log.i(TAG, "Got ${cb.licenseList.size} licenses")
            } else {
                Log.e(TAG, "License list failed: ${cb.result}")
            }
            licenseLatch.countDown()
        }

        if (!licenseLatch.await(LICENSE_TIMEOUT_SEC, TimeUnit.SECONDS)) {
            throw RuntimeException("Timeout waiting for license list")
        }

        if (licenses == null) {
            throw RuntimeException("Failed to get license list")
        }
    }

    // ---- Download ----

    private fun download(appId: Int, installDir: String, steamappsDir: String) {
        val client = steamClient ?: throw IllegalStateException("Not connected")
        val licenseList = licenses ?: throw IllegalStateException("No licenses")

        val downloader = DepotDownloader(
            client,
            licenseList,
            /* androidEmulation = */ true
        )

        downloader.addListener(object : IDownloadListener {
            override fun onStatusUpdate(status: String) {
                notify(status)
            }

            override fun onChunkCompleted(depotId: Int, progress: Float, bytesDownloaded: Long, totalBytes: Long) {
                val pct = (progress * 100).toInt()
                val mbDown = bytesDownloaded / (1024 * 1024)
                val mbTotal = totalBytes / (1024 * 1024)
                notify("Depot $depotId: $pct% ($mbDown/$mbTotal MB)")
            }

            override fun onDepotCompleted(depotId: Int, bytesDownloaded: Long, totalBytes: Long) {
                notify("Depot $depotId complete (${totalBytes / (1024 * 1024)} MB)")
            }

            override fun onDownloadCompleted(item: DownloadItem) {
                Log.i(TAG, "Download completed for AppID ${item.appId}")
            }

            override fun onDownloadFailed(item: DownloadItem, error: Throwable) {
                Log.e(TAG, "Download failed for AppID ${item.appId}", error)
            }
        })

        // Configure download item — use named params like GameNative does
        val installPath = "$steamappsDir/common/$installDir"
        notify("Install path: $installPath")

        // 228980 depots must be specified explicitly — it's a "tool" app
        // whose depots aren't auto-discovered by the standard depot enumeration.
        // Both 228981 (main) and 228983 (additional redists) are needed — Steam
        // blocks game launches if 228983 is missing/incomplete.
        val depotIds = when (appId) {
            228980 -> listOf(228981, 228983)
            else -> emptyList()
        }
        val appItem = AppItem(
            appId = appId,
            installDirectory = installPath,
            depot = depotIds,
        )

        downloader.add(appItem)
        downloader.finishAdding()
        downloader.startDownloading()

        // Use DepotDownloader's built-in completion future with timeout
        notify("Waiting for download to complete...")
        try {
            downloader.getCompletion().get(30, TimeUnit.MINUTES)
        } catch (e: Exception) {
            val cause = e.cause ?: e
            throw RuntimeException("Download failed: ${cause.message}", cause)
        } finally {
            downloader.close()
        }
        notify("Download finished for AppID $appId")
    }

    // ---- App Manifest ----

    /**
     * Generate appmanifest_<appId>.acf so Steam recognizes the content as installed.
     * StateFlags=4 means "fully installed".
     */
    private fun generateAppManifest(appId: Int, installDir: String, steamappsDir: String) {
        val manifestFile = File(steamappsDir, "appmanifest_$appId.acf")

        // Calculate installed size
        val contentDir = File(steamappsDir, "common/$installDir")
        val sizeOnDisk = if (contentDir.exists()) {
            contentDir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
        } else 0L

        val manifest = buildString {
            appendLine("\"AppState\"")
            appendLine("{")
            appendLine("\t\"appid\"\t\t\"$appId\"")
            appendLine("\t\"Universe\"\t\t\"1\"")
            appendLine("\t\"name\"\t\t\"Steamworks Common Redistributables\"")
            appendLine("\t\"StateFlags\"\t\t\"4\"")
            appendLine("\t\"installdir\"\t\t\"$installDir\"")
            appendLine("\t\"SizeOnDisk\"\t\t\"$sizeOnDisk\"")
            appendLine("\t\"buildid\"\t\t\"0\"")
            appendLine("\t\"LastOwner\"\t\t\"0\"")
            appendLine("\t\"UpdateResult\"\t\t\"0\"")
            appendLine("\t\"BytesToDownload\"\t\t\"0\"")
            appendLine("\t\"BytesDownloaded\"\t\t\"0\"")
            appendLine("\t\"AutoUpdateBehavior\"\t\t\"1\"")
            appendLine("\t\"AllowOtherDownloadsWhileRunning\"\t\t\"1\"")
            appendLine("}")
        }

        manifestFile.writeText(manifest)
        Log.i(TAG, "Wrote ${manifestFile.absolutePath} (${sizeOnDisk} bytes on disk)")
    }

    private fun notify(msg: String) {
        Log.i(TAG, msg)
        onProgress?.invoke(msg)
    }
}
