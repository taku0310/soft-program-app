const { Diagnostics } = require("../models");

async function snapshot() {
  /* TODO: read from IPC. Scaffold returns the latest persisted row. */
  const row = await Diagnostics.findOne({ order: [["timestamp", "DESC"]] });
  return row ? row.toJSON() : null;
}

async function listLogs(query = {}) {
  const where = {};
  if (query.severity) where.severity = query.severity;
  const rows = await Diagnostics.findAll({
    where,
    order: [["timestamp", "DESC"]],
    limit: Math.min(Number(query.limit) || 100, 1000),
  });
  return rows.map((r) => r.toJSON());
}

module.exports = { snapshot, listLogs };
