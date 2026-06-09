import { createSlice, PayloadAction } from "@reduxjs/toolkit";

export interface Diagnostics {
  avgCycleNs: number;
  maxCycleNs: number;
  maxJitterNs: number;
  cycleCount: number;
}

export interface PlcState {
  diagnostics: Diagnostics | null;
  connected: boolean;
}

const initialState: PlcState = {
  diagnostics: null,
  connected: false,
};

const slice = createSlice({
  name: "plc",
  initialState,
  reducers: {
    setConnected(state, action: PayloadAction<boolean>) {
      state.connected = action.payload;
    },
    setDiagnostics(state, action: PayloadAction<Diagnostics>) {
      state.diagnostics = action.payload;
    },
  },
});

export const { setConnected, setDiagnostics } = slice.actions;
export default slice.reducer;
