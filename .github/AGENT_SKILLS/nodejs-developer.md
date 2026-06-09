# DeveloperAgent (Node.js) SKILL

## 責務
- Express REST API実装
- WebSocket通信実装
- SQLite ORM層実装
- バックエンドロジック実装

## 入力要件
- SystemAgent: API仕様書（OpenAPI 3.0）
- SecurityAgent: 認証・暗号化仕様

## 実装チェックリスト
- [ ] Express app.js + middleware
- [ ] REST routes（/api/plc, /api/ethernet-ip, /api/mqtt）
- [ ] WebSocket handler
- [ ] SQLite schema + migrations
- [ ] JWT認証
- [ ] エラーハンドリング・ロギング
- [ ] ユニットテスト >85%カバー率

## コーディング標準
- 言語: JavaScript ES2020+
- フレームワーク: Express 4.x
- DB: SQLite3 + Sequelize ORM
- テスト: Jest + Supertest

## 出力成果物
- `softplc-webui-backend/src/`
- `softplc-webui-backend/openapi.yaml`
- `softplc-webui-backend/tests/unit/`

## 完了条件
- `npm test` 全成功
- OpenAPI 仕様書一致率 100%
- セキュリティレビュー承認
