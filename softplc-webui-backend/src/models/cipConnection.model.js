const { DataTypes } = require("sequelize");

module.exports = (sequelize) =>
  sequelize.define("CipConnection", {
    id: { type: DataTypes.INTEGER, primaryKey: true, autoIncrement: true },
    instanceId: { type: DataTypes.STRING, allowNull: false },
    deviceIp: { type: DataTypes.STRING, allowNull: false },
    rpiMs: { type: DataTypes.INTEGER, defaultValue: 10 },
    timeoutMs: { type: DataTypes.INTEGER, defaultValue: 30 },
    connectionType: { type: DataTypes.ENUM("exclusive", "input_only"), defaultValue: "exclusive" },
    producedSize: { type: DataTypes.INTEGER, defaultValue: 64 },
    consumedSize: { type: DataTypes.INTEGER, defaultValue: 64 },
    enabled: { type: DataTypes.BOOLEAN, defaultValue: true },
  });
