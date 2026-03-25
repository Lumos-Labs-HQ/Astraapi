import ws from "k6/ws";
import { check, sleep } from "k6";

export const options = {
  scenarios: {
    websocket_test: {
      executor: "ramping-vus",
      startVUs: 10,
      stages: [
        { duration: "30s", target: 100 },
        { duration: "30s", target: 500 },
        { duration: "30s", target: 1000 },
        { duration: "30s", target: 2000 },
        { duration: "30s", target: 5000 },
      ],
      gracefulRampDown: "30s",
    },
  },
};

export default function () {
  const url = "ws://localhost:8002/ws";

  const res = ws.connect(url, {}, function (socket) {
    socket.on("open", () => {
      socket.send("ping");
    });

    socket.on("message", (data) => {
      // optional message handling
    });

    socket.on("close", () => {
      // connection closed
    });

    socket.on("error", (e) => {
      console.log("error:", e.error());
    });

    // keep connection open
    sleep(10);
  });

  check(res, { "status is 101": (r) => r && r.status === 101 });
}
