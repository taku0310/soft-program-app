const { CipConnection } = require("../models");
const { isHexString, isIpv4, assertField } = require("../utils/validators");

const MAX_CONNECTIONS = 16;

function validate(payload) {
  assertField(payload, "instanceId", (v) => isHexString(v, 2));
  assertField(payload, "deviceIp", isIpv4);
  assertField(payload, "rpiMs", (v) => Number.isInteger(v) && v >= 1 && v <= 1000);
}

async function list() {
  return (await CipConnection.findAll()).map((r) => r.toJSON());
}

async function create(payload) {
  validate(payload);
  const count = await CipConnection.count();
  if (count >= MAX_CONNECTIONS) {
    const err = new Error(`maximum ${MAX_CONNECTIONS} CIP connections`);
    err.status = 409;
    throw err;
  }
  const row = await CipConnection.create(payload);
  return row.toJSON();
}

async function update(id, payload) {
  validate(payload);
  const row = await CipConnection.findByPk(id);
  if (!row) { const err = new Error("not found"); err.status = 404; throw err; }
  await row.update(payload);
  return row.toJSON();
}

async function remove(id) {
  await CipConnection.destroy({ where: { id } });
}

async function test(id) {
  const row = await CipConnection.findByPk(id);
  if (!row) { const err = new Error("not found"); err.status = 404; throw err; }
  /* TODO: forward to runtime via IPC; return scaffold result */
  return { id, deviceIp: row.deviceIp, status: "ok", latencyMs: 0 };
}

module.exports = { list, create, update, remove, test };
