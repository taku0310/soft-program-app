import { Grid } from "@mui/material";
import EtherNetIPConfig from "../components/Configuration/EtherNetIPConfig";
import MQTTConfig from "../components/Configuration/MQTTConfig";

export default function ConfigurationPage() {
  return (
    <Grid container spacing={2}>
      <Grid item xs={12} md={7}>
        <EtherNetIPConfig />
      </Grid>
      <Grid item xs={12} md={5}>
        <MQTTConfig />
      </Grid>
    </Grid>
  );
}
