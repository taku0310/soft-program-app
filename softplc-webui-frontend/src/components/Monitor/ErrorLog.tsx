import { useEffect, useState } from "react";
import {
  Paper,
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableRow,
  Typography,
} from "@mui/material";
import { plcApi } from "../../services/api.service";

interface LogRow {
  id: number;
  timestamp: string;
  severity: "INFO" | "WARNING" | "ERROR";
  message: string;
}

export default function ErrorLog() {
  const [rows, setRows] = useState<LogRow[]>([]);

  useEffect(() => {
    plcApi.getLogs().then((data) => setRows(data ?? []));
  }, []);

  return (
    <Paper sx={{ p: 2 }}>
      <Typography variant="h6" sx={{ mb: 1 }}>
        エラーログ
      </Typography>
      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>時刻</TableCell>
            <TableCell>Severity</TableCell>
            <TableCell>メッセージ</TableCell>
          </TableRow>
        </TableHead>
        <TableBody>
          {rows.map((r) => (
            <TableRow key={r.id}>
              <TableCell>{new Date(r.timestamp).toLocaleString()}</TableCell>
              <TableCell>{r.severity}</TableCell>
              <TableCell>{r.message}</TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </Paper>
  );
}
