# システムアーキテクチャ

## 全体構成

```
┌─────────────────────────────────────────────────────────┐
│                    Web UI (React)                       │
│  ┌─────────────┬──────────────┬──────────────┐         │
│  │ ST Editor   │ Ladder Editor│  Dashboard   │         │
│  └─────────────┴──────────────┴──────────────┘         │
│              ↓ HTTP + WebSocket ↓                        │
├─────────────────────────────────────────────────────────┤
│           Backend API (Node.js / Express)               │
│  ┌────────────────────────────────────────────┐         │
│  │ REST + WebSocket                           │         │
│  │  /api/plc/*       (PLC制御)               │         │
│  │  /api/ethernet-ip/* (CIP管理)            │         │
│  │  /api/mqtt/*      (MQTT設定)              │         │
│  └────────────────────────────────────────────┘         │
│              ↓ Shared Memory (IPC) ↓                     │
├─────────────────────────────────────────────────────────┤
│         Soft PLC Runtime (Linux + C)                    │
│  ┌──────────────────────────────────────────┐           │
│  │ Main Scheduler (10ms cycle)              │           │
│  │  ├─ Input Sampling (EtherNet/IP RX)     │           │
│  │  ├─ Logic Execution (IEC 61131-3)       │           │
│  │  ├─ Output Sync (CIP TX)                │           │
│  │  └─ MQTT Publishing (1s interval)       │           │
│  └──────────────────────────────────────────┘           │
│              ↓ Ethernet ↓                                │
├─────────────────────────────────────────────────────────┤
│  EtherNet/IP Devices (16)    │ MQTT Broker (1883)       │
└─────────────────────────────────────────────────────────┘
```

## レイヤーの責務

### Runtime層（C / Linux）
- リアルタイム制御（PREEMPT_RT, CPU affinity, mlockall）
- EtherNet/IP CIPスタック（exclusive owner connection）
- MQTT パブリッシュ
- ロジック実行（コンパイル済みSTバイトコード or ladder IR）
- 共有メモリでバックエンドと変数交換

### Backend層（Node.js）
- REST API（設定取得・更新、デバイス管理）
- WebSocketによるリアルタイムストリーム
- SQLite による設定・履歴永続化
- 認証 / 認可（JWT, bcrypt）
- Runtime IPC との橋渡し

### Frontend層（React + TypeScript）
- ST / Ladder のエディタ
- 設定 UI（EtherNet/IP, MQTT）
- 監視ダッシュボード
- WebSocket クライアント

## IPC 設計

Backend ↔ Runtime は `/dev/shm/softplc` の共有メモリで連携：

```
SoftPLCSharedRegion {
  uint64_t cycle_count;
  uint64_t last_cycle_us;
  uint64_t max_jitter_us;
  uint32_t variable_count;
  Variable variables[MAX_VARS];
  RingBuffer log_ring;
}
```

書き込みはRuntime（生産者）、読み取りはBackend（消費者）。命令配信は逆方向の `command_ring`。

## セキュリティ境界

- 工場LAN：信頼境界。HTTPS + TLS 1.3 は外向け（リモート保守）に必須
- EtherNet/IP：Exclusive Owner で接続制限
- MQTT：TLS 1.2+、ACL でトピック制限
- 全API：JWT必須（健康チェックを除く）
