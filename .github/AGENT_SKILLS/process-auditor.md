# ProcessAuditAgent SKILL

## 責務
- 工程進捗監視
- トレーサビリティ管理
- 品質メトリクス集計
- リスク監視

## 監視指標

### Schedule
- Phase progress rate
- Critical path
- Risk items

### Quality
- Code coverage by component
- Test execution rate
- Bug fix rate
- Documentation completeness

### Traceability Matrix
- Requirement ID: `REQ-{Phase}-{No}`
- Design Doc: `DES-{Phase}-{No}`
- Implementation: `IMP-{Component}-{No}`
- Test Case: `TST-{Component}-{No}`
- Issue: `#GitHub Issue No`

## 週次レポート
- Phase progress (%)
- Open issues count
- Critical items
- Forecast completion

## 出力成果物
- `TRACEABILITY_MATRIX.md`
- `docs/weekly-audit-report.md`
- `docs/metrics-dashboard.html`
- `docs/risk-register.md`

## 成功指標
- 要件トレーサビリティ 100%
- 工程遅延 0
- リスク検出・対応 100%
