# Soft PLC + Web Configuration Tool

ソフトPLC（Docker + Linux）と Web設定ツール（React + Node.js）の統合プロジェクト。

## アーキテクチャ概要

```
┌─────────────────────────────────────────────────────────┐
│                    Web UI (React)                       │
│              ↓ HTTP + WebSocket ↓                        │
├─────────────────────────────────────────────────────────┤
│           Backend API (Node.js / Express)               │
│              ↓ Shared Memory (IPC) ↓                     │
├─────────────────────────────────────────────────────────┤
│         Soft PLC Runtime (Linux + C)                    │
│              ↓ Ethernet ↓                                │
├─────────────────────────────────────────────────────────┤
│    Network (工場内 LAN) - EtherNet/IP + MQTT             │
└─────────────────────────────────────────────────────────┘
```

## コンポーネント

| ディレクトリ | 説明 | 言語 |
|--------------|------|------|
| `softplc-runtime/` | ソフトPLC本体（10msスキャンサイクル, EtherNet/IP, MQTT） | C11 |
| `softplc-webui-backend/` | REST API + WebSocket サーバー | Node.js / Express |
| `softplc-webui-frontend/` | Web設定ツール UI | React + TypeScript |
| `docs/` | プロジェクト仕様、API仕様、マニュアル | Markdown |
| `.github/AGENT_SKILLS/` | AIエージェントチーム定義（11体） | Markdown |

## クイックスタート

```bash
docker-compose up --build
```

- Web UI: http://localhost:3000
- Backend API: http://localhost:4000
- API Docs (Swagger): http://localhost:4000/docs

## 開発

各コンポーネントの詳細は配下の `README.md` を参照。

- [Project Specification](docs/PROJECT_SPEC.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Agent Team](.github/AGENT_TEAM.md)
