package dev.monadoart.qprocamservice

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

data class AppRelease(val version: String, val apkUrl: String)

object UpdateClient {
    private const val RELEASE_URL =
        "https://api.github.com/repos/MonadoArt/Quest-Pro-Camera-Network-Relay/releases/latest"
    private const val APK_NAME = "QuestProCameraService.apk"

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
            AppRelease(version, asset.getString("browser_download_url"))
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
            connection.inputStream.use { input ->
                apk.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            apk
        } catch (error: Exception) {
            apk.delete()
            throw error
        } finally {
            connection.disconnect()
        }
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
