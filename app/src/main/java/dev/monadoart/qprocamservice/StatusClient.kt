package dev.monadoart.qprocamservice

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.URL

data class StreamStat(
    val index: Int,
    val clients: Int,
    val outputFps: Double,
    val meanJpegBytes: Double,
    val meanEncodeMs: Double
)

data class ModelStat(
    val enabled: Boolean,
    val loaded: Boolean,
    val error: String?,
    val subscribers: Int,
    val results: Long,
    val meanTotalMs: Double,
    val meanHtaMs: Double
)

data class DaemonStatus(
    val version: String?,
    val sourceState: String,
    val lastFrameAgeMs: Double?,
    val uptimeS: Double,
    val cpuPercent: Double?,
    val readFps: Double?,
    val meanFrameAgeMs: Double?,
    val streams: List<StreamStat>,
    val tongue: ModelStat?,
    val pupil: ModelStat?,
    val raw: String
)

object StatusClient {
    suspend fun fetch(port: Int = 27280, timeoutMs: Int = 800): DaemonStatus? = withContext(Dispatchers.IO) {
        var connection: HttpURLConnection? = null
        try {
            connection = URL("http://127.0.0.1:$port/status").openConnection() as HttpURLConnection
            connection.connectTimeout = timeoutMs
            connection.readTimeout = timeoutMs
            connection.requestMethod = "GET"
            if (connection.responseCode != HttpURLConnection.HTTP_OK) return@withContext null

            val body = connection.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val root = JSONObject(body)
            val source = root.optJSONObject("source")
            val process = root.optJSONObject("process")
            val input = root.optJSONObject("input")
            val clients = root.optJSONArray("clients")
            val streamData = root.optJSONArray("streams")

            fun nullableDouble(objectValue: JSONObject?, key: String): Double? {
                if (objectValue == null) return null
                val value = objectValue.optDouble(key, Double.NaN)
                return value.takeUnless { it.isNaN() }
            }

            val streamCount = maxOf(clients?.length() ?: 0, streamData?.length() ?: 0)
            val streamStats = (0 until streamCount).map { index ->
                val clientCount = clients?.optInt(index, 0) ?: 0
                val stream = streamData?.optJSONObject(index)
                StreamStat(
                    index = index,
                    clients = clientCount,
                    outputFps = nullableDouble(stream, "output_frames_per_second") ?: 0.0,
                    meanJpegBytes = nullableDouble(stream, "mean_jpeg_bytes") ?: 0.0,
                    meanEncodeMs = nullableDouble(stream, "mean_encode_ms") ?: 0.0
                )
            }

            // Onboard HTA models; absent in daemons older than 0.5.0.
            fun model(key: String): ModelStat? = root.optJSONObject(key)?.let {
                ModelStat(
                    enabled = it.optBoolean("enabled", true),
                    loaded = it.optBoolean("loaded"),
                    error = it.optString("error").takeUnless { e -> e == "" || e == "null" },
                    subscribers = it.optInt("subscribers"),
                    results = it.optLong("results"),
                    meanTotalMs = it.optDouble("mean_total_ms", 0.0),
                    meanHtaMs = it.optDouble("mean_hta_ms", 0.0)
                )
            }

            DaemonStatus(
                version = root.optString("version").takeUnless { it == "" || it == "null" },
                sourceState = source?.optString("state")?.takeUnless { it == "" || it == "null" } ?: "unknown",
                lastFrameAgeMs = nullableDouble(source, "last_frame_age_ms"),
                uptimeS = nullableDouble(root, "uptime") ?: 0.0,
                cpuPercent = nullableDouble(process, "cpu_percent"),
                readFps = nullableDouble(input, "frames_read_per_second"),
                meanFrameAgeMs = nullableDouble(input, "mean_frame_age_ms"),
                streams = streamStats,
                tongue = model("tongue"),
                pupil = model("pupil"),
                raw = body
            )
        } catch (_: Exception) {
            null
        } finally {
            connection?.disconnect()
        }
    }
}

object ModelControl {
    /** Starts or stops one onboard model in the running daemon (localhost-only /control). */
    suspend fun set(model: String, enabled: Boolean, port: Int = 27280): Boolean = withContext(Dispatchers.IO) {
        var connection: HttpURLConnection? = null
        try {
            val value = if (enabled) 1 else 0
            connection = URL("http://127.0.0.1:$port/control?$model=$value").openConnection() as HttpURLConnection
            connection.connectTimeout = 800
            connection.readTimeout = 800
            connection.responseCode == HttpURLConnection.HTTP_OK
        } catch (_: Exception) {
            false
        } finally {
            connection?.disconnect()
        }
    }
}

object NetInfo {
    fun lanIpv4(): String? {
        return try {
            val interfaces = NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
                .filter { it.isUp && !it.isLoopback }
            val wlanAddress = interfaces.firstOrNull { it.name == "wlan0" }
                ?.inetAddresses?.toList()
                ?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
            wlanAddress?.hostAddress ?: interfaces
                .asSequence()
                .flatMap { it.inetAddresses.toList().asSequence() }
                .filterIsInstance<Inet4Address>()
                .firstOrNull { it.isSiteLocalAddress && !it.isLoopbackAddress }
                ?.hostAddress
        } catch (_: Exception) {
            null
        }
    }
}
