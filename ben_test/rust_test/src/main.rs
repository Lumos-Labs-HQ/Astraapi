use axum::{
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    routing::get,
    Router,
};
use tokio::net::TcpListener;

async fn hello() -> &'static str {
    "Hello from 2 workers 🚀"
}

async fn ws_handler(ws: WebSocketUpgrade) -> impl axum::response::IntoResponse {
    ws.on_upgrade(handle_socket)
}

async fn handle_socket(mut socket: WebSocket) {
    while let Some(Ok(msg)) = socket.recv().await {
        if let Message::Text(text) = msg {
            let _ = socket.send(Message::Text(text)).await;
        }
    }
}

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let app = Router::new()
        .route("/", get(hello))
        .route("/ws", get(ws_handler));

    let listener = TcpListener::bind("0.0.0.0:4000").await.unwrap();
    println!("Server running with 2 workers");
    axum::serve(listener, app).await.unwrap();
}
