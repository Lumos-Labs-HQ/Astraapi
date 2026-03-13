# ANA Domestic Tour Scraper

Fetches ANA flight+hotel package prices via direct HTTPS requests and stores all results in MongoDB. No browser automation — pure Python `requests`.

---

## Quick Start

### Prerequisites
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) installed and running

### 1. Add your task Excel file
Place the client-provided `tasks.xlsx` in:
```
config/tasks.xlsx
```
> If this file is missing, the scraper falls back to built-in sample tasks.

### 2. Start everything
```bash
docker compose up -d
```
This starts MongoDB and runs the scraper. The scraper will:
- Run all tasks scheduled for today (by weekday setting in Excel)
- Fetch 90 or 150 days of prices per task
- Save ALL results to MongoDB

### 3. Run a specific task
```bash
docker compose run --rm scraper python -m scraper.main --task-id 1
```

### 4. Run all tasks regardless of schedule
```bash
docker compose run --rm scraper python -m scraper.main --all
```

### 5. Dry run (no DB save)
```bash
docker compose run --rm scraper python -m scraper.main --all --dry-run
```

---

## Project Structure

```
ana-scraper/
├── memory.md               ← Full project memory (API details, research notes)
├── docker-compose.yml
├── Dockerfile
├── requirements.txt
├── README.md
├── config/
│   ├── settings.py         ← Environment variable config
│   └── tasks.xlsx          ← 255 search task definitions (client-provided)
├── scraper/
│   ├── __init__.py
│   ├── main.py             ← Entry point
│   ├── ana_client.py       ← HTTPS API client (session + CSRF + pagination)
│   ├── mongo_client.py     ← MongoDB save logic (upsert / idempotent)
│   └── task_loader.py      ← Excel task reader
├── data/
│   └── sample_response.json   ← Sample MongoDB document
└── logs/
    └── scraper.log
```

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MONGO_URI` | `mongodb://localhost:27017/ana_scraper` | MongoDB connection URI |
| `DB_NAME` | `ana_scraper` | Database name |
| `EXCEL_PATH` | `config/tasks.xlsx` | Path to task Excel file |
| `REQUEST_DELAY` | `1.5` | Seconds between HTTP requests |
| `LOG_LEVEL` | `INFO` | Log verbosity |

Set in `docker-compose.yml` or create a `.env` file.

---

## MongoDB Data

**Collection:** `search_results`

**Index:** `(task_id, search_params.goDeptDt)` — unique, enables upsert-safe re-runs.

**Query example** — find cheapest plan for hotel "マリオット" on 2026-04-15:
```javascript
db.search_results.find(
  { "task_id": 1, "search_params.goDeptDt": "20260415" },
  { "hotels": 1 }
)
```

**Filter by flight number** (Python):
```python
from scraper.mongo_client import MongoSaver
saver = MongoSaver("mongodb://localhost:27017/ana_scraper")
results = saver.get_hotels_by_flight(task_id=1, go_date="20260415", flight_no="NH061")
```

---

## Local Dev (without Docker)

```bash
pip install -r requirements.txt
# Start MongoDB locally (or use MongoDB Atlas)
export MONGO_URI=mongodb://localhost:27017/ana_scraper
python -m scraper.main --task-id 1 --dry-run
```

---

## How It Works

1. **GET** `/domtour/booking/csm/search/DSCP0390/init?goDeptAirpCd=HND&...`  
   → Sets session cookies (`JSESSIONID`, `ATDCSMSID`, `ATDSVR`)  
   → Returns HTML with CSRF token in `<meta name="_csrf" content="...">`

2. **POST** `/domtour/booking/csm/search/DSCP0390/searchHotel`  
   → Body: `{districtCd, checkInDt, checkOutDt, _csrf, ...}`  
   → Returns JSON with all hotels + plans + prices + flight info

3. Paginate through all pages (check `totalPages` in response)

4. Save raw result to MongoDB with `upsert` (idempotent)

> See `memory.md` for full API details and research notes.

Next step: When you receive the client's Excel file with the 255 search task conditions, place it at config/tasks.xlsx and re-run!