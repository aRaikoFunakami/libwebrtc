// AudioFrameBuffer の assert ベース自己テスト（Linux ホストで実行）。
// ビルド: ninja -C out/linux_x64 local_audio:audio_frame_buffer_test
#include <cassert>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

#include "local_audio/audio_frame_buffer.h"

using local_audio::AudioFrameBuffer;

int main() {
  // 基本: push → pop で同一データ
  {
    AudioFrameBuffer b(1000);
    int16_t in[480];
    std::iota(in, in + 480, 0);
    assert(b.Push(in, 480));
    assert(b.Available() == 480);
    int16_t out[480] = {};
    assert(b.PopFrame(out, 480));
    for (int i = 0; i < 480; ++i) assert(out[i] == in[i]);
    assert(b.Available() == 0);
  }

  // 端数 push の蓄積と部分 frame 拒否
  {
    AudioFrameBuffer b(1000);
    int16_t in[300] = {};
    assert(b.Push(in, 300));
    int16_t out[480];
    assert(!b.PopFrame(out, 480));  // 足りない → false、framing 崩さない
    assert(b.Push(in, 300));
    assert(b.PopFrame(out, 480));
    assert(b.Available() == 120);
  }

  // 満杯時 drop とカウンタ
  {
    AudioFrameBuffer b(500);
    int16_t in[480] = {};
    assert(b.Push(in, 480));
    assert(!b.Push(in, 480));  // 空き 20 < 480 → 全 drop
    assert(b.dropped_samples() == 480);
    assert(b.Available() == 480);
  }

  // wrap-around 整合性（cap 非整数倍の push/pop を繰り返す）
  {
    AudioFrameBuffer b(1024);
    int16_t seq = 0;
    int16_t expect = 0;
    int16_t in[313];
    int16_t out[480];
    for (int iter = 0; iter < 1000; ++iter) {
      for (auto& v : in) v = seq++;
      assert(b.Push(in, 313));
      while (b.PopFrame(out, 480)) {
        for (int i = 0; i < 480; ++i) assert(out[i] == expect++);
      }
    }
  }

  // SPSC 並行: producer 48k サンプル/回 ×100、consumer は 480 ずつ検証
  {
    AudioFrameBuffer b(48000);
    std::thread producer([&] {
      int16_t v = 0;
      int16_t in[441];
      for (int iter = 0; iter < 10000;) {
        for (auto& x : in) x = v++;
        if (b.Push(in, 441)) {
          ++iter;
        } else {
          v -= 441;  // all-or-nothing なので同一 chunk の再試行が安全
          std::this_thread::yield();
        }
      }
    });
    int16_t expect = 0;
    int16_t out[480];
    size_t total = 0;
    while (total < 441u * 10000 / 480 * 480) {
      if (b.PopFrame(out, 480)) {
        for (int i = 0; i < 480; ++i) assert(out[i] == static_cast<int16_t>(expect++));
        total += 480;
      }
    }
    producer.join();
  }

  // Clear
  {
    AudioFrameBuffer b(1000);
    int16_t in[480] = {};
    b.Push(in, 480);
    b.Clear();
    assert(b.Available() == 0);
  }

  printf("audio_frame_buffer_test: ALL PASS\n");
  return 0;
}
