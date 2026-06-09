# AIエージェントチーム定義

このプロジェクトは **11体のAIエージェント** + 人間エンジニア5名で運用される。

## エージェント一覧

| # | 名前 | 役割 | SKILL |
|----|------|------|-------|
| 1 | SystemAgent | システムアーキテクト | [system-architect.md](AGENT_SKILLS/system-architect.md) |
| 2 | DeveloperAgent (C/Linux) | C/Linux実装 (PLC Runtime) | [c-linux-developer.md](AGENT_SKILLS/c-linux-developer.md) |
| 3 | DeveloperAgent (Node.js) | バックエンド API実装 | [nodejs-developer.md](AGENT_SKILLS/nodejs-developer.md) |
| 4 | DeveloperAgent (React) | フロントエンド実装 | [react-frontend-dev.md](AGENT_SKILLS/react-frontend-dev.md) |
| 5 | SecurityAgent | セキュリティ専門家 | [security-specialist.md](AGENT_SKILLS/security-specialist.md) |
| 6 | QAAgent | テスト・品質管理 | [qa-automation.md](AGENT_SKILLS/qa-automation.md) |
| 7 | DocAgent | ドキュメンテーション | [documentation-specialist.md](AGENT_SKILLS/documentation-specialist.md) |
| 8 | DevOpsAgent | CI/CD・デプロイ | [devops-engineer.md](AGENT_SKILLS/devops-engineer.md) |
| 9 | ProcessAuditAgent | 品質監査・トレーサビリティ | [process-auditor.md](AGENT_SKILLS/process-auditor.md) |
| 10 | IntegrationAgent | E2E統合テスト | [integration-tester.md](AGENT_SKILLS/integration-tester.md) |
| 11 | ReviewAgent | コード・設計レビュー | [code-reviewer.md](AGENT_SKILLS/code-reviewer.md) |

## 依存関係

```
Phase 1 (Week 3-8)         Phase 2 (Week 9-14)        Phase 3 (Week 9-16)
─────────────────────       ─────────────────────       ─────────────────────
SystemAgent ──────────┬───→ DeveloperAgent (Node.js) ─→ DeveloperAgent (React)
SecurityAgent ────────┤
                      └───→ DeveloperAgent (C/Linux)
                            ↓
                            QAAgent (継続)
                            ReviewAgent (継続)
                            ProcessAuditAgent (継続)

Phase 4 (Week 17-24)
─────────────────────
IntegrationAgent → QAAgent → DocAgent → DevOpsAgent
```

## 通信ルール

- **質問 / ブロッカー:** GitHub Issues に `agent-conflict`, `blocker`, `dependency` ラベル
- **週次同期:** 月曜09:00 - 自動生成 Issue
- **PRレビュー:** ReviewAgent + SecurityAgent + 人間Lead

## 知識共有

`.github/AGENT_KNOWLEDGE/` 配下に各エージェントが学習・パターンを蓄積。
