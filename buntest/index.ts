import { Hono } from "hono";

const app = new Hono();

app.get("/", (c) => {
  return c.text("Hello, World!");
});

Bun.serve({
  port: 3000,
  fetch: app.fetch,
});

console.log("Server is running on http://localhost:3000");