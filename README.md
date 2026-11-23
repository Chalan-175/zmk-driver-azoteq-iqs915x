# LandsFall 向け Azoteq IQS9150 トラックパッドドライバ

自作キーボード **LandsFall 専用**に開発した ZMK／Zephyr 用トラックパッド入力ドライバです。LandsFall に搭載する **Azoteq IQS9150** を想定しています。Devicetree の compatible は `azoteq,iqs915x` です。

## 主な特徴

- **ポインタ操作** — センサーの絶対座標に LandsFall のキーピッチに合わせた周期補正と MCU 側の動的 IIR フィルタを適用し、相対移動として出力します。1 本指のポインタ加速も設定できます。
- **タップとドラッグ** — 1 本指タップを左クリック、2 本指タップを右クリックとして扱います。タップ後に再タッチして左ボタンを保持するドラッグにも対応します。
- **スクロールとスワイプ** — 2 本指の縦・横スクロール、指を離した後の慣性スクロール、3 本指・4 本指の方向別スワイプを扱います。
- **接触フレーム出力** — マウス入力と、最大 4 接点の座標を渡す接触フレーム出力を切り替えられます。
- **左右別のセンサー設定** — LandsFall の左右それぞれに対応するチャンネル無効化プロファイルを持ちます。
- **電力管理と診断** — Active、固定 LP2、IC Suspend を制御し、センサーのリセットを検出して再初期化します。ATI や接触座標の診断用 API も備えています。

## ZMK への組み込み

利用側ファームウェアの west マニフェストにこのリポジトリを Zephyr モジュールとして追加します。ZMK 設定で `CONFIG_I2C=y` と `CONFIG_ZMK_POINTING=y` を有効にしてください。`CONFIG_INPUT_AZOTEQ_IQS915X` は、GPIO・I²C・Zephyr Input が有効で、対応する Devicetree ノードがあるときに既定で有効になります。

以下は LandsFall の右側を想定した overlay の例です。I²C バスと GPIO は実際の配線に合わせて変更してください。`rdy-gpios` は必須で、`reset-gpios` は配線している場合に指定します。

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

/ {
    trackpad_input {
        compatible = "zmk,input-listener";
        device = <&trackpad>;
    };
};

&i2c0 {
    status = "okay";

    trackpad: iqs915x@56 {
        compatible = "azoteq,iqs915x";
        reg = <0x56>;
        status = "okay";

        rdy-gpios = <&gpio0 15 GPIO_ACTIVE_LOW>;
        reset-gpios = <&gpio0 14 GPIO_ACTIVE_LOW>;
        channel-profile = "landsfall-right";

        one-finger-tap;
        two-finger-tap;
        tap-and-hold;
        scroll;
        scroll-inertia;
        three-finger-swipe;
        four-finger-swipe;
    };
};
```

左側では `channel-profile = "landsfall-left";` を選択します。左右プロファイルは Azoteq の設定が 18 Rx × 12 Tx の場合に使用でき、それぞれ 18 チャンネルを無効にします。既定の `export` は Azoteq の出力に含まれるマスクを使用します。スクロール感度や慣性、ポインタ加速、スワイプ閾値は [Devicetree binding](dts/bindings/input/azoteq,iqs915x-common.yaml) を参照してください。

## センサーの初期化

[IQS9150_init.h](drivers/input/IQS9150_init.h) に Azoteq GUI から出力した設定を収録しています。ビルド時に [生成スクリプト](scripts/generate_init_array.py) が 1,174 バイト分のマクロを参照する配列を生成します。センサー設定を更新するには、GUI から出力したヘッダーでこのファイルを置き換え、利用側の ZMK ファームウェアをビルドします。

ドライバは転送時に通信制御や選択したチャンネルマスクを反映します。IC がリセットされた場合は設定を書き込み、TP Re-ATI と通信モードの確認を行います。MCU の再起動などで IC に設定が保持されている場合は、解像度やチャンネルマスクなどを確認し、一致すれば再転送を省略します。保持設定に不一致があれば、ソフトウェアリセットから再初期化します。

## ファームウェア連携と診断

| 用途 | 参照先 |
| --- | --- |
| 有効・無効、電力モード、状態通知 | [iqs915x.h](include/iqs915x.h) の `iqs915x_set_enabled()`、`iqs915x_set_mode()`、`iqs915x_get_status()`、`iqs915x_subscribe_status()` |
| マウスと接触フレームの切り替え | [iqs915x.h](include/iqs915x.h) の `iqs915x_set_output_mode()`、`iqs915x_subscribe_contacts()` と [フレーム定義](include/iqs915x_contacts.h) |
| 座標補正、動的 IIR、接触トレース | [iqs915x_processing.h](include/iqs915x_processing.h) |
| ATI Compensation、Base Count、Count／Reference | [チャンネル取得](include/iqs915x_channel_capture.h)、[Base Count](include/iqs915x_base_count.h)、[Count／Reference](include/iqs915x_count_reference.h) |

モード変更 API は要求を非同期で受け付けます。適用状態は `iqs915x_get_status()` で確認できます。接触フレームの受信側は `SESSION_END` でその世代の接触を解放し、同じ世代の遅延フレームを破棄してください。設定の永続化と左右間の同期は利用側ファームウェアが担当します。

補正前の座標を確認するには、対象 MCU で `CONFIG_INPUT_AZOTEQ_IQS915X_COORD_LOG=y` を有効にします。

## 開発時の確認

このリポジトリで初期化データのホストテストを実行できます。

```sh
python3 tests/init_header.py
python3 tests/init_data.py
```

ファームウェアのビルドは利用側の ZMK ワークスペースで行います。実機では、起動、タップとドラッグ、縦横スクロールと慣性、3／4 本指スワイプ、電力モードの切り替え、IC リセットからの復帰を確認してください。

## Acknowledgements

- ZMK モジュールとしての出発点になった `zmk-driver-azoteq-iqs5xx` の作者・貢献者に感謝します。[フォーク時点の履歴](https://github.com/Chalan-175/zmk-driver-azoteq-iqs915x/commit/27321f0)
- 慣性スクロールの実装を見直す際に参考にした `zmk-input-inertia` の作者、amgskobo さんに感謝します。[amgskobo/zmk-input-inertia](https://github.com/amgskobo/zmk-input-inertia)
- センサーの仕様と設定には、Azoteq のデータシートと GUI の出力を参照しています。[IQS9150／IQS9151 データシート](https://www.azoteq.com/images/stories/pdf/IQS9150_IQS9151_datasheet.pdf)

## ライセンス

[MIT License](LICENSE)
