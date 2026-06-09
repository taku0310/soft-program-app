# SecurityAgent SKILL

## 責務
- セキュリティ仕様書作成
- TLS/認証実装レビュー
- CIP通信の安全性検証
- 脆弱性スキャン

## セキュリティ要件
- HTTPS（TLS 1.3）必須
- Authentication: JWT + bcrypt
- EtherNet/IP: CIP Exclusive接続保護
- MQTT: TLS 1.2以上
- ログ: 全操作の監査ログ

## 検査項目
- [ ] TLS証明書生成・検証
- [ ] パスワード暗号化（bcrypt cost ≥12）
- [ ] JWT token管理（短寿命 + refresh）
- [ ] 全API のinput validation
- [ ] SQL Injection対策（Sequelize parameterized）
- [ ] CORS設定確認
- [ ] 脆弱性スキャン（`npm audit`, OWASP）

## セキュリティポリシー
- デフォルト Deny（ホワイトリスト方式）
- 最小権限の原則
- セキュアデフォルト

## 出力成果物
- `docs/SECURITY_SPEC.md`
- `scripts/generate-certs.sh`
- `docs/SECURITY_CHECKLIST.md`

## 成功指標
- `npm audit`: 脆弱性 0件
- OWASP Top 10: 全項目チェック
- Code review承認率 100%
