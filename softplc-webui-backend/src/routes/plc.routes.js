const express = require("express");
const plcController = require("../controllers/plc.controller");

const router = express.Router();

router.get("/config", plcController.getConfig);
router.post("/config", plcController.updateConfig);
router.get("/variables", plcController.getVariables);
router.get("/diagnostics", plcController.getDiagnostics);
router.get("/logs", plcController.getLogs);

module.exports = router;
