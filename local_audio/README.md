# local_audio

このディレクトリは [aRaikoFunakami/libwebrtc](https://github.com/aRaikoFunakami/libwebrtc)（Google WebRTC の fork、
`local-audio` ブランチ = upstream `refs/branch-heads/7300` 起点）に追加した唯一の独自コードです。
WebRTC の Audio Processing Module（APM）/ AEC3 を、通信ライブラリとしてではなく
**Android 端末内の音響前処理エンジン**として使うための薄いラッパと JNI bridge を提供します。

利用側（Android アプリ）は
[aRaikoFunakami/android-local-voice-agent](https://github.com/aRaikoFunakami/android-local-voice-agent) です。

本ドキュメントは呼び出す側（アプリ実装者）向けに、変更内容・検証結果・API の使い方と制約を説明する。
local_audio 自体の内部実装（クラス構成・スレッドモデル・ビルド依存関係）を変更する場合は
[ARCHITECTURE.md](ARCHITECTURE.md) を参照。

## 1. upstream への変更内容

upstream（Google WebRTC）のファイルへの変更は **ルート `BUILD.gn` の 4 行のみ**です:

```gn
group("default") {
  ...
  if (is_android) {
    # local_audio: 本 fork 独自の音響前処理エンジン（唯一の upstream 変更点）
    deps += [ "local_audio" ]
  }
  ...
}
```

これは GN が「ルートから参照されない BUILD.gn をビルドグラフに載せない」仕様のための最小限の配線で、
`local_audio` 配下のコード自体は upstream の何にも依存の外側から手を入れていません。
revision 更新（`branch-heads/N` の切り替え）時は、この 4 行が新しいルート `BUILD.gn` にそのまま
当たることを確認するだけで済みます。

追加ファイル一覧（643 行）:

| ファイル | 役割 |
|---|---|
| `BUILD.gn` | GN ターゲット定義。Android では `.so`、ホストではテスト群をビルド |
| `local_audio_processor.{h,cc}` | APM/AEC3/NS/AGC2 のラッパ。**ヘッダに WebRTC 型を一切露出させない**（Pimpl） |
| `audio_frame_buffer.{h,cc}` | SPSC lock-free の 10ms framing ring buffer |
| `local_audio_jni.cc` | JNI bridge（`JNI_OnLoad` + `RegisterNatives` 方式） |
| `test/audio_frame_buffer_test.cc` | ring buffer の assert 自己テスト（Linux ホスト） |
| `test/offline_aec_test.cc` | 合成 echo による AEC のオフライン検証（Linux ホスト） |

開発履歴（PR #1〜#5、[aRaikoFunakami/libwebrtc pulls](https://github.com/aRaikoFunakami/libwebrtc/pulls?q=is%3Apr+is%3Aclosed)）:

1. **#1** hello-world `rtc_shared_library`（バージョン文字列のみ、JNI export方式の検証）
2. **#2** `AudioFrameBuffer`（SPSC ring buffer、ASan テストつき）
3. **#3** `LocalAudioProcessor`（APM/AEC3/NS/AGC2 ラップ）
4. **#4** `offline_aec_test`（合成 echo での ERLE 実測）
5. **#5** JNI bridge 本実装（5操作 + version）

## 2. 検証結果

### ビルドサイズの推移

| 段階 | `.so` サイズ（stripped） |
|---|---|
| hello-world（PR #1） | 259 KB |
| APM/AEC3 リンク後（PR #5 時点） | **863 KB**（unstripped: 10.5 MB） |

### AEC のオフライン検証（Linux ホスト、`offline_aec_test`、PR #4）

合成 far-end（バースト状ノイズ）+ 遅延20ms・ゲイン0.5 の echo 合成で測定:

| ケース | ERLE |
|---|---|
| AEC on | **58.5 dB** |
| AEC off（対照群） | 0.3 dB |
| double-talk | near-end エネルギー残存を確認（barge-in 前提が成立） |
| 長時間 | 60,000 frame（10分相当）+ 途中 `Reset()`、ASan クリーン |

### Android 実配管での検証（アプリ側、[docs/aec_evaluation.md](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/docs/aec_evaluation.md)）

実際の `AudioRecord`（ホストマイクのノイズ込み）+ 合成 echo 注入で測定: **ERLE 33.8 dB**
（ホスト単体テストの 58.5dB よりは低いが、実マイクノイズと非線形経路を考えれば妥当な値）

### 未検証（実機が必要）

実スピーカー→マイクの物理的な音響経路での AEC 評価は、エミュレータには音響経路がないため未実施。
`EchoCanceller3Config` の実機チューニング用の注入口（後述）は実装済み。

## 3. ライブラリの使い方

### ビルド

x86_64 Linux ホスト限定（WebRTC の prebuilt clang が `Linux_x64` のみのため）。

```bash
# Android 向け .so
gn gen out/android_arm64 --args='target_os="android" target_cpu="arm64" is_debug=false ...'
ninja -C out/android_arm64 local_audio:local_audio_engine
# → out/android_arm64/liblocal_audio_engine.so

# ホスト向けテスト（ASan推奨）
gn gen out/linux_asan --args='is_debug=true is_asan=true ...'
ninja -C out/linux_asan local_audio:audio_frame_buffer_test local_audio:offline_aec_test
./out/linux_asan/audio_frame_buffer_test
./out/linux_asan/offline_aec_test
```

具体的な gn args とコンテナ手順は
[android-local-voice-agent の scripts/build_webrtc_android.sh](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/scripts/build_webrtc_android.sh)
を参照。

### C++ API（`LocalAudioProcessor`）

固定フォーマット: **48kHz / mono / int16 / 10ms(480 samples)**。これ以外は拒否する。

```cpp
#include "local_audio/local_audio_processor.h"

local_audio::LocalAudioProcessor processor;
// capture thread と render thread は同一の processor インスタンスを共有すること。
// 別々に生成すると AEC 状態が分裂する（理由: ARCHITECTURE.md §4）。

local_audio::AudioProcessorConfig config;
config.enable_aec = true;   // AEC3 (mobile_mode=false固定、AECM不使用)
config.enable_ns  = true;   // Noise Suppression (High)
config.enable_agc = true;   // AGC2 digital のみ（AGC1は不使用、webrtc:7494で削除予定のため）
processor.Initialize(config);

// capture thread（AudioRecord 直後）から:
processor.SetStreamDelayMs(20);  // 実測値。定数埋め込み禁止、実行時に設定すること
int16_t out[480];
processor.ProcessCapture(in /*480 samples*/, 480, out);

// render thread（再生直前）から、実際にスピーカーへ送る信号と同一データで:
processor.ProcessRender(render_pcm /*480 samples*/, 480);

// AEC状態を破棄して作り直す（例: audio route変更時）
processor.Reset();
```

ヘッダ（`local_audio_processor.h`）は WebRTC の型を一切含まない（`std::unique_ptr<Impl>` に隠蔽）。
呼び出し側が WebRTC の API 変更の影響を受けないようにするための設計。

**AEC3チューニング**: `local_audio_processor.cc` 内で `EchoCanceller3Factory` へ
`EchoCanceller3Config` を明示注入する経路をすでに用意している（現状はデフォルト値）。
実機評価でパラメータ調整が必要になった場合はここを変更する。

### JNI / Kotlin から使う場合

`local_audio_jni.cc` が公開する 5 操作 + version:

```
create(aec: Boolean, ns: Boolean, agc: Boolean): Long   // handleを返す。0=失敗
processCapture(handle: Long, input: ByteBuffer, output: ByteBuffer): Int  // 0=成功
processRender(handle: Long, input: ByteBuffer): Int
setStreamDelayMs(handle: Long, delayMs: Int)
reset(handle: Long)
destroy(handle: Long)
```

`handle` は `create()` を capture thread 側で1回呼んで取得し、render thread はその同じ `handle` を
使うこと（C++ API と同じ制約。理由は ARCHITECTURE.md §4）。

`input`/`output` は 480 samples(960 bytes) の **DirectByteBuffer**（native order）を呼び出し側で
事前確保して使い回すこと（audio callback path でのアロケーション回避のため）。

Kotlin側の実装例は
[LocalAudioEngine.kt](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/app/src/main/java/com/example/localvoiceagent/LocalAudioEngine.kt)
を参照。

**注意**: Android の既定 version script は `JNI_OnLoad` 以外のシンボルを export しない
（ThinLTOでも内部化される）。そのため `Java_パッケージ名_クラス名_メソッド名` という
命名規約でのJNI関数定義は機能しない。`local_audio_jni.cc` は `JNI_OnLoad` 内で
`RegisterNatives()` を呼ぶ方式で登録している。

### `AudioFrameBuffer`（オプション）

`AudioRecord`/`AudioTrack` の read/write サイズが 10ms(480 samples) 単位と一致しない場合に使う
SPSC ring buffer。`Push()` は all-or-nothing（空き不足時は chunk 全体を drop してカウンタに計上、
部分書き込みはしない）。`android-local-voice-agent` の現行実装は `AudioRecord` の
`READ_BLOCKING` モードで代替しているため未使用だが、TTS 側の可変長 PCM を framing する用途などに
使える。

## 4. revision 更新時の手順

1. upstream の新しい `refs/branch-heads/<N>` を `release-<N>` として fork へ push
2. `local-audio` を `release-<N>` へ rebase（衝突面は基本的にルート `BUILD.gn` の4行のみ）
3. `./out/linux_asan/audio_frame_buffer_test` と `offline_aec_test` を実行し回帰確認
4. NOTICE を再生成（`android-local-voice-agent/scripts/generate_notices.sh`）し差分をレビュー

詳細は
[docs/webrtc_revision.md](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/docs/webrtc_revision.md)。
