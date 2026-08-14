// JNI bridge（Kotlin 側: com.example.localvoiceagent.LocalAudioEngine）。
//
// Chromium の Android ビルド既定は version script で JNI_OnLoad 以外の
// シンボルを全て隠すため、Java_* の命名規約エクスポートは使えない。
// JNI_OnLoad + RegisterNatives で登録する（この build tree の流儀）。
//
// Issue #5: hello-world（バージョン文字列のみ）。APM ラップは Issue #10/#12 で追加。
#include <jni.h>

namespace {

constexpr char kVersion[] = "local_audio_engine 0.1 (webrtc branch-heads/7300)";
constexpr char kClassPath[] = "com/example/localvoiceagent/LocalAudioEngine";

jstring GetVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF(kVersion);
}

const JNINativeMethod kMethods[] = {
    {"nativeGetVersion", "()Ljava/lang/String;",
     reinterpret_cast<void*>(GetVersion)},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return -1;
  }
  jclass cls = env->FindClass(kClassPath);
  if (cls == nullptr) {
    return -1;
  }
  if (env->RegisterNatives(cls, kMethods,
                           sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
    return -1;
  }
  return JNI_VERSION_1_6;
}
