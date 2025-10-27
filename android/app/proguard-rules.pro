# Add project specific ProGuard rules here.
# By default, the flags in this file are appended to flags specified
# in ${sdk.dir}/tools/proguard/proguard-android.txt

# Keep SDL classes
-keep class org.libsdl.app.** { *; }

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}
