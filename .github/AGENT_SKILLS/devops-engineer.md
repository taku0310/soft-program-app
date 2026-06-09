# DevOpsAgent SKILL

## 責務
- Docker化
- CI/CDパイプライン構築
- デプロイメント自動化
- 監視・ロギング設定

## 実装項目
- [ ] Dockerfile最適化（マルチステージビルド）
- [ ] docker-compose.yml（Dev/Staging/Prod）
- [ ] GitHub Actions workflow
- [ ] Secret管理（環境変数）
- [ ] ログ集約

## CI/CDパイプライン

### On: push to main
1. Build images
2. Run unit / integration / E2E tests
3. Security scan（`npm audit`, Trivy）
4. Build docs
5. Deploy to staging

### On: pull request
1. Lint
2. Unit tests
3. Coverage report
4. Security preview

## デプロイメント
- Staging: 自動（main push）
- Production: 手動（タグ作成）

## 出力成果物
- `softplc-runtime/Dockerfile`
- `softplc-webui-backend/Dockerfile`
- `softplc-webui-frontend/Dockerfile`
- `docker-compose.yml`
- `.github/workflows/*.yml`

## 成功指標
- CI実行時間 <15分
- デプロイ時間 <5分
- ビルド成功率 100%
