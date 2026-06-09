# ソフトPLC + Web設定ツール プロジェクト仕様書

**プロジェクト要件確定日：2026年6月**
**開発期間：6ヶ月 / チーム規模：5名 / 予算：100万円**

## 成果物概要

### 1. ソフトPLC（Docker + Linux）

- Docker コンテナ（Ubuntu 22.04 LTS + PREEMPT_RT カーネル）
- スキャンサイクル：10ms（ジッター ±1ms以内）
- 通信プロトコル：EtherNet/IP（CIP接続16デバイス同時、Exclusive + Input Only）
- MQTTブローカー：ローカル（工場内LAN）連携、1秒ごと配信（ペイロード ≤512bytes）
- プログラミング言語：Structured Text (ST) を主言語、Ladder Logic (LL) もサポート
- 内蔵IO：なし（すべて EtherNet/IP経由）

### 2. Web設定ツール（React + Node.js）

| # | 機能 | 優先度 |
|---|------|--------|
| 1 | ST言語エディタ（シンタックスハイライト、デバッグ） | ⭐⭐⭐ |
| 2 | ラダー図エディタ（基本PLC命令の図形ベース） | ⭐⭐⭐ |
| 3 | EtherNet/IP接続管理UI（CIP設定、リアルタイム監視） | ⭐⭐⭐ |
| 4 | EtherNet/IP接続テスト（疎通テスト） | ⭐⭐⭐ |
| 5 | MQTT設定・監視 | ⭐⭐⭐ |
| 6 | PLC監視ダッシュボード（リアルタイム、ジッター分析、エラーログ） | ⭐⭐⭐ |
| 7 | バージョン管理（Git連携） | ⭐⭐ |

### 3. ドキュメント・テスト

- API仕様書（OpenAPI 3.0）
- インストール手順、トラブルシューティング、ユーザーマニュアル
- 自動テスト（ユニット、統合、E2E、負荷）

## 技術スタック

- **フロントエンド:** React 18 + TypeScript 5 + Monaco Editor + React Flow + Redux Toolkit + MUI
- **バックエンド:** Node.js 18 LTS + Express 4 + Sequelize + SQLite3
- **PLCランタイム:** C11 + CMake + pthread + PREEMPT_RT
- **通信:** WebSocket (socket.io), MQTT (mosquitto), EtherNet/IP (CIP)
- **認証:** JWT + bcrypt
- **暗号化:** TLS 1.3
- **デプロイ:** docker-compose (オンプレ工場内)

## 実装フェーズ

1. **Phase 1（2ヶ月）:** ソフトPLC基本フレーム + EtherNet/IP通信
2. **Phase 2（1.5ヶ月）:** Web設定ツール バックエンド
3. **Phase 3（1.5ヶ月）:** Web設定ツール フロントエンド
4. **Phase 4（1.5ヶ月）:** 統合テスト + ドキュメント

## IEC 61131-3 言語仕様

### ST言語 対応データ型

```
BOOL, BYTE, WORD, DWORD, LINT
INT, DINT, LINT
REAL, LREAL
STRING[256]
DATE_AND_TIME, TIME
ARRAY, STRUCT
```

### ST言語 対応ステートメント

```
制御フロー：IF/THEN/ELSE, CASE, WHILE, FOR, REPEAT
関数：SIN, COS, SQRT, ABS, ROUND, MAX, MIN
文字列：CONCAT, MID, LEN, FIND
日付時刻：CURRENT_TIME, TIME_TO_STRING
IPC通信：WRITE_SHARED_MEMORY, READ_SHARED_MEMORY
```

### Ladder Logic 対応命令

| 命令 | シンボル | 機能 |
|------|---------|------|
| コンタクト（常開） | `-｜-` | ON/OFFスイッチ |
| コンタクト（常閉） | `-╱-` | 反転スイッチ |
| コイル（標準） | `-( )-` | 出力セット |
| コイル（SET） | `-(S)-` | ラッチセット |
| コイル（RESET） | `-(R)-` | ラッチリセット |
| タイマ ON遅延 | `-(TON)-` | T#10ms単位の遅延 |
| カウンタ アップ | `-(CTU)-` | インクリメント |
| 算術演算 | `-(+)-` | 加算 |
| 比較演算 | `-(>)-` | 大小比較 |

## EtherNet/IP 設定例

```json
{
  "connections": [
    {
      "instance_id": "0x01",
      "device_ip": "192.168.1.100",
      "produced_data": { "size": 64, "type": "uint8_t[64]" },
      "consumed_data": { "size": 64, "type": "uint8_t[64]" },
      "rpi_ms": 10,
      "connection_type": "exclusive",
      "timeout_ms": 30
    }
  ],
  "mqtt_publisher": {
    "broker": "192.168.1.50:1883",
    "topic_base": "softplc/devices",
    "publish_interval_ms": 1000,
    "payload_format": "json",
    "max_size_bytes": 512
  }
}
```

## 完了条件（フェーズ別）

### Phase 1
- [ ] Dockerfile + docker-compose.yml（PREEMPT_RT対応）
- [ ] EtherNet/IP CIPスタック（16デバイス）
- [ ] スケジューラ（10ms ±1ms, cyclictest検証）
- [ ] MQTT パブリッシャー（1秒ごと、≤512bytes）

### Phase 2
- [ ] REST API（OpenAPI 3.0 仕様書）
- [ ] WebSocket通信
- [ ] SQLite DB
- [ ] JWT認証 + TLS

### Phase 3
- [ ] React UI（全画面）
- [ ] ST言語エディタ（Monaco Editor）
- [ ] ラダー図エディタ（React Flow）
- [ ] 監視ダッシュボード

### Phase 4
- [ ] E2Eテスト全成功
- [ ] API仕様書（Swagger UI）
- [ ] インストール・トラブルシューティング・ユーザーマニュアル
- [ ] 負荷テスト結果レポート
