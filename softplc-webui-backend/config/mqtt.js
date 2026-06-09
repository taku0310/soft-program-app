module.exports = {
  brokerUrl: process.env.MQTT_BROKER_URL || "mqtt://localhost:1883",
  clientId: process.env.MQTT_CLIENT_ID || `softplc-backend-${process.pid}`,
  topicBase: process.env.MQTT_TOPIC_BASE || "softplc/devices",
};
