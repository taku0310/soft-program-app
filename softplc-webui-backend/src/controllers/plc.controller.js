const plcService = require("../services/plc.service");
const diagnosticsService = require("../services/diagnostics.service");

exports.getConfig = async (_req, res, next) => {
  try {
    res.json(await plcService.getConfig());
  } catch (err) { next(err); }
};

exports.updateConfig = async (req, res, next) => {
  try {
    res.json(await plcService.updateConfig(req.body));
  } catch (err) { next(err); }
};

exports.getVariables = async (_req, res, next) => {
  try {
    res.json(await plcService.getVariables());
  } catch (err) { next(err); }
};

exports.getDiagnostics = async (_req, res, next) => {
  try {
    res.json(await diagnosticsService.snapshot());
  } catch (err) { next(err); }
};

exports.getLogs = async (req, res, next) => {
  try {
    res.json(await diagnosticsService.listLogs(req.query));
  } catch (err) { next(err); }
};
