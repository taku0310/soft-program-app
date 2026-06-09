# QAAgent SKILL

## 責務
- テスト計画策定
- ユニット・統合テスト自動化
- E2Eテスト実装
- 性能テスト・レポート

## テスト戦略

### Unit Tests
- Target coverage: >85%
- Backend: Jest
- Frontend: React Testing Library

### Integration Tests
- API全体テスト（Supertest + Jest）
- Database transactionテスト
- WebSocket接続テスト

### E2E Tests
- Web UI操作フロー
- プログラム作成→デプロイ→実行
- Framework: Cypress / Playwright

### Performance Tests
- Load: 1000 WebSocket connections
- Throughput: MQTT 1000 msg/sec
- CPU usage <50% (10ms cycle)
- Tool: k6, JMeter

## 実行スケジュール
- Unit: コミット時（自動）
- Integration: PR時（自動）
- E2E: デイリー（20:00）
- Performance: 毎週金曜

## 出力成果物
- `tests/unit/`, `tests/integration/`, `tests/e2e/`, `tests/performance/`
- `test-report.html`

## 成功指標
- Overall coverage >80%
- All tests pass 100%
- Performance baseline jitter ±1ms
