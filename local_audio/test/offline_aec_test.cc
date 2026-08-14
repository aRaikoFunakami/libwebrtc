// LocalAudioProcessor のオフライン AEC テスト（Linux ホスト実行、Phase 1）。
//
// 合成 far-end 信号（バースト状ノイズ）をスピーカー経路に見立て、
// 遅延 + 減衰させた echo を capture に混入して AEC3 の消去量を測る。
// 実機の音響経路は Phase 3 で評価する。ここでは
//   - AEC on で echo が大きく減衰すること（ERLE > 10dB）
//   - AEC off では減衰しないこと（正しく比較できていることの検証）
//   - double-talk で near-end が残ること
//   - 長時間（10 分相当）連続処理で crash/リークがないこと（ASan）
// を確認する。
//
// ビルド: ninja -C out/linux_asan local_audio:offline_aec_test
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "local_audio/local_audio_processor.h"

namespace {

constexpr int kRate = 48000;
constexpr size_t kFrame = 480;  // 10ms
constexpr size_t kEchoDelaySamples = 960;  // 20ms（スピーカー→マイク想定）
constexpr float kEchoGain = 0.5f;

// 乱数（Xorshift、再現性のため固定シード）
uint32_t rng_state = 0x12345678;
float NextNoise() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return (static_cast<int32_t>(rng_state) / 2147483648.0f);
}

// far-end: 0.5 秒オン / 0.5 秒オフのバースト状ノイズ（-20dBFS 程度）
int16_t FarEndSample(size_t i) {
  const bool active = (i / (kRate / 2)) % 2 == 0;
  return active ? static_cast<int16_t>(NextNoise() * 3000.0f) : 0;
}

// near-end: 1kHz トーン（double-talk 区間のみ）
int16_t NearEndSample(size_t i, bool enabled) {
  if (!enabled) return 0;
  return static_cast<int16_t>(2000.0 * std::sin(2.0 * M_PI * 1000.0 * i / kRate));
}

double FrameEnergy(const int16_t* x, size_t n) {
  double e = 0;
  for (size_t i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
  return e;
}

// far-end のみ区間で echo 消去量（dB）を測る。
// near_end_on=true なら double-talk 構成で near-end 残存エネルギーを返す。
struct RunResult {
  double erle_db;         // far-end only 区間の in/out エネルギー比
  double near_out_energy; // double-talk 区間の出力エネルギー
};

RunResult Run(bool aec_on, bool near_end_on, size_t seconds) {
  local_audio::LocalAudioProcessor p;
  local_audio::AudioProcessorConfig cfg;
  cfg.enable_aec = aec_on;
  cfg.enable_ns = false;   // 測定を AEC のみに分離
  cfg.enable_agc = false;
  bool ok = p.Initialize(cfg);
  assert(ok);
  p.SetStreamDelayMs(20);

  std::vector<int16_t> echo_line(kEchoDelaySamples, 0);  // 遅延線
  size_t echo_pos = 0;

  const size_t total = seconds * kRate / kFrame;  // frame 数
  const size_t settle = total / 2;                // 前半は収束待ち
  double in_e = 0, out_e = 0, near_e = 0;
  size_t sample_idx = 0;

  int16_t render[kFrame], capture[kFrame], out[kFrame];
  for (size_t f = 0; f < total; ++f) {
    for (size_t i = 0; i < kFrame; ++i) {
      render[i] = FarEndSample(sample_idx + i);
    }
    int r = p.ProcessRender(render, kFrame);
    assert(r == 0);

    // echo 合成: capture = delay(render) * gain + near_end
    for (size_t i = 0; i < kFrame; ++i) {
      const int16_t echoed = echo_line[echo_pos];
      echo_line[echo_pos] = render[i];
      echo_pos = (echo_pos + 1) % kEchoDelaySamples;
      const float mixed = echoed * kEchoGain +
                          NearEndSample(sample_idx + i, near_end_on);
      capture[i] = static_cast<int16_t>(mixed);
    }
    r = p.ProcessCapture(capture, kFrame, out);
    assert(r == 0);

    // far-end アクティブ区間のみ集計（収束後）
    if (f >= settle) {
      const bool far_active = ((sample_idx / (kRate / 2)) % 2) == 0;
      if (far_active && !near_end_on) {
        in_e += FrameEnergy(capture, kFrame);
        out_e += FrameEnergy(out, kFrame);
      }
      if (near_end_on) {
        near_e += FrameEnergy(out, kFrame);
      }
    }
    sample_idx += kFrame;
  }

  RunResult res;
  res.erle_db = (out_e > 0) ? 10.0 * std::log10(in_e / out_e) : 99.0;
  res.near_out_energy = near_e;
  return res;
}

}  // namespace

int main() {
  // 不正フォーマット拒否
  {
    local_audio::LocalAudioProcessor p;
    local_audio::AudioProcessorConfig bad;
    bad.sample_rate_hz = 16000;
    assert(!p.Initialize(bad));
    int16_t buf[480] = {};
    assert(p.ProcessCapture(buf, 480, buf) != 0);  // 未初期化
    local_audio::AudioProcessorConfig good;
    assert(p.Initialize(good));
    assert(p.ProcessCapture(buf, 123, buf) != 0);  // frame サイズ不正
  }

  // AEC on: echo が消える
  const RunResult on = Run(/*aec_on=*/true, /*near_end_on=*/false, 20);
  printf("AEC on : ERLE = %.1f dB\n", on.erle_db);

  // AEC off: 消えない（比較の妥当性確認）
  const RunResult off = Run(/*aec_on=*/false, /*near_end_on=*/false, 20);
  printf("AEC off: ERLE = %.1f dB\n", off.erle_db);

  assert(on.erle_db > 10.0);
  assert(off.erle_db < 3.0);

  // double-talk: near-end が完全には消えない（barge-in 成立の前提）
  const RunResult dt = Run(/*aec_on=*/true, /*near_end_on=*/true, 20);
  printf("double-talk: near-end out energy = %.3g\n", dt.near_out_energy);
  assert(dt.near_out_energy > 0);

  // NS/AGC 有効での長時間安定性（10 分相当 = 60000 frames）
  {
    local_audio::LocalAudioProcessor p;
    local_audio::AudioProcessorConfig cfg;  // 全機能 on
    assert(p.Initialize(cfg));
    int16_t render[kFrame], out[kFrame];
    size_t idx = 0;
    for (size_t f = 0; f < 60000; ++f) {
      for (size_t i = 0; i < kFrame; ++i) render[i] = FarEndSample(idx + i);
      assert(p.ProcessRender(render, kFrame) == 0);
      assert(p.ProcessCapture(render, kFrame, out) == 0);
      idx += kFrame;
      if (f == 30000) p.Reset();  // 途中 Reset も破綻しない
    }
  }

  printf("offline_aec_test: ALL PASS\n");
  return 0;
}
