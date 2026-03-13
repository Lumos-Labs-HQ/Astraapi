"""
Global settings — loaded from environment variables.
Copy .env.example to .env and fill in values for local dev.
"""

import os


MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017/ana_scraper")
DB_NAME = os.getenv("DB_NAME", "ana_scraper")
EXCEL_PATH = os.getenv("EXCEL_PATH", "config/tasks.xlsx")

# Seconds to sleep between HTTP requests (be nice to ANA servers)
REQUEST_DELAY = float(os.getenv("REQUEST_DELAY", "1.5"))

# How many consecutive errors before a task is aborted
MAX_ERRORS_PER_TASK = int(os.getenv("MAX_ERRORS_PER_TASK", "5"))

# Log level: DEBUG | INFO | WARNING | ERROR
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO")
