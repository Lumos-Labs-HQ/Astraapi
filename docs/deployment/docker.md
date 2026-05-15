# Deploy with Docker

Deploy AstraAPI with Docker for consistent, reproducible production environments. No gunicorn or uvicorn needed — just `python main.py`.

## Basic Dockerfile

```dockerfile
# Build stage
FROM python:3.14-slim as builder

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc cmake \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir --user -r requirements.txt

# Production stage
FROM python:3.14-slim

WORKDIR /app

# Copy installed packages
COPY --from=builder /root/.local /root/.local
ENV PATH=/root/.local/bin:$PATH

# Copy application
COPY . .

EXPOSE 8000

CMD ["python", "main.py"]
```

## Multi-Stage Build (Optimized)

```dockerfile
# Builder stage — compile C++ core
FROM python:3.14-slim as builder

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

COPY pyproject.toml uv.lock ./
COPY cpp_core/ ./cpp_core/
COPY astraapi/ ./astraapi/
COPY scripts/ ./scripts/

RUN pip install --no-cache-dir uv && \
    uv sync --no-dev

# Runtime stage
FROM python:3.14-slim

WORKDIR /app

# Create non-root user
RUN useradd -m -u 1000 astraapi

# Copy only necessary files
COPY --from=builder /app/.venv /app/.venv
COPY --from=builder /app/astraapi /app/astraapi
COPY main.py ./

ENV PATH="/app/.venv/bin:$PATH" \
    PYTHONPATH="/app" \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

USER astraapi

EXPOSE 8000

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://localhost:8000/health')"

CMD ["python", "main.py"]
```

## docker-compose.yml

```yaml
version: '3.8'

services:
  app:
    build: .
    ports:
      - "8000:8000"
    environment:
      - WORKERS=4
    restart: unless-stopped
    deploy:
      resources:
        limits:
          cpus: '4'
          memory: 2G
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8000/health"]
      interval: 30s
      timeout: 10s
      retries: 3

  nginx:
    image: nginx:alpine
    ports:
      - "80:80"
      - "443:443"
    volumes:
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
      - ./static:/var/www/static:ro
    depends_on:
      - app
    restart: unless-stopped
```

## Nginx Reverse Proxy

```nginx
upstream astraapi {
    server app:8000;
    keepalive 64;
}

server {
    listen 80;
    server_name api.example.com;

    location / {
        proxy_pass http://astraapi;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        proxy_connect_timeout 30s;
        proxy_send_timeout 30s;
        proxy_read_timeout 30s;
    }

    location /static/ {
        alias /var/www/static/;
        expires 1y;
        add_header Cache-Control "public, immutable";
    }
}
```

## Build and Run

```bash
# Build image
docker build -t astraapi-app .

# Run container
docker run -d -p 8000:8000 --name astraapi astraapi-app

# With docker-compose
docker-compose up -d
```

## Kubernetes Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: astraapi
spec:
  replicas: 3
  selector:
    matchLabels:
      app: astraapi
  template:
    metadata:
      labels:
        app: astraapi
    spec:
      containers:
      - name: astraapi
        image: astraapi-app:latest
        ports:
        - containerPort: 8000
        env:
        - name: WORKERS
          value: "4"
        resources:
          requests:
            memory: "512Mi"
            cpu: "500m"
          limits:
            memory: "2Gi"
            cpu: "2000m"
        livenessProbe:
          httpGet:
            path: /health
            port: 8000
          initialDelaySeconds: 10
          periodSeconds: 30
        readinessProbe:
          httpGet:
            path: /health
            port: 8000
          initialDelaySeconds: 5
          periodSeconds: 10
---
apiVersion: v1
kind: Service
metadata:
  name: astraapi
spec:
  selector:
    app: astraapi
  ports:
  - port: 80
    targetPort: 8000
  type: LoadBalancer
```

## Environment Variables

```python
import os

workers = int(os.getenv("WORKERS", os.cpu_count() or 1))
port = int(os.getenv("PORT", 8000))
host = os.getenv("HOST", "0.0.0.0")

if __name__ == "__main__":
    app.run(host=host, port=port, workers=workers)
```
