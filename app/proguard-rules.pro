# Add project specific ProGuard rules here.
# By default, the flags in this file are appended to flags specified
# in /sdk/tools/proguard/proguard-android.txt

# Steam Launcher ProGuard Rules

# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep X11Server and its inner classes
-keep class com.mediatek.steamlauncher.X11Server { *; }
-keep class com.mediatek.steamlauncher.X11Server$* { *; }

# Keep LorieView
-keep class com.mediatek.steamlauncher.LorieView { *; }

# Keep service classes
-keep class com.mediatek.steamlauncher.SteamService { *; }

# OkHttp
-dontwarn okhttp3.**
-dontwarn okio.**
-keep class okhttp3.** { *; }
-keep interface okhttp3.** { *; }

# Apache Commons Compress
-keep class org.apache.commons.compress.** { *; }
-dontwarn org.apache.commons.compress.**

# Keep BuildConfig
-keep class com.mediatek.steamlauncher.BuildConfig { *; }
