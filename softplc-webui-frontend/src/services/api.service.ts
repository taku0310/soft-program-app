import axios from "axios";

export const api = axios.create({
  baseURL: "/api",
  timeout: 5000,
});

api.interceptors.request.use((config) => {
  const token = localStorage.getItem("softplc_token");
  if (token) config.headers.Authorization = `Bearer ${token}`;
  return config;
});

export interface CipConnectionDto {
  id?: number;
  instanceId: string;
  deviceIp: string;
  rpiMs: number;
  timeoutMs?: number;
  connectionType?: "exclusive" | "input_only";
  producedSize?: number;
  consumedSize?: number;
  enabled?: boolean;
}

export const plcApi = {
  getConfig: () => api.get("/plc/config").then((r) => r.data),
  updateConfig: (payload: unknown) => api.post("/plc/config", payload).then((r) => r.data),
  getDiagnostics: () => api.get("/plc/diagnostics").then((r) => r.data),
  getLogs: (severity?: string) =>
    api.get("/plc/logs", { params: { severity } }).then((r) => r.data),
};

export const ethernetIpApi = {
  list: () => api.get<CipConnectionDto[]>("/ethernet-ip/devices").then((r) => r.data),
  create: (payload: CipConnectionDto) => api.post("/ethernet-ip/devices", payload).then((r) => r.data),
  update: (id: number, payload: CipConnectionDto) =>
    api.put(`/ethernet-ip/devices/${id}`, payload).then((r) => r.data),
  remove: (id: number) => api.delete(`/ethernet-ip/devices/${id}`),
  test: (id: number) => api.post(`/ethernet-ip/test/${id}`).then((r) => r.data),
};

export const mqttApi = {
  getConfig: () => api.get("/mqtt/config").then((r) => r.data),
  updateConfig: (payload: unknown) => api.post("/mqtt/config", payload).then((r) => r.data),
  listTopics: () => api.get<string[]>("/mqtt/topics").then((r) => r.data),
};
