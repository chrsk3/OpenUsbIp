#include <jni.h>
#include <android/log.h>

#define LOG_TAG "OpenUsbIp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

JNIEXPORT jstring JNICALL
Java_com_openusbip_MainActivity_startUsbIpServer(JNIEnv *env, jobject thiz, jint fd, jint port) {
    LOGI("Servidor USB/IP inicializado com FD=%d na porta=%d", fd, port);
    return (*env)->NewStringUTF(env, "NDK pronto (aguardando protocolo USB/IP)");
}
