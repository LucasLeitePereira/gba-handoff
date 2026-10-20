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
        versionCode = 1
        versionName = "0.1"

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

    buildTypes {
        release {
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
