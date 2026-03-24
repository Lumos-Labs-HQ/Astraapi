import { serve } from "bun";

serve({
  port: 3000,
  fetch(req, server) {
    if (server.upgrade(req)) return;
    return new Response("Hello from Bun");
  },
  websocket: {
    message(ws, msg) { ws.send(msg); },
  },
});

console.log("Bun WS server ready on :3000");
