const { DataTypes } = require("sequelize");

module.exports = (sequelize) =>
  sequelize.define("Diagnostics", {
    id: { type: DataTypes.INTEGER, primaryKey: true, autoIncrement: true },
    timestamp: { type: DataTypes.DATE, defaultValue: DataTypes.NOW },
    avgCycleNs: { type: DataTypes.BIGINT },
    maxCycleNs: { type: DataTypes.BIGINT },
    maxJitterNs: { type: DataTypes.BIGINT },
    cycleCount: { type: DataTypes.BIGINT },
    severity: { type: DataTypes.ENUM("INFO", "WARNING", "ERROR"), defaultValue: "INFO" },
    message: { type: DataTypes.STRING },
  });
