#include "local_audio/audio_frame_buffer.h"

#include <cstring>

namespace local_audio {

AudioFrameBuffer::AudioFrameBuffer(size_t capacity_samples)
    : buf_(capacity_samples), cap_(capacity_samples) {}

bool AudioFrameBuffer::Push(const int16_t* samples, size_t n) {
  const size_t w = write_pos_.load(std::memory_order_relaxed);
  const size_t r = read_pos_.load(std::memory_order_acquire);
  const size_t free_space = cap_ - (w - r);

  // all-or-nothing: 部分書き込みはリトライ不能なセマンティクスになるため、
  // 空きが足りなければ chunk 全体を drop する（callback はブロックしない）。
  if (n > free_space) {
    dropped_.fetch_add(n, std::memory_order_relaxed);
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    buf_[(w + i) % cap_] = samples[i];
  }
  write_pos_.store(w + n, std::memory_order_release);
  return true;
}

bool AudioFrameBuffer::PopFrame(int16_t* out, size_t frame_samples) {
  const size_t r = read_pos_.load(std::memory_order_relaxed);
  const size_t w = write_pos_.load(std::memory_order_acquire);
  if (w - r < frame_samples) {
    return false;
  }
  for (size_t i = 0; i < frame_samples; ++i) {
    out[i] = buf_[(r + i) % cap_];
  }
  read_pos_.store(r + frame_samples, std::memory_order_release);
  return true;
}

size_t AudioFrameBuffer::Available() const {
  return write_pos_.load(std::memory_order_acquire) -
         read_pos_.load(std::memory_order_acquire);
}

void AudioFrameBuffer::Clear() {
  read_pos_.store(write_pos_.load(std::memory_order_acquire),
                  std::memory_order_release);
}

}  // namespace local_audio
