import { createSlice, PayloadAction } from "@reduxjs/toolkit";

export interface UiState {
  activeEditor: "st" | "ladder";
}

const initialState: UiState = {
  activeEditor: "st",
};

const slice = createSlice({
  name: "ui",
  initialState,
  reducers: {
    setActiveEditor(state, action: PayloadAction<"st" | "ladder">) {
      state.activeEditor = action.payload;
    },
  },
});

export const { setActiveEditor } = slice.actions;
export default slice.reducer;
