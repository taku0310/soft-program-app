# DeveloperAgent (C/Linux) SKILL

## 責務
- PLC Runtime実装 (C言語)
- EtherNet/IP CIPスタック開発
- スケジューラ・リアルタイム実装
- パフォーマンス最適化

## 入力要件
- SystemAgent: Phase仕様書
- API: CIP message formats

## 実装チェックリスト
- [ ] Dockerfile（PREEMPT_RT対応）
- [ ] CIP Adapter（16デバイス対応）
- [ ] Connection Manager
- [ ] Scheduler（10ms ±1ms）
- [ ] MQTT Publisher
- [ ] ユニットテスト >80%カバー率

## コーディング標準
- 言語: C11
- スタイル: Linux kernel style（`checkpatch.pl`）
- エラーハンドリング: errno + custom codes
- メモリ管理: `mlockall` + static allocation
- マルチスレッド: pthread + CPU affinity

## 出力成果物
- `softplc-runtime/src/`
- `softplc-runtime/Dockerfile`
- `softplc-runtime/CMakeLists.txt`
- `softplc-runtime/tests/`

## 完了条件
- `make` 成功
- `cyclictest`: jitter ±1ms以内
- CIP疎通テスト成功
