import java.util.Properties

// Version: single source of truth is /workspace/VERSION (code=N, name=X.Y), shared with the desktop build.
val versionProps = Properties().also { props ->
    val f = rootProject.projectDir.parentFile.resolve("VERSION")
    if (!f.isFile) throw GradleException("Arquivo VERSION não encontrado em ${f.absolutePath} (esperado: code=N e name=X.Y)")
    f.inputStream().use { props.load(it) }
}
fun versionKey(k: String): String = versionProps.getProperty(k)?.trim()?.takeIf { it.isNotEmpty() }
    ?: throw GradleException("VERSION sem a chave '$k' (esperado: code=N e name=X.Y)")

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.gbahandoff"
    compileSdk = 36
    ndkVersion = "27.3.13750724"
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "io.gbahandoff"
        minSdk = 26
        targetSdk = 36
        versionCode = versionKey("code").toIntOrNull() ?: throw GradleException("VERSION: 'code' deve ser inteiro")
        versionName = versionKey("name")

        ndk {
            // x86_64 only for the emulator; real devices are arm.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=none",
                    // NDK r27 does not 16 KB-align ELF segments by default (r28 does).
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.5"
        }
    }

    // Same key for debug and release: `adb install -r` over the installed app keeps the save only if the
    // certificate is identical. Default = the key that signed the app already on the user's phone.
    signingConfigs {
        create("handoff") {
            storeFile = file(System.getenv("GBAH_KEYSTORE") ?: "${System.getProperty("user.home")}/.android/debug.keystore")
            storePassword = System.getenv("GBAH_KEYSTORE_PASS") ?: "android"
            keyAlias = System.getenv("GBAH_KEY_ALIAS") ?: "androiddebugkey"
            keyPassword = System.getenv("GBAH_KEY_PASS") ?: "android"
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("handoff")
        }
        release {
            signingConfig = signingConfigs.getByName("handoff")
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
