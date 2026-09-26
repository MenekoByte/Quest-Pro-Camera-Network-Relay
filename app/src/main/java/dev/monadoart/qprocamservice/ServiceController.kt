package dev.monadoart.qprocamservice

class ServiceController(private val nativeLibDir: String, private val port: Int = 27280) {
    data class ActionResult(val ok: Boolean, val log: String)

    suspend fun start(tongueEnabled: Boolean = true, pupilEnabled: Boolean = true): ActionResult {
        val nativeDir = shellQuote(nativeLibDir)
        val modelFlags = listOfNotNull(
            "--disable-tongue".takeUnless { tongueEnabled },
            "--disable-pupil".takeUnless { pupilEnabled }
        ).joinToString(" ")
        val script = """
            echo 'STEP stage'
            if ! cmp -s $nativeDir/libquestpro-camera-streamer-v12.so /data/local/tmp/libquestpro-camera-streamer-v12.so; then
                cp $nativeDir/libquestpro-camera-streamer-v12.so /data/local/tmp/libquestpro-camera-streamer-v12.so || exit $?
                chmod 644 /data/local/tmp/libquestpro-camera-streamer-v12.so || exit $?
            fi
            # The daemon runs from /data/local/tmp: started from the app's lib
            # directory, the linker namespace blocks it from loading
            # /vendor/lib64/libhta_hexagon_runtime.so for onboard inference.
            if ! cmp -s $nativeDir/libqprocamd.so /data/local/tmp/qpro-camd; then
                cp $nativeDir/libqprocamd.so /data/local/tmp/qpro-camd || exit $?
                chmod 755 /data/local/tmp/qpro-camd || exit $?
            fi
            echo 'STEP log'
            if [ ! -e /data/local/tmp/questpro-live-v12.log ]; then
                touch /data/local/tmp/questpro-live-v12.log || exit $?
            fi
            chmod 666 /data/local/tmp/questpro-live-v12.log 2>/dev/null || true
            echo 'STEP inject'
            inject_output=$( $nativeDir/libqpinjector.so /data/local/tmp/libquestpro-camera-streamer-v12.so 2>&1 )
            inject_status=$?
            printf '%s\n' "${'$'}inject_output"
            if [ "${'$'}inject_status" -ne 0 ]; then exit "${'$'}inject_status"; fi
            case "${'$'}inject_output" in
                *INJECTION_OK*|*INJECTION_ALREADY_ACTIVE*) ;;
                *) exit 1 ;;
            esac
            echo 'STEP daemon'
            /data/local/tmp/qpro-camd --daemonize --port $port $modelFlags
        """.trimIndent()

        val result = RootShell.run(script)
        val combined = listOf(result.stdout, result.stderr).filter { it.isNotBlank() }.joinToString("\n").trim()
        val injectorAccepted = combined.contains("INJECTION_OK") || combined.contains("INJECTION_ALREADY_ACTIVE")
        return ActionResult(result.exitCode == 0 && injectorAccepted, combined)
    }

    suspend fun stop(): ActionResult {
        // Always stop with the APK's own binary: it is the current version, and its
        // --stop (pid file + cmdline basename check) accepts both "libqprocamd.so"
        // and the staged "/data/local/tmp/qpro-camd". A stale staged copy from an
        // older tool may not recognise the running daemon.
        val result = RootShell.run("${shellQuote(nativeLibDir)}/libqprocamd.so --stop")
        val combined = listOf(result.stdout, result.stderr).filter { it.isNotBlank() }.joinToString("\n").trim()
        return ActionResult(result.exitCode == 0, combined)
    }

    suspend fun hasRoot(): Boolean = RootShell.run("id").stdout.contains("uid=0")

    private fun shellQuote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
