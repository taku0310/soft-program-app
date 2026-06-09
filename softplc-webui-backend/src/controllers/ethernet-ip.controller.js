const svc = require("../services/ethernet-ip.service");

exports.list = async (_req, res, next) => {
  try { res.json(await svc.list()); } catch (err) { next(err); }
};

exports.create = async (req, res, next) => {
  try { res.status(201).json(await svc.create(req.body)); } catch (err) { next(err); }
};

exports.update = async (req, res, next) => {
  try { res.json(await svc.update(req.params.id, req.body)); } catch (err) { next(err); }
};

exports.remove = async (req, res, next) => {
  try { await svc.remove(req.params.id); res.status(204).end(); } catch (err) { next(err); }
};

exports.test = async (req, res, next) => {
  try { res.json(await svc.test(req.params.id)); } catch (err) { next(err); }
};
