// JNI bridge（Kotlin 側: com.example.localvoiceagent.LocalAudioEngine）。
//
// Chromium の Android ビルド既定は version script で JNI_OnLoad 以外の
// シンボルを全て隠すため、JNI_OnLoad + RegisterNatives で登録する。
//
// audio callback path から呼ばれるため、process 系は JNI 呼び出しごとの
// heap alloc なし（DirectByteBuffer のアドレス取得のみ。開発計画 §12）。
#include <jni.h>

#include <cstdint>

#include "local_audio/local_audio_processor.h"

namespace {

constexpr char kVersion[] = "local_audio_engine 0.2 (webrtc branch-heads/7300)";
constexpr char kClassPath[] = "com/example/localvoiceagent/LocalAudioEngine";
constexpr size_t kFrameSamples = 480;  // 10ms @ 48kHz mono

local_audio::LocalAudioProcessor* FromHandle(jlong h) {
  return reinterpret_cast<local_audio::LocalAudioProcessor*>(h);
}

jstring GetVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF(kVersion);
}

// 成功: processor ハンドル / 失敗: 0
jlong Create(JNIEnv*, jclass, jboolean aec, jboolean ns, jboolean agc) {
  auto* p = new local_audio::LocalAudioProcessor();
  local_audio::AudioProcessorConfig cfg;
  cfg.enable_aec = aec;
  cfg.enable_ns = ns;
  cfg.enable_agc = agc;
  if (!p->Initialize(cfg)) {
    delete p;
    return 0;
  }
  return reinterpret_cast<jlong>(p);
}

void Destroy(JNIEnv*, jclass, jlong handle) {
  delete FromHandle(handle);
}

// input/output: 480 samples の int16 を格納した DirectByteBuffer（native order）。
// in-place 可（同一 buffer 可）。戻り値 0 = 成功。
jint ProcessCapture(JNIEnv* env, jclass, jlong handle, jobject input,
                    jobject output) {
  auto* in = static_cast<int16_t*>(env->GetDirectBufferAddress(input));
  auto* out = static_cast<int16_t*>(env->GetDirectBufferAddress(output));
  if (handle == 0 || in == nullptr || out == nullptr) {
    return -2;
  }
  return FromHandle(handle)->ProcessCapture(in, kFrameSamples, out);
}

jint ProcessRender(JNIEnv* env, jclass, jlong handle, jobject input) {
  auto* in = static_cast<int16_t*>(env->GetDirectBufferAddress(input));
  if (handle == 0 || in == nullptr) {
    return -2;
  }
  return FromHandle(handle)->ProcessRender(in, kFrameSamples);
}

void SetStreamDelayMs(JNIEnv*, jclass, jlong handle, jint delay_ms) {
  if (handle != 0) {
    FromHandle(handle)->SetStreamDelayMs(delay_ms);
  }
}

void Reset(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) {
    FromHandle(handle)->Reset();
  }
}

const JNINativeMethod kMethods[] = {
    {"nativeGetVersion", "()Ljava/lang/String;",
     reinterpret_cast<void*>(GetVersion)},
    {"nativeCreate", "(ZZZ)J", reinterpret_cast<void*>(Create)},
    {"nativeDestroy", "(J)V", reinterpret_cast<void*>(Destroy)},
    {"nativeProcessCapture",
     "(JLjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)I",
     reinterpret_cast<void*>(ProcessCapture)},
    {"nativeProcessRender", "(JLjava/nio/ByteBuffer;)I",
     reinterpret_cast<void*>(ProcessRender)},
    {"nativeSetStreamDelayMs", "(JI)V",
     reinterpret_cast<void*>(SetStreamDelayMs)},
    {"nativeReset", "(J)V", reinterpret_cast<void*>(Reset)},
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
