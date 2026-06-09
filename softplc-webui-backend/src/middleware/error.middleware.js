const logger = require("../utils/logger");

module.exports = function errorMiddleware(err, _req, res, _next) {
  const status = err.status || 500;
  if (status >= 500) logger.error(err.message, { stack: err.stack });
  res.status(status).json({ error: err.message || "internal error" });
};
