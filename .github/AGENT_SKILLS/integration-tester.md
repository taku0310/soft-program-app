# IntegrationAgent SKILL

## 責務
- End-to-End統合テスト
- システム全体検証
- デバイス連携テスト
- パフォーマンス最終確認

## テスト項目

### E2E Workflow
1. Web UIでプログラム作成（ST + ラダー）
2. PLCにデプロイ
3. EtherNet/IP接続確認
4. デバイスからデータ受信
5. MQTT配信確認
6. ダッシュボード表示確認

### Device Integration
- EtherNet/IP 16デバイス同時接続
- Omron NX5 連携
- FANUC Robot 連携
- デバイス別エラー処理

### Performance Validation
- Cycle time: 10ms ±1ms
- MQTT throughput: 1sec interval
- CPU usage <50%
- Memory leak: 無し

## テスト環境
- Test Docker container（実PLC不要）
- Mock EtherNet/IP devices
- Mock MQTT broker

## 出力成果物
- `tests/integration/`
- `docs/performance-test-report.md`
- `docs/device-compatibility-matrix.md`

## 成功指標
- E2Eテスト全成功
- デバイス連携 100%
- パフォーマンス要件 全達成
