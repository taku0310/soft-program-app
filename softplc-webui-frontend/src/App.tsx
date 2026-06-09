import { AppBar, Box, Tab, Tabs, Toolbar, Typography } from "@mui/material";
import { Link, Route, Routes, useLocation } from "react-router-dom";

import EditorPage from "./pages/Editor";
import ConfigurationPage from "./pages/Configuration";
import MonitorPage from "./pages/Monitor";
import SettingsPage from "./pages/Settings";

const routes = [
  { path: "/", label: "エディタ" },
  { path: "/configuration", label: "設定" },
  { path: "/monitor", label: "監視" },
  { path: "/settings", label: "その他" },
];

export default function App() {
  const { pathname } = useLocation();
  const activeIndex = Math.max(
    routes.findIndex((r) => r.path === pathname),
    0
  );

  return (
    <Box sx={{ display: "flex", flexDirection: "column", height: "100vh" }}>
      <AppBar position="static" color="default" elevation={1}>
        <Toolbar>
          <Typography variant="h6" sx={{ flexGrow: 1 }}>
            ソフトPLC Web設定ツール
          </Typography>
        </Toolbar>
        <Tabs value={activeIndex}>
          {routes.map((r) => (
            <Tab key={r.path} label={r.label} component={Link} to={r.path} />
          ))}
        </Tabs>
      </AppBar>
      <Box sx={{ flexGrow: 1, overflow: "auto", p: 2 }}>
        <Routes>
          <Route path="/" element={<EditorPage />} />
          <Route path="/configuration" element={<ConfigurationPage />} />
          <Route path="/monitor" element={<MonitorPage />} />
          <Route path="/settings" element={<SettingsPage />} />
        </Routes>
      </Box>
    </Box>
  );
}
