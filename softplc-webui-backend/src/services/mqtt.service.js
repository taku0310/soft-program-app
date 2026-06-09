const mqttConfig = require("../../config/mqtt");

let cachedConfig = { ...mqttConfig };

async function getConfig() { return cachedConfig; }

async function updateConfig(payload) {
  cachedConfig = { ...cachedConfig, ...payload };
  return cachedConfig;
}

async function listTopics() {
  /* TODO: read live topic registry */
  return [`${cachedConfig.topicBase}/status`];
}

module.exports = { getConfig, updateConfig, listTopics };
