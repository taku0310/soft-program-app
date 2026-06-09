const { DataTypes } = require("sequelize");

module.exports = (sequelize) =>
  sequelize.define("PlcConfig", {
    id: { type: DataTypes.INTEGER, primaryKey: true, autoIncrement: true },
    name: { type: DataTypes.STRING, allowNull: false, unique: true },
    scanCycleMs: { type: DataTypes.INTEGER, defaultValue: 10 },
    mqttBroker: { type: DataTypes.STRING },
    mqttTopicBase: { type: DataTypes.STRING },
    payload: { type: DataTypes.JSON },
  });
