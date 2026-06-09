export type WsMessageHandler = (event: MessageEvent) => void;

export function connectWebSocket(path = "/ws"): WebSocket {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const url = `${protocol}//${window.location.host}${path}`;
  const socket = new WebSocket(url);
  return socket;
}

export function subscribe(socket: WebSocket, channel: string) {
  const send = () => socket.send(JSON.stringify({ type: "subscribe", channel }));
  if (socket.readyState === WebSocket.OPEN) send();
  else socket.addEventListener("open", send, { once: true });
}
