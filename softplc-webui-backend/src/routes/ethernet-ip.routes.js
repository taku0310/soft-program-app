const express = require("express");
const ctrl = require("../controllers/ethernet-ip.controller");

const router = express.Router();

router.get("/devices", ctrl.list);
router.post("/devices", ctrl.create);
router.put("/devices/:id", ctrl.update);
router.delete("/devices/:id", ctrl.remove);
router.post("/test/:id", ctrl.test);

module.exports = router;
