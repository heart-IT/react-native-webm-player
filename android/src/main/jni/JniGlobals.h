#pragma once

#include <jni.h>

// Global JVM pointer, set in JNI_OnLoad (OnLoad.cpp).
// Used by audiosession_jni.cpp for JNI calls from native threads.
extern JavaVM* g_jvm;
