import { useEffect, useRef, useState } from "react";
import { Paper, Typography } from "@mui/material";
import {
  CategoryScale,
  Chart as ChartJS,
  LinearScale,
  LineElement,
  PointElement,
  Title,
  Tooltip,
  Legend,
} from "chart.js";
import { Line } from "react-chartjs-2";

import { connectWebSocket, subscribe } from "../../services/websocket.service";

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend);

interface Sample {
  t: string;
  avgUs: number;
  maxUs: number;
  jitterUs: number;
}

export default function CycleAnalyzer() {
  const [samples, setSamples] = useState<Sample[]>([]);
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    const ws = connectWebSocket();
    wsRef.current = ws;
    subscribe(ws, "diagnostics");
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === "diagnostics" && msg.snapshot) {
          const s = msg.snapshot;
          setSamples((prev) =>
            prev
              .concat({
                t: new Date().toLocaleTimeString(),
                avgUs: Math.round((s.avgCycleNs ?? 0) / 1000),
                maxUs: Math.round((s.maxCycleNs ?? 0) / 1000),
                jitterUs: Math.round((s.maxJitterNs ?? 0) / 1000),
              })
              .slice(-60)
          );
        }
      } catch {
        /* ignore */
      }
    };
    return () => ws.close();
  }, []);

  const data = {
    labels: samples.map((s) => s.t),
    datasets: [
      { label: "avg (μs)", data: samples.map((s) => s.avgUs), borderColor: "#0a66c2" },
      { label: "max (μs)", data: samples.map((s) => s.maxUs), borderColor: "#dc2626" },
      { label: "jitter (μs)", data: samples.map((s) => s.jitterUs), borderColor: "#16a34a" },
    ],
  };

  return (
    <Paper sx={{ p: 2 }}>
      <Typography variant="h6">スキャンサイクル分析</Typography>
      <Line data={data} />
    </Paper>
  );
}
