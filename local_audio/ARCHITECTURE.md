# local_audio アーキテクチャ

[README.md](README.md) が「何を・なぜ・どう使うか」を説明するのに対し、本ドキュメントは
**構造と実行時の流れ**を図で説明する。

## 1. システム全体（2リポジトリの境界）

`local_audio` は WebRTC fork 内の1ディレクトリであり、通信スタック（PeerConnection等）とは
独立に APM/AEC3 だけを Android アプリへ届けるための境界層。アプリ側は `.so` を受け取るだけで、
WebRTC のソースツリーにもビルドシステムにも依存しない。

```mermaid
flowchart TB
    subgraph FORK["aRaikoFunakami/libwebrtc（fork, local-audio ブランチ）"]
        direction TB
        UPSTREAM["WebRTC upstream tree<br/>（branch-heads/7300 起点、無改変）"]
        ROOT["ルート BUILD.gn<br/>+4行のみ: is_android → deps local_audio"]
        LA["local_audio/<br/>（本ドキュメントの対象）"]
        UPSTREAM --> ROOT --> LA
    end

    subgraph APP["aRaikoFunakami/android-local-voice-agent（アプリ）"]
        direction TB
        SO["liblocal_audio_engine.so<br/>jniLibs/arm64-v8a/"]
        KOTLIN["LocalAudioEngine.kt<br/>CapturePipeline / RenderPipeline"]
        SO --> KOTLIN
    end

    LA -- "ninja local_audio:local_audio_engine<br/>x86_64 Linuxホストでビルド" --> SO
```

## 2. GN ビルドターゲット依存関係

`local_audio/BUILD.gn` 内のターゲット構成。Android では `.so` 一つに、
ホストではテスト実行ファイル2本に分岐する（`group("local_audio")` が入口）。

```mermaid
flowchart LR
    ROOT["ルート BUILD.gn<br/>group default に4行追加"] -->|is_androidのみ| G["group local_audio"]

    subgraph android["is_android"]
        ENGINE["rtc_shared_library<br/>local_audio_engine（.so）"]
    end
    subgraph host["非Android ホスト"]
        FBT["rtc_executable<br/>audio_frame_buffer_test"]
        AEC["rtc_executable<br/>offline_aec_test"]
    end

    G -->|Android| ENGINE
    G -->|host| FBT
    G -->|host| AEC

    JNI["local_audio_jni.cc"] --> ENGINE
    PROC["rtc_library<br/>local_audio_processor"] --> ENGINE
    BUF["rtc_library<br/>audio_frame_buffer"] --> ENGINE
    PROC --> AEC
    BUF --> FBT

    PROC -.->|deps| APMDEP["upstream:<br/>api/audio:audio_processing<br/>api/audio:aec3_factory<br/>api/audio:builtin_audio_processing_builder<br/>api/environment:environment_factory"]
```

## 3. クラス構成（WebRTC 型の隠蔽）

`LocalAudioProcessor` は Pimpl で WebRTC の型（`AudioProcessing`, `EchoCanceller3Factory` 等）を
`.cc` 側に閉じ込める。ヘッダを見る側（JNI や将来の別バックエンド実装）は WebRTC を一切意識しない。

```mermaid
classDiagram
    class LocalAudioProcessor {
        +Initialize(AudioProcessorConfig) bool
        +ProcessCapture(int16_t* in, size_t, int16_t* out) int
        +ProcessRender(int16_t* in, size_t) int
        +SetStreamDelayMs(int)
        +Reset()
        -unique_ptr~Impl~ impl_
    }
    class Impl {
        private, .cc内のみ
        scoped_refptr~AudioProcessing~ apm
        StreamConfig stream_config
        atomic~int~ stream_delay_ms
    }
    class AudioProcessing {
        WebRTC upstream
        ProcessStream()
        ProcessReverseStream()
        set_stream_delay_ms()
    }
    class BuiltinAudioProcessingBuilder {
        WebRTC upstream
        SetEchoControlFactory()
        Build(Environment) AudioProcessing
    }
    class EchoCanceller3Factory {
        WebRTC upstream
    }

    LocalAudioProcessor *-- Impl : Pimpl
    Impl --> AudioProcessing : 保持
    LocalAudioProcessor ..> BuiltinAudioProcessingBuilder : Initialize内で使用
    BuiltinAudioProcessingBuilder ..> EchoCanceller3Factory : SetEchoControlFactory
    BuiltinAudioProcessingBuilder --> AudioProcessing : Buildが生成

    note for LocalAudioProcessor "ヘッダ.hはWebRTC型を含まない"
```

## 4. 実行時データフロー（10ms サイクル、2スレッド構成）

capture / render は**別スレッド**だが、**同一の `AudioProcessing` インスタンス**（= 同一
`LocalAudioProcessor`）を共有する。二重に APM を作ると AEC 状態が分裂して破綻するため、
アプリ側は capture 開始時に作った `LocalAudioProcessor` の handle を render 側へ渡す設計にしている
（[CapturePipeline.kt](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/app/src/main/java/com/example/localvoiceagent/audio/CapturePipeline.kt) /
[RenderPipeline.kt](https://github.com/aRaikoFunakami/android-local-voice-agent/blob/main/app/src/main/java/com/example/localvoiceagent/audio/RenderPipeline.kt)）。

```mermaid
sequenceDiagram
    participant Mic as AudioRecord
    participant CT as Capture thread<br/>Kotlin
    participant JNI as local_audio_jni.cc
    participant LAP as LocalAudioProcessor
    participant APM as WebRTC APM/AEC3
    participant RT as Render thread<br/>Kotlin
    participant Spk as AudioTrack

    Note over CT,RT: 起動時 CTがcreateでhandle取得、RTはそのhandleを共有

    loop 10msごと capture
        Mic->>CT: 480 samples DirectByteBuffer
        CT->>JNI: processCapture handle in out
        JNI->>LAP: ProcessCapture
        LAP->>APM: set_stream_delay_ms + ProcessStream
        APM-->>LAP: clean PCM
        LAP-->>JNI: 0 成功
        JNI-->>CT: out echo除去済み
        CT->>CT: STTへ渡す
    end

    loop 10msごと render 非同期に並行
        RT->>RT: TTSまたは無音480samples用意
        RT->>JNI: processRender handle frame
        JNI->>LAP: ProcessRender
        LAP->>APM: ProcessReverseStream AEC参照として登録のみ
        APM-->>LAP: OK
        RT->>Spk: 同じframeを再生 write
    end

    Note over APM: capture threadのみがstream setterとProcessStreamを呼び、<br/>render threadのみがProcessReverseStreamを呼ぶ。<br/>WebRTC APMのスレッド制約に適合。
```

## 5. JNI 境界（DirectByteBuffer 契約）

```mermaid
flowchart LR
    subgraph Kotlin["Kotlin"]
        FB["ByteBuffer.allocateDirect 960<br/>事前確保・使い回し"]
    end
    subgraph JNILayer["local_audio_jni.cc"]
        GA["GetDirectBufferAddress<br/>アロケーションなし"]
        FH["FromHandle jlong<br/>reinterpret_cast"]
    end
    subgraph Native["LocalAudioProcessor native heap"]
        OBJ["1回のcreateで1インスタンス<br/>handleはポインタのjlongキャスト<br/>レジストリなし、破棄はdestroy呼び出し側責任"]
    end

    FB -->|processCapture/processRender 呼び出しごと| GA
    GA --> FH --> OBJ
```

audio callback path（capture/render 両スレッド）では JNI 呼び出しごとのヒープ確保をしない設計。
`ByteBuffer` はアプリ起動時に確保して使い回し、`create()`/`destroy()` のみが確保・解放を伴う。

## 関連ドキュメント

- [README.md](README.md) — 変更内容・検証結果・API使用例
- [android-local-voice-agent](https://github.com/aRaikoFunakami/android-local-voice-agent) — アプリ全体のアーキテクチャ（STT/LLM/TTSを含む）
