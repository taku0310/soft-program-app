const { sequelize, CipConnection } = require("../../src/models");
const svc = require("../../src/services/ethernet-ip.service");

beforeAll(async () => {
  await sequelize.sync({ force: true });
});

afterEach(async () => {
  await CipConnection.destroy({ where: {} });
});

afterAll(async () => {
  await sequelize.close();
});

const validPayload = {
  instanceId: "0x01",
  deviceIp: "192.168.1.100",
  rpiMs: 10,
  timeoutMs: 30,
  connectionType: "exclusive",
};

test("creates a CIP connection", async () => {
  const row = await svc.create(validPayload);
  expect(row.deviceIp).toBe("192.168.1.100");
});

test("rejects invalid IP", async () => {
  await expect(
    svc.create({ ...validPayload, deviceIp: "999.999.0.1" })
  ).rejects.toThrow(/deviceIp/);
});

test("enforces max 16 connections", async () => {
  for (let i = 0; i < 16; i++) {
    await svc.create({ ...validPayload, deviceIp: `192.168.1.${100 + i}` });
  }
  await expect(svc.create(validPayload)).rejects.toThrow(/maximum/);
});
