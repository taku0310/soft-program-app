const { PlcConfig } = require("../models");

async function getConfig() {
  const row = await PlcConfig.findOne({ order: [["id", "DESC"]] });
  return row ? row.toJSON() : null;
}

async function updateConfig(payload) {
  const [row] = await PlcConfig.upsert({
    name: payload?.name || "default",
    scanCycleMs: payload?.scanCycleMs ?? 10,
    mqttBroker: payload?.mqttBroker,
    mqttTopicBase: payload?.mqttTopicBase,
    payload,
  });
  return row.toJSON();
}

async function getVariables() {
  /* TODO: read from IPC shared memory */
  return [];
}

module.exports = { getConfig, updateConfig, getVariables };
