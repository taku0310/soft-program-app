function isHexString(value, maxBytes = 2) {
  if (typeof value !== "string") return false;
  return new RegExp(`^0x[0-9a-fA-F]{1,${maxBytes * 2}}$`).test(value);
}

function isIpv4(value) {
  if (typeof value !== "string") return false;
  return /^(25[0-5]|2[0-4]\d|[01]?\d\d?)(\.(25[0-5]|2[0-4]\d|[01]?\d\d?)){3}$/.test(value);
}

function assertField(obj, field, validator, message) {
  if (!validator(obj?.[field])) {
    const err = new Error(message || `invalid field: ${field}`);
    err.status = 400;
    throw err;
  }
}

module.exports = { isHexString, isIpv4, assertField };
