const express = require("express");
const ctrl = require("../controllers/mqtt.controller");

const router = express.Router();

router.get("/config", ctrl.getConfig);
router.post("/config", ctrl.updateConfig);
router.get("/topics", ctrl.listTopics);

module.exports = router;
