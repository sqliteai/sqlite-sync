#include <stdio.h>
#include <jni.h>
#include "android_https_call.h"

int make_android_https_call(void) {
    JavaVM *vms[1];
    jsize num_vms;
    JNIEnv *env = NULL;
    jint attached_here = 0;

    JNI_GetCreatedJavaVMs(vms, 1, &num_vms);

    if (num_vms == 0) {
        printf("No Java VM available\n");
        return 0;
    }

    JavaVM *jvm = vms[0];

    // Try to get the environment
    if ((*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        // Not attached, attach it now
        if ((*jvm)->AttachCurrentThread(jvm, (void**)&env, NULL) != JNI_OK) {
            printf("Failed to attach current thread to JVM\n");
            return 0;
        }
        attached_here = 1;
    }
    // // Locate the HttpsCaller class
    jclass cls = (*env)->FindClass(env, "com/example/testcloudsync/HttpsCaller");
    if (cls == NULL) {
        printf("Could not find HttpsCaller class\n");
        if (attached_here) (*jvm)->DetachCurrentThread(jvm);
        return 0;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "callHttps", "()Ljava/lang/String;");
    if (mid == NULL) {
        printf("Could not find method callHttps\n");
        if (attached_here) (*jvm)->DetachCurrentThread(jvm);
        return 0;
    }

    jstring result = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid);
    if (result != NULL) {
        const char *str = (*env)->GetStringUTFChars(env, result, NULL);
        printf("HTTPS result: %s\n", str);
        (*env)->ReleaseStringUTFChars(env, result, str);
    }

    // Detach thread ONLY if we attached it
    if (attached_here) {
        (*jvm)->DetachCurrentThread(jvm);
    }

    return (int)num_vms;
}