import { Paper, Typography } from "@mui/material";

export default function SettingsPage() {
  return (
    <Paper sx={{ p: 3 }}>
      <Typography variant="h5">その他の設定</Typography>
      <Typography sx={{ mt: 2 }} color="text.secondary">
        バージョン管理（Git連携）、ユーザー管理、表示設定などをここに実装します。
      </Typography>
    </Paper>
  );
}
