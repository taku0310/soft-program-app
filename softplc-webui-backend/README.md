# softplc-webui-backend

REST API + WebSocket サーバー（Node.js / Express / SQLite）。

## セットアップ

```bash
npm install
npm run dev
```

- API: http://localhost:4000
- Swagger UI: http://localhost:4000/docs
- WebSocket: ws://localhost:4000/ws

## 環境変数

| 変数 | デフォルト | 説明 |
|------|-----------|------|
| `PORT` | `4000` | HTTP listen port |
| `DB_PATH` | `./data/softplc.sqlite` | SQLite file |
| `JWT_SECRET` | `dev-secret-change-me` | JWT署名秘密鍵 |
| `MQTT_BROKER_URL` | `mqtt://localhost:1883` | MQTT broker |
| `CORS_ORIGIN` | `http://localhost:3000` | CORS許容オリジン |
| `AUTH_DISABLED` | `false` | 開発用にJWT検証を無効化 |

## テスト

```bash
npm test
```
