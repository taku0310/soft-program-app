import { useState } from "react";
import { Box, Tab, Tabs } from "@mui/material";
import STEditor from "../components/Editor/STEditor";
import LadderEditor from "../components/Editor/LadderEditor";

export default function EditorPage() {
  const [tab, setTab] = useState(0);
  return (
    <Box>
      <Tabs value={tab} onChange={(_, v) => setTab(v)}>
        <Tab label="ST 言語" />
        <Tab label="ラダー図" />
      </Tabs>
      <Box sx={{ mt: 2, height: "calc(100vh - 220px)" }}>
        {tab === 0 ? <STEditor /> : <LadderEditor />}
      </Box>
    </Box>
  );
}
