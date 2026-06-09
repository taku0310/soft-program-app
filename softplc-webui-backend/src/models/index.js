const { Sequelize } = require("sequelize");
const dbConfig = require("../../config/database");

const sequelize = new Sequelize(dbConfig);

const PlcConfig = require("./plcConfig.model")(sequelize);
const CipConnection = require("./cipConnection.model")(sequelize);
const Diagnostics = require("./diagnostics.model")(sequelize);

module.exports = { sequelize, PlcConfig, CipConnection, Diagnostics };
