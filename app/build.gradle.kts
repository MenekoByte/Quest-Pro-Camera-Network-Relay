import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Release signing: keystore.properties in the repo root (git-ignored) with
// storeFile, storePassword, keyAlias, keyPassword. Optional.
val keystoreProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.isFile) file.inputStream().use { stream -> load(stream) }
}

val stageQproNativeLibraries by tasks.registering(Copy::class) {
    val nativeBuildDir = rootProject.file("native/build/android")
    val daemon = nativeBuildDir.resolve("qpro-camd")
    val injector = nativeBuildDir.resolve("questpro-camera-injector")
    val streamer = nativeBuildDir.resolve("libquestpro-camera-streamer-v12.so")
    doFirst {
        val missing = listOf(daemon, injector, streamer).filterNot { it.isFile }
        check(missing.isEmpty()) {
            "Build the native binaries first: native/build.ps1 (missing: ${missing.joinToString { it.name }})"
        }
    }
    from(daemon) { rename { "libqprocamd.so" } }
    from(injector) { rename { "libqpinjector.so" } }
    from(streamer)
    into(layout.buildDirectory.dir("generated/qprocam-jniLibs/arm64-v8a"))
}

android {
    namespace = "dev.monadoart.qprocamservice"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "dev.monadoart.qprocamservice"
        minSdk = 34
        targetSdk = 37
        versionCode = 10
        versionName = "0.6.3"
        ndk { abiFilters += "arm64-v8a" }
    }

    signingConfigs {
        if (keystoreProperties.containsKey("storeFile")) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            // Without keystore.properties, release builds are signed with the local debug key.
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
    // AGP rejects a Provider here; preBuild depends on the copy task.
    sourceSets.getByName("main").jniLibs.srcDir(layout.buildDirectory.dir("generated/qprocam-jniLibs").get().asFile)
    packaging { jniLibs { useLegacyPackaging = true } }
}

tasks.named("preBuild").configure { dependsOn(stageQproNativeLibraries) }

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.runtime)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.kotlinx.coroutines.android)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
