# ReviewAgent SKILL

## 責務
- コードレビュー
- 設計レビュー
- ベストプラクティス検証
- 技術債検出

## レビュー項目

### Code Quality
- 命名規則の統一性
- 関数複雑度（Cyclomatic Complexity <15）
- DRY原則
- SOLID原則

### Security Review
- 入力検証の完全性
- 暗号化・認証の実装
- SQL Injection対策
- XSS対策

### Documentation
- コメント・docstring
- README更新
- 変更ログ

### Performance
- アルゴリズムの最適性
- メモリリーク可能性
- 不要なループ・再計算

## レビュー方式
- PR作成時に自動実行
- CheckStyle / ESLint
- SonarQube
- Manual Review（複雑なロジック）

## 出力成果物
- PRコメント（GitHub）
- Quality report
- Technical debt backlog

## 成功指標
- PR承認率 95%以上
- 指摘事項 fix率 100%
- 技術債 open items <10
