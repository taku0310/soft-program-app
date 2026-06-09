import { useEffect, useState } from "react";
import { Button, Paper, Stack, TextField, Typography } from "@mui/material";
import { mqttApi } from "../../services/api.service";

interface MqttConfigState {
  brokerUrl: string;
  topicBase: string;
}

export default function MQTTConfig() {
  const [config, setConfig] = useState<MqttConfigState>({ brokerUrl: "", topicBase: "" });

  useEffect(() => {
    mqttApi.getConfig().then((cfg) => setConfig({
      brokerUrl: cfg?.brokerUrl ?? "",
      topicBase: cfg?.topicBase ?? "",
    }));
  }, []);

  return (
    <Paper sx={{ p: 2 }}>
      <Typography variant="h6" sx={{ mb: 1 }}>MQTT 設定</Typography>
      <Stack spacing={2}>
        <TextField
          label="Broker URL"
          value={config.brokerUrl}
          onChange={(e) => setConfig({ ...config, brokerUrl: e.target.value })}
          fullWidth
        />
        <TextField
          label="Topic Base"
          value={config.topicBase}
          onChange={(e) => setConfig({ ...config, topicBase: e.target.value })}
          fullWidth
        />
        <Button variant="contained" onClick={() => mqttApi.updateConfig(config)}>
          保存
        </Button>
      </Stack>
    </Paper>
  );
}
