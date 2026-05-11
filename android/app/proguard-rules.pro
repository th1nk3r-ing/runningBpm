# Add project specific ProGuard rules here.
# Keep native methods (for JNI - Phase 2)
-keepclasseswithmembernames class * {
    native <methods>;
}
