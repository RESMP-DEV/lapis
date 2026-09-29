// Root build file: declares the plugins once so :app can apply them by alias.
// AGP 9 provides Kotlin compilation itself (built-in Kotlin); only its
// compiler plugins (compose, serialization) are declared here.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
    alias(libs.plugins.kotlin.serialization) apply false
}
