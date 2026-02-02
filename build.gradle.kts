// Top-level build file for MediaTek Steam Gaming App
plugins {
    id("com.android.application") version "8.4.0" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}

tasks.register("clean", Delete::class) {
    delete(rootProject.layout.buildDirectory)
}
