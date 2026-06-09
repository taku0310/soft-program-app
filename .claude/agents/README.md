# Claude Code Subagents

このディレクトリには、ソフトPLC + Web設定ツール開発のための Claude Code subagent 定義が格納されています。

## エージェント一覧

| エージェント名 | 担当領域 | 主な呼び出しタイミング |
|----------------|----------|------------------------|
| `system-architect` | 全体設計・トレードオフ判断・タスク分解 | 非自明な機能着手前 |
| `plc-runtime-developer` | `softplc-runtime/`（C11, EtherNet/IP, MQTT, リアルタイム） | ランタイム関連の実装 |
| `backend-api-developer` | `softplc-webui-backend/`（Express, Sequelize, WebSocket） | API実装・OpenAPI更新 |
| `frontend-react-developer` | `softplc-webui-frontend/`（React, TS, Monaco, React Flow） | UI実装 |
| `qa-test-engineer` | ユニット・統合テスト追加 | カバレッジ向上、回帰テスト |
| `integration-tester` | E2E、負荷試験、デバイス連携検証 | Phase 4 受入確認 |
| `security-reviewer` | 認証・入力検証・暗号化レビュー | 認証・通信を触る変更 |
| `devops-engineer` | Docker、CI、デプロイ | CI失敗、新サービス追加 |
| `doc-writer` | `docs/` 配下のユーザー向けドキュメント | 機能リリース、仕様変更 |

## 呼び出し方

Claude Code のメインセッションから自動的に委譲されます。明示的に指定したい場合：

```
@plc-runtime-developer CIPアダプタのforward_openを実装してください
```

または Agent ツール経由：

```
Agent({ subagent_type: "backend-api-developer", description: "...", prompt: "..." })
```

## 並列実行

独立した作業は同時に複数エージェントを起動できます。例：

- 同一機能の Backend + Frontend 実装を並列実行
- `security-reviewer` と `qa-test-engineer` を並列で同じ PR に走らせる

## 追加・編集

新しいエージェントは `<name>.md` を作成し、以下の YAML frontmatter を含めます：

```yaml
---
name: agent-name
description: いつ呼ぶか（具体的に書くほど自動委譲が効きやすい）
tools: Read, Edit, Write, Glob, Grep, Bash  # 必要なツールだけ
model: sonnet  # 省略可
---

(システムプロンプト本文)
```

## 関連

- `.github/AGENT_SKILLS/` — プロジェクト全体のエージェント役割定義（プロセス・組織レベル）
- `.github/AGENT_TEAM.md` — チーム全体像
