# softplc-webui-frontend

React + TypeScript の Web設定ツールUI。

## セットアップ

```bash
npm install
npm run start
```

- 開発サーバー: http://localhost:3000
- バックエンド API へのプロキシは `vite.config.ts` で設定済み

## ビルド

```bash
npm run build
```

## ディレクトリ

```
src/
├── App.tsx
├── index.tsx
├── components/
│   ├── Editor/        # ST + Ladder エディタ
│   ├── Configuration/ # EtherNet/IP + MQTT
│   └── Monitor/       # ダッシュボード
├── pages/             # ルートページ
├── services/          # API / WebSocket クライアント
└── store/             # Redux Toolkit
```

## 主なライブラリ

- `@monaco-editor/react` - ST言語エディタ
- `reactflow` - ラダー図エディタ
- `chart.js` + `react-chartjs-2` - ジッター解析グラフ
- `@reduxjs/toolkit` - 状態管理
- `@mui/material` - UI フレームワーク
