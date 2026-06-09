const { WebSocketServer } = require("ws");
const logger = require("../utils/logger");
const diagnosticsService = require("../services/diagnostics.service");

function attachWebSocketHandlers(server) {
  const wss = new WebSocketServer({ server, path: "/ws" });

  wss.on("connection", (socket) => {
    logger.info("ws client connected");
    socket.on("message", (raw) => {
      try {
        const msg = JSON.parse(raw.toString());
        if (msg.type === "subscribe" && msg.channel === "diagnostics") {
          startDiagnosticsStream(socket);
        }
      } catch (err) {
        socket.send(JSON.stringify({ type: "error", error: err.message }));
      }
    });
    socket.on("close", () => logger.info("ws client disconnected"));
  });

  return wss;
}

function startDiagnosticsStream(socket) {
  const interval = setInterval(async () => {
    if (socket.readyState !== socket.OPEN) {
      clearInterval(interval);
      return;
    }
    const snapshot = await diagnosticsService.snapshot();
    socket.send(JSON.stringify({ type: "diagnostics", snapshot }));
  }, 1000);
  socket.on("close", () => clearInterval(interval));
}

module.exports = { attachWebSocketHandlers };
