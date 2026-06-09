const http = require("http");
const path = require("path");
const express = require("express");
const helmet = require("helmet");
const cors = require("cors");
const swaggerUi = require("swagger-ui-express");
const YAML = require("yamljs");

const serverConfig = require("../config/server");
const logger = require("./utils/logger");
const { sequelize } = require("./models");
const errorMiddleware = require("./middleware/error.middleware");
const authMiddleware = require("./middleware/auth.middleware");

const plcRoutes = require("./routes/plc.routes");
const ethernetIpRoutes = require("./routes/ethernet-ip.routes");
const mqttRoutes = require("./routes/mqtt.routes");

const { attachWebSocketHandlers } = require("./websocket/ws-handler");

const openApiPath = path.join(__dirname, "..", "openapi.yaml");
const openApiDoc = YAML.load(openApiPath);

const app = express();
app.use(helmet());
app.use(cors({ origin: serverConfig.corsOrigin }));
app.use(express.json({ limit: "1mb" }));

app.get("/health", (_req, res) => res.json({ status: "ok" }));
app.use("/docs", swaggerUi.serve, swaggerUi.setup(openApiDoc));

app.use("/api/plc", authMiddleware, plcRoutes);
app.use("/api/ethernet-ip", authMiddleware, ethernetIpRoutes);
app.use("/api/mqtt", authMiddleware, mqttRoutes);

app.use(errorMiddleware);

async function bootstrap() {
  await sequelize.sync();
  const server = http.createServer(app);
  attachWebSocketHandlers(server);
  server.listen(serverConfig.port, () => {
    logger.info(`backend listening on :${serverConfig.port}`);
  });
}

if (require.main === module) {
  bootstrap().catch((err) => {
    logger.error("bootstrap failed", err);
    process.exit(1);
  });
}

module.exports = { app };
