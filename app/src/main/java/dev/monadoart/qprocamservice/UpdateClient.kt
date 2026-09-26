package dev.monadoart.qprocamservice

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

data class AppRelease(val version: String, val apkUrl: String, val digest: String?)

object UpdateClient {
    private const val RELEASE_URL =
        "https://api.github.com/repos/MonadoArt/Quest-Pro-Camera-Network-Relay/releases/latest"
    private const val APK_NAME = "QuestProCameraService.apk"
    private const val MAX_APK_BYTES = 150L * 1024 * 1024

    suspend fun check(currentVersion: String): AppRelease? = withContext(Dispatchers.IO) {
        val connection = (URL(RELEASE_URL).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10_000
            readTimeout = 10_000
            setRequestProperty("Accept", "application/vnd.github+json")
        }
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("GitHub returned HTTP ${connection.responseCode}")
            }
            val release = JSONObject(connection.inputStream.bufferedReader().use { it.readText() })
            val version = release.getString("tag_name").removePrefix("v")
            if (compareVersions(version, currentVersion) <= 0) return@withContext null
            val assets = release.getJSONArray("assets")
            val asset = (0 until assets.length()).asSequence()
                .map { assets.getJSONObject(it) }
                .firstOrNull { it.optString("name") == APK_NAME }
                ?: throw IOException("The latest release has no $APK_NAME asset")
            val url = asset.getString("browser_download_url")
            if (!url.startsWith("https://github.com/MonadoArt/Quest-Pro-Camera-Network-Relay/releases/download/")) {
                throw IOException("Unexpected APK download address")
            }
            AppRelease(version, url, asset.optString("digest").takeIf { it.startsWith("sha256:") })
        } finally {
            connection.disconnect()
        }
    }

    suspend fun download(context: Context, release: AppRelease): File = withContext(Dispatchers.IO) {
        val connection = (URL(release.apkUrl).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10_000
            readTimeout = 30_000
        }
        val directory = File(context.cacheDir, "updates").apply { mkdirs() }
        val apk = File(directory, APK_NAME)
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("APK download returned HTTP ${connection.responseCode}")
            }
            if (connection.contentLengthLong > MAX_APK_BYTES) throw IOException("APK is too large")
            val hash = MessageDigest.getInstance("SHA-256")
            var count = 0L
            connection.inputStream.use { input ->
                apk.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        count += read
                        if (count > MAX_APK_BYTES) throw IOException("APK is too large")
                        hash.update(buffer, 0, read)
                        output.write(buffer, 0, read)
                    }
                }
            }
            if (count == 0L) throw IOException("Downloaded APK is empty")
            val actualDigest = hash.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
            if (release.digest != null && !release.digest.removePrefix("sha256:").equals(actualDigest, true)) {
                throw IOException("APK checksum does not match the release")
            }
            val packageInfo = context.packageManager.getPackageArchiveInfo(apk.absolutePath, 0)
                ?: throw IOException("Downloaded file is not an APK")
            if (packageInfo.packageName != context.packageName) throw IOException("APK package name does not match")
            if (packageInfo.versionName != release.version) throw IOException("APK version does not match the release")
            val installed = context.packageManager.getPackageInfo(context.packageName, 0)
            if (packageInfo.longVersionCode <= installed.longVersionCode) {
                throw IOException("APK version code is not newer than the installed app")
            }
            apk
        } catch (error: Exception) {
            apk.delete()
            throw error
        } finally {
            connection.disconnect()
        }
    }

    fun canInstall(context: Context): Boolean = context.packageManager.canRequestPackageInstalls()

    fun openInstallPermission(context: Context) {
        context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES).apply {
            data = Uri.parse("package:${context.packageName}")
        })
    }

    fun openInstaller(context: Context, apk: File) {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.updates", apk)
        context.startActivity(Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        })
    }

    private fun compareVersions(a: String, b: String): Int {
        val pattern = Regex("\\d+(?:\\.\\d+)*")
        if (!pattern.matches(a) || !pattern.matches(b)) throw IOException("Cannot compare release versions")
        val left = a.split('.').map(String::toLong)
        val right = b.split('.').map(String::toLong)
        for (index in 0 until maxOf(left.size, right.size)) {
            val result = (left.getOrElse(index) { 0L }).compareTo(right.getOrElse(index) { 0L })
            if (result != 0) return result
        }
        return 0
    }
}
