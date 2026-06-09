const svc = require("../services/mqtt.service");

exports.getConfig = async (_req, res, next) => {
  try { res.json(await svc.getConfig()); } catch (err) { next(err); }
};

exports.updateConfig = async (req, res, next) => {
  try { res.json(await svc.updateConfig(req.body)); } catch (err) { next(err); }
};

exports.listTopics = async (_req, res, next) => {
  try { res.json(await svc.listTopics()); } catch (err) { next(err); }
};
