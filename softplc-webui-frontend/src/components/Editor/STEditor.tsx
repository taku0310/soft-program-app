import { useState } from "react";
import { Box, Button, Stack } from "@mui/material";
import Editor from "@monaco-editor/react";

const DEFAULT_PROGRAM = `PROGRAM MainControl
VAR
  counter : INT := 0;
END_VAR

  counter := counter + 1;

END_PROGRAM
`;

export default function STEditor() {
  const [source, setSource] = useState(DEFAULT_PROGRAM);

  return (
    <Stack spacing={1} sx={{ height: "100%" }}>
      <Box sx={{ flexGrow: 1, border: "1px solid #ddd" }}>
        <Editor
          height="100%"
          defaultLanguage="pascal"
          theme="vs-light"
          value={source}
          onChange={(value) => setSource(value ?? "")}
          options={{ minimap: { enabled: false }, fontSize: 14 }}
        />
      </Box>
      <Stack direction="row" spacing={1}>
        <Button variant="outlined">Save</Button>
        <Button variant="contained">Deploy</Button>
        <Button color="success" variant="contained">
          Run
        </Button>
        <Button color="warning">Stop</Button>
      </Stack>
    </Stack>
  );
}
