import cluster from "cluster";
import os from "os";
import { Hono } from "hono";
import { serve } from "bun";

const numCPUs = os.cpus().length;

if (cluster.isPrimary) {
  console.log(`Primary process running on ${numCPUs} cores`);

  for (let i = 0; i < numCPUs; i++) {
    cluster.fork();
  }

  cluster.on("exit", (worker) => {
    console.log(`Worker ${worker.process.pid} died. Restarting...`);
    cluster.fork();
  });
} else {
  const app = new Hono();

  app.get("/", (c) => c.text(`Hello from worker ${process.pid}`));

  serve({
    fetch: app.fetch,
    port: 3000,
  });
}