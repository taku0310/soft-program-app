# softplc-runtime

Soft PLC本体（C11 / Linux / PREEMPT_RT）。

## ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Docker

```bash
docker build -t softplc-runtime:dev .
docker run --rm --privileged --network host softplc-runtime:dev
```

## ディレクトリ

```
softplc-runtime/
├── docker/entrypoint.sh
├── src/
│   ├── plc_runtime.c           # メインループ (10ms)
│   ├── ethernet_ip/            # CIP スタック
│   ├── mqtt/                   # MQTT パブリッシャー
│   ├── scheduler/              # CPU affinity / cycle monitor
│   └── ipc/                    # 共有メモリ (Backend連携)
├── config/
│   ├── plc_config.json
│   └── runtime.conf
├── tests/                      # ctest ベースのユニットテスト
└── CMakeLists.txt
```

## 検証

```bash
# スキャンサイクル ジッター測定
cyclictest -t 1 -p 80 -i 10000 -n -l 100000

# ctest
cd build && ctest --output-on-failure
```

## 完了条件（Phase 1）
- [ ] cyclictest: max jitter < 1ms
- [ ] 16デバイス CIP 接続成功
- [ ] MQTT 1秒間隔配信
