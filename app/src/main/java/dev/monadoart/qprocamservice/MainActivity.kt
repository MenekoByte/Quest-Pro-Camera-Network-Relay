package dev.monadoart.qprocamservice

import android.content.Context
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import dev.monadoart.qprocamservice.ui.theme.QuestProCameraServiceTheme
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            QuestProCameraServiceTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    CameraServiceScreen(
                        nativeLibDir = applicationInfo.nativeLibraryDir,
                        appVersion = packageManager.getPackageInfo(packageName, 0).versionName ?: "unknown"
                    )
                }
            }
        }
    }
}

@Composable
private fun CameraServiceScreen(nativeLibDir: String, appVersion: String) {
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    var status by remember { mutableStateOf<DaemonStatus?>(null) }
    var rootGranted by remember { mutableStateOf<Boolean?>(null) }
    var actionInProgress by remember { mutableStateOf(false) }
    var actionLog by remember { mutableStateOf("") }
    var lanIp by remember { mutableStateOf<String?>(null) }
    // Per-model Start/Stop, remembered across app and daemon restarts.
    val prefs = LocalContext.current.getSharedPreferences("models", Context.MODE_PRIVATE)
    var tongueEnabled by remember { mutableStateOf(prefs.getBoolean("tongue", true)) }
    var pupilEnabled by remember { mutableStateOf(prefs.getBoolean("pupil", true)) }

    LaunchedEffect(lifecycleOwner) {
        lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            while (true) {
                status = StatusClient.fetch()
                lanIp = NetInfo.lanIpv4()
                kotlinx.coroutines.delay(1_000)
            }
        }
    }
    LaunchedEffect(nativeLibDir) {
        rootGranted = ServiceController(nativeLibDir).hasRoot()
    }

    val controller = remember(nativeLibDir) { ServiceController(nativeLibDir) }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("Quest Pro Camera Service", style = MaterialTheme.typography.headlineSmall)
        Text("App version: $appVersion", style = MaterialTheme.typography.bodyMedium)

        // Running means the daemon answers /status.
        val running = status != null
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Button(
                enabled = !actionInProgress,
                colors = if (running) ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error,
                    contentColor = MaterialTheme.colorScheme.onError
                ) else ButtonDefaults.buttonColors(),
                onClick = {
                    scope.launch {
                        actionInProgress = true
                        try {
                            val result = if (running) controller.stop() else controller.start(tongueEnabled, pupilEnabled)
                            actionLog = result.log.takeLast(4000)
                            // Refresh now instead of waiting for the next poll.
                            status = StatusClient.fetch()
                        } finally {
                            actionInProgress = false
                        }
                    }
                }
            ) {
                Text(
                    when {
                        actionInProgress && running -> "Stopping..."
                        actionInProgress -> "Starting..."
                        running -> "Stop"
                        else -> "Start"
                    }
                )
            }
            Box(
                Modifier
                    .size(12.dp)
                    .background(if (running) Color(0xFF2E7D32) else MaterialTheme.colorScheme.outline, CircleShape)
            )
            Text(if (running) "Running" else "Stopped", style = MaterialTheme.typography.titleMedium)
        }
        if (actionInProgress) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        if (!running) {
            Text("Start briefly freezes the headset display while it attaches to the camera service.", style = MaterialTheme.typography.bodySmall)
        }
        if (rootGranted == false) {
            Text("Root not granted - allow this app in Magisk > Superuser", color = MaterialTheme.colorScheme.error)
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("Status", style = MaterialTheme.typography.titleMedium)
                Text("Service: ${if (status != null) "Running" else "Stopped"}")
                status?.let { current ->
                    Text("Cameras: ${cameraStateText(current.sourceState)}")
                    current.streams.filter { it.clients > 0 }.forEach { stream ->
                        Text("${streamName(stream.index)}: ${format(stream.outputFps)} fps, ${format(stream.meanJpegBytes / 1024.0)} KB/frame")
                    }
                    Text("Daemon CPU: ${current.cpuPercent?.let { "${format(it)}%" } ?: "-"}")
                    Text("Frame age: ${current.lastFrameAgeMs?.let { "${format(it)} ms" } ?: "-"}")
                    Text("Daemon version: ${current.version ?: "unknown"}")
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Onboard models", style = MaterialTheme.typography.titleMedium)
                listOf(
                    Triple("tongue", "Tongue", tongueEnabled),
                    Triple("pupil", "Pupil", pupilEnabled)
                ).forEach { (key, label, enabled) ->
                    val stat = if (key == "tongue") status?.tongue else status?.pupil
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Button(
                            colors = if (enabled) ButtonDefaults.buttonColors(
                                containerColor = MaterialTheme.colorScheme.error,
                                contentColor = MaterialTheme.colorScheme.onError
                            ) else ButtonDefaults.buttonColors(),
                            onClick = {
                                val next = !enabled
                                prefs.edit().putBoolean(key, next).apply()
                                if (key == "tongue") tongueEnabled = next else pupilEnabled = next
                                // Applies now if the service runs; otherwise at the next Start.
                                scope.launch {
                                    if (running) ModelControl.set(key, next)
                                    status = StatusClient.fetch()
                                }
                            }
                        ) { Text(if (enabled) "Stop" else "Start") }
                        Text("$label: ${if (!enabled) "stopped" else stat?.let { modelText(it) } ?: "starts with the service"}")
                    }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("Stream URLs", style = MaterialTheme.typography.titleMedium)
                if (lanIp == null) {
                    Text("Not connected to Wi-Fi")
                } else {
                    Text("http://$lanIp:27280/camera2.mjpg", fontFamily = FontFamily.Monospace)
                    Text("Tongue output (onboard, NDJSON): http://$lanIp:27280/tongue", fontFamily = FontFamily.Monospace)
                    Text("Pupil output (onboard, NDJSON): http://$lanIp:27280/pupil", fontFamily = FontFamily.Monospace)
                    Text("Other endpoints:", style = MaterialTheme.typography.titleSmall)
                    listOf(
                        "/camera0.mjpg", "/camera1.mjpg", "/camera3.mjpg", "/camera4.mjpg",
                        "/strip.mjpg", "/mouth.mjpg", "/face.mjpg", "/eyes.mjpg",
                        "/camera0.jpg", "/camera1.jpg", "/camera2.jpg",
                        "/camera3.jpg", "/camera4.jpg"
                    ).forEach { Text("http://$lanIp:27280$it", fontFamily = FontFamily.Monospace) }
                }
            }
        }

        if (actionLog.isNotEmpty()) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Text(
                    actionLog,
                    modifier = Modifier.padding(12.dp),
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
        Spacer(Modifier.height(4.dp))
    }
}

private fun cameraStateText(state: String): String = when (state) {
    "ok" -> "Streaming"
    "idle" -> "Ready (no viewers)"
    "waiting_for_frames" -> "Waiting for camera frames - is face tracking active?"
    "test_pattern" -> "Test pattern"
    else -> state
}

private fun streamName(index: Int): String = when (index) {
    0 -> "Left eye"
    1 -> "Right eye"
    2 -> "Left mouth"
    3 -> "Right mouth"
    4 -> "Brow"
    5 -> "Strip"
    6 -> "Eyes pair"
    7 -> "Face (3 cams)"
    8 -> "Mouth pair"
    else -> "Stream $index"
}

private fun format(value: Double): String = String.format(java.util.Locale.US, "%.1f", value)

private fun modelText(model: ModelStat): String = when {
    !model.loaded -> "not loaded${model.error?.let { " ($it)" } ?: ""}"
    model.subscribers == 0 -> "loaded, idle (no client connected)"
    else -> "running for ${model.subscribers} client(s), ${format(model.meanTotalMs)} ms per result (${model.results} results)"
}
