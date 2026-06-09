const jwt = require("jsonwebtoken");
const serverConfig = require("../../config/server");

module.exports = function authMiddleware(req, res, next) {
  if (process.env.AUTH_DISABLED === "true") return next();

  const header = req.headers.authorization || "";
  const [scheme, token] = header.split(" ");
  if (scheme !== "Bearer" || !token) {
    return res.status(401).json({ error: "missing bearer token" });
  }
  try {
    req.user = jwt.verify(token, serverConfig.jwtSecret);
    return next();
  } catch (err) {
    return res.status(401).json({ error: "invalid token" });
  }
};
