#include <stdio.h>
#include <jni.h>
#include "android_https_call.h"

void make_android_https_call(void) {
    JavaVM *vms[1];
    jsize num_vms;
    JNIEnv *env = NULL;

    // Get the Java VM (assumes one has been created)
    if (JNI_GetCreatedJavaVMs(vms, 1, &num_vms) != JNI_OK || num_vms == 0) {
        printf("No Java VM available\n");
        return;
    }

    JavaVM *jvm = vms[0];

    // Attach the current thread to the JVM
    if ((*jvm)->AttachCurrentThread(jvm, (void**)&env, NULL) != JNI_OK) {
        printf("Failed to attach current thread to JVM\n");
        return;
    }

    // Locate the HttpsCaller class
    jclass cls = (*env)->FindClass(env, "HttpsCaller");
    if (cls == NULL) {
        printf("Could not find HttpsCaller class\n");
        (*jvm)->DetachCurrentThread(jvm);
        return;
    }

    // Find the static method ID for `callHttps`
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "callHttps", "()Ljava/lang/String;");
    if (mid == NULL) {
        printf("Could not find method callHttps\n");
        (*jvm)->DetachCurrentThread(jvm);
        return;
    }

    // Call the static method
    jstring result = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid);

    // Convert Java string to C string
    const char *str = (*env)->GetStringUTFChars(env, result, NULL);
    printf("HTTPS result: %s\n", str);
    (*env)->ReleaseStringUTFChars(env, result, str);

    // Detach thread from JVM
    (*jvm)->DetachCurrentThread(jvm);
}