import { Paper, Typography } from "@mui/material";

export default function VariablesMonitor() {
  return (
    <Paper sx={{ p: 2, height: "100%" }}>
      <Typography variant="h6">変数モニタ</Typography>
      <Typography color="text.secondary" sx={{ mt: 2 }}>
        WebSocket でストリーミング配信される変数値をここにリスト表示します（実装予定）。
      </Typography>
    </Paper>
  );
}
