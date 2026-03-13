FROM python:3.11-slim

WORKDIR /app

# Install dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source
COPY scraper/ ./scraper/

# Create directories
RUN mkdir -p config logs

# Run as non-root
RUN useradd -m scraper && chown -R scraper:scraper /app
USER scraper

CMD ["python", "-m", "scraper.main", "--all"]
