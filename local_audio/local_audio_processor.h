// WebRTC APM/AEC3 の境界クラス。このヘッダに WebRTC の型を一切露出させない
// （開発計画 §4。JNI/Kotlin へ WebRTC API 変更を伝播させないため）。
// フォーマットは 48kHz / mono / int16 / 10ms (480 samples) 固定（§3）。
#ifndef LOCAL_AUDIO_LOCAL_AUDIO_PROCESSOR_H_
#define LOCAL_AUDIO_LOCAL_AUDIO_PROCESSOR_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace local_audio {

struct AudioProcessorConfig {
  int sample_rate_hz = 48000;  // 48000 のみ受理
  int channels = 1;            // 1 のみ受理
  bool enable_aec = true;      // AEC3 (mobile_mode=false)
  bool enable_ns = true;       // Noise Suppression (high)
  bool enable_agc = true;      // AGC2 digital（AGC1 は削除予定のため不使用）
};

class LocalAudioProcessor {
 public:
  LocalAudioProcessor();
  ~LocalAudioProcessor();

  LocalAudioProcessor(const LocalAudioProcessor&) = delete;
  LocalAudioProcessor& operator=(const LocalAudioProcessor&) = delete;

  // config が固定フォーマットを満たさない場合は false。再呼び出しで再構築。
  bool Initialize(const AudioProcessorConfig& config);

  // capture 10ms frame を処理して output へ（in-place 可）。
  // samples != 480 または未初期化はエラー。戻り値 0 = 成功。
  // stream setter 群と ProcessStream の並行呼び出し禁止（capture thread 専用）。
  int ProcessCapture(const int16_t* input, size_t samples, int16_t* output);

  // render(再生) 10ms frame を AEC 参照として投入（render thread 専用）。
  int ProcessRender(const int16_t* input, size_t samples);

  // ProcessReverseStream 投入から echo 含み capture 到達までの遅延通知。
  // AEC3 は内部 delay 推定を持つため補助的（開発計画 §2 修正 4）。
  void SetStreamDelayMs(int delay_ms);

  // APM を作り直して内部状態を破棄する（Initialize 済みが前提）。
  void Reset();

 private:
  struct Impl;  // WebRTC 型は .cc 内に隠蔽
  std::unique_ptr<Impl> impl_;
};

}  // namespace local_audio

#endif  // LOCAL_AUDIO_LOCAL_AUDIO_PROCESSOR_H_
