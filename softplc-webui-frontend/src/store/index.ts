import { configureStore } from "@reduxjs/toolkit";
import plcReducer from "./plcSlice";
import uiReducer from "./uiSlice";

export const store = configureStore({
  reducer: {
    plc: plcReducer,
    ui: uiReducer,
  },
});

export type RootState = ReturnType<typeof store.getState>;
export type AppDispatch = typeof store.dispatch;
