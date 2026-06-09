import { Grid } from "@mui/material";
import CycleAnalyzer from "../components/Monitor/CycleAnalyzer";
import VariablesMonitor from "../components/Monitor/VariablesMonitor";
import ErrorLog from "../components/Monitor/ErrorLog";

export default function MonitorPage() {
  return (
    <Grid container spacing={2}>
      <Grid item xs={12} md={8}>
        <CycleAnalyzer />
      </Grid>
      <Grid item xs={12} md={4}>
        <VariablesMonitor />
      </Grid>
      <Grid item xs={12}>
        <ErrorLog />
      </Grid>
    </Grid>
  );
}
