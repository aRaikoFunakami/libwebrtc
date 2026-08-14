// SPSC（単一 producer / 単一 consumer）の int16 PCM ring buffer。
// AudioRecord の read サイズと APM の 10ms frame (480 samples @48kHz) を吸収する。
// audio callback path から呼ぶため、Push/PopFrame は lock-free・heap alloc なし
// （開発計画 §12）。空きが足りない Push は chunk 全体を捨てて dropped カウンタを
// 増やす（all-or-nothing。部分書き込みはしない。callback をブロックしない）。
#ifndef LOCAL_AUDIO_AUDIO_FRAME_BUFFER_H_
#define LOCAL_AUDIO_AUDIO_FRAME_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace local_audio {

class AudioFrameBuffer {
 public:
  // capacity_samples: 事前確保するサンプル数（例: 48kHz で 1 秒 = 48000）。
  explicit AudioFrameBuffer(size_t capacity_samples);

  AudioFrameBuffer(const AudioFrameBuffer&) = delete;
  AudioFrameBuffer& operator=(const AudioFrameBuffer&) = delete;

  // producer thread 専用。全サンプル書けたら true。
  // 空きが足りなければ何も書かず n サンプル分 drop を計上して false。
  bool Push(const int16_t* samples, size_t n);

  // consumer thread 専用。frame_samples 揃っていれば out へコピーし true。
  // 足りなければ何もせず false（部分 frame は返さない = framing を崩さない）。
  bool PopFrame(int16_t* out, size_t frame_samples);

  // 読み出し可能サンプル数（近似値。SPSC なら安全側）。
  size_t Available() const;

  size_t dropped_samples() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  // consumer 側から全破棄（barge-in 時のキュー捨て等）。
  void Clear();

 private:
  std::vector<int16_t> buf_;
  const size_t cap_;
  std::atomic<size_t> write_pos_{0};  // 累積書き込みサンプル数
  std::atomic<size_t> read_pos_{0};   // 累積読み出しサンプル数
  std::atomic<size_t> dropped_{0};
};

}  // namespace local_audio

#endif  // LOCAL_AUDIO_AUDIO_FRAME_BUFFER_H_
