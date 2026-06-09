import { useEffect, useState } from "react";
import {
  Button,
  Paper,
  Stack,
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableRow,
  Typography,
} from "@mui/material";
import { ethernetIpApi, CipConnectionDto } from "../../services/api.service";

export default function EtherNetIPConfig() {
  const [rows, setRows] = useState<CipConnectionDto[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    ethernetIpApi
      .list()
      .then(setRows)
      .catch((e) => setError(e.message));
  }, []);

  return (
    <Paper sx={{ p: 2 }}>
      <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mb: 1 }}>
        <Typography variant="h6">EtherNet/IP 接続</Typography>
        <Button variant="contained" size="small">
          新規追加
        </Button>
      </Stack>
      {error && <Typography color="error">{error}</Typography>}
      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>Instance</TableCell>
            <TableCell>Device IP</TableCell>
            <TableCell>RPI (ms)</TableCell>
            <TableCell>Type</TableCell>
            <TableCell align="right">Test</TableCell>
          </TableRow>
        </TableHead>
        <TableBody>
          {rows.map((r) => (
            <TableRow key={r.id}>
              <TableCell>{r.instanceId}</TableCell>
              <TableCell>{r.deviceIp}</TableCell>
              <TableCell>{r.rpiMs}</TableCell>
              <TableCell>{r.connectionType}</TableCell>
              <TableCell align="right">
                <Button size="small" onClick={() => ethernetIpApi.test(r.id!)}>
                  Test
                </Button>
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </Paper>
  );
}
