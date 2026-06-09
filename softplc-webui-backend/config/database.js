const path = require("path");

module.exports = {
  dialect: "sqlite",
  storage: process.env.DB_PATH || path.join(__dirname, "..", "data", "softplc.sqlite"),
  logging: false,
};
