#include "local_audio/local_audio_processor.h"

#include <atomic>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_canceller3_factory.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"

namespace local_audio {

namespace {
constexpr int kSampleRateHz = 48000;
constexpr int kChannels = 1;
constexpr size_t kFrameSamples = 480;  // 10ms @ 48kHz
}  // namespace

struct LocalAudioProcessor::Impl {
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
  webrtc::StreamConfig stream_config{kSampleRateHz, kChannels};
  AudioProcessorConfig config;
  std::atomic<int> stream_delay_ms{0};
};

LocalAudioProcessor::LocalAudioProcessor() = default;
LocalAudioProcessor::~LocalAudioProcessor() = default;

bool LocalAudioProcessor::Initialize(const AudioProcessorConfig& config) {
  if (config.sample_rate_hz != kSampleRateHz || config.channels != kChannels) {
    return false;
  }

  webrtc::AudioProcessing::Config apm_config;
  apm_config.echo_canceller.enabled = config.enable_aec;
  apm_config.echo_canceller.mobile_mode = false;  // AEC3（AECM 禁止、計画 §8）
  apm_config.noise_suppression.enabled = config.enable_ns;
  apm_config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  apm_config.high_pass_filter.enabled = true;
  // AGC2 digital のみ。AGC1(analog) は削除予定 (webrtc:7494) かつ
  // Android にアナログゲイン制御がないため使わない。
  apm_config.gain_controller2.enabled = config.enable_agc;
  apm_config.gain_controller2.adaptive_digital.enabled = config.enable_agc;

  webrtc::BuiltinAudioProcessingBuilder builder(apm_config);
  if (config.enable_aec) {
    // EchoCanceller3Config を明示注入（Phase 3 のチューニング経路を最初から確保）
    builder.SetEchoControlFactory(
        std::make_unique<webrtc::EchoCanceller3Factory>(
            webrtc::EchoCanceller3Config()));
  }
  auto apm = builder.Build(webrtc::CreateEnvironment());
  if (apm == nullptr) {
    return false;
  }

  auto impl = std::make_unique<Impl>();
  impl->apm = std::move(apm);
  impl->config = config;
  impl_ = std::move(impl);
  return true;
}

int LocalAudioProcessor::ProcessCapture(const int16_t* input,
                                        size_t samples,
                                        int16_t* output) {
  if (impl_ == nullptr || samples != kFrameSamples) {
    return -1;
  }
  // AEC3 の delay 補助情報。capture thread から stream setter を呼ぶのは
  // APM の並行性制約（stream setter と ProcessStream は同一スレッド）に適合。
  impl_->apm->set_stream_delay_ms(
      impl_->stream_delay_ms.load(std::memory_order_relaxed));
  return impl_->apm->ProcessStream(input, impl_->stream_config,
                                   impl_->stream_config, output);
}

int LocalAudioProcessor::ProcessRender(const int16_t* input, size_t samples) {
  if (impl_ == nullptr || samples != kFrameSamples) {
    return -1;
  }
  // in-place 出力（render 側の出力は使わない）
  static thread_local int16_t scratch[kFrameSamples];
  return impl_->apm->ProcessReverseStream(input, impl_->stream_config,
                                          impl_->stream_config, scratch);
}

void LocalAudioProcessor::SetStreamDelayMs(int delay_ms) {
  if (impl_ != nullptr) {
    impl_->stream_delay_ms.store(delay_ms, std::memory_order_relaxed);
  }
}

void LocalAudioProcessor::Reset() {
  if (impl_ != nullptr) {
    Initialize(impl_->config);
  }
}

}  // namespace local_audio
