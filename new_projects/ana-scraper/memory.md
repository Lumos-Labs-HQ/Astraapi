# 🧠 ANA Scraper — Project Memory

> This file serves as the persistent memory for this project.
> Always read this file first before starting any work session.

---

## 📌 Project Goal

Build a **Python 3 scraper** that:
1. Reads 255 search task configs (Excel)
2. Fetches ANA Domestic Tour (flight + hotel package) prices via **direct HTTPS requests** (no browser automation)
3. Saves ALL raw results to **MongoDB**
4. Allows post-query filtering by specific flight numbers / hotel names
5. Runs in **Docker** (`docker compose up -d`)

**Deadline:** 2026-03-18 (Monday)

---

## 🌐 Target Site

```
https://www.ana.co.jp/ja/jp/domtour/
```
Product: ANA Travelers Dynamic Package (航空券＋宿泊 = Flight + Hotel)

---

## 🔑 API Endpoints (Reverse-Engineered)

### Base URL
```
https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390/
```

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `init` | GET | Initialize session, get CSRF token |
| `searchHotel` | POST | Get hotel list + prices (**main data**) |
| `getCriteriaList` | POST | Get filter counts |
| `searchFlight` | POST | Get flight options |

---

## 🔐 Authentication Flow

### Step 1 — GET `/init` (Session + CSRF)

```
GET https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390/init
```

Query parameters:

| Param | Description | Example |
|-------|-------------|---------|
| `goDeptAirpCd` | Outbound departure airport | `HND` |
| `goArrAirpCd` | Outbound arrival airport | `CTS` |
| `goDeptDt` | Outbound departure date | `20260415` |
| `rtnDeptDt` | Return departure date | `20260418` |
| `adultCnt` | Number of adults | `2` |
| `districtCd` | Destination district code | `02` |
| `checkInDt` | Hotel check-in (= `goDeptDt`) | `20260415` |
| `checkOutDt` | Hotel check-out (= `rtnDeptDt`) | `20260418` |

Response sets these session cookies (kept automatically by `requests.Session()`):
- `JSESSIONID`
- `ATDCSMSID`
- `ATDSVR` (server affinity: `atd11a`, `atd12a`, etc.)

CSRF token is embedded in the HTML response:
```html
<meta name="_csrf_parameter" content="_csrf" />
<meta name="_csrf_header" content="X-CSRF-TOKEN" />
<meta name="_csrf" content="addcf275-29c6-4978-95df-a6f4a7d6cecb" />
```

Extract with:
```python
import re
csrf = re.search(r'<meta name="_csrf" content="([^"]+)"', html).group(1)
```

---

### Step 2 — POST `/searchHotel` (Main Data)

```
POST https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390/searchHotel
```

Required headers:
```
Content-Type: application/json; charset=UTF-8
X-CSRF-Token: <value from meta tag>
X-Requested-With: XMLHttpRequest
Referer: <URL from init response>
Origin: https://www.ana.co.jp
```

Request body (JSON):
```json
{
  "criteriaCondId": "",
  "hotelName": "",
  "coptCd": "",
  "faclCd": "",
  "districtCd": "02",
  "regionCd": "001",
  "areaCd": "",
  "checkInDt": "20260415",
  "checkOutDt": "20260418",
  "sortCls": "recommend",
  "pageNum": "",
  "_csrf": "addcf275-29c6-4978-95df-a6f4a7d6cecb",
  "goDeptDt": "20260415",
  "reDeptDt": "20260418"
}
```

Response structure:
```json
{
  "pageInfo": {
    "page": {
      "content": [
        {
          "faclCd": "01234",
          "faclNam": "ホテル名",
          "adrs": "住所",
          "planList": [
            {
              "planCd": "EARLY21",
              "planNam": "プラン名",
              "price": {
                "pricePerKind": [
                  {"kindTypeCd": "001", "kindTypeNam": "大人", "kindTypePrice": 48600}
                ],
                "priceWithAir": {"priceWithAir": 97200}
              },
              "flightInfo": {
                "goFlightNo": "NH061",
                "goDepTime": "0630",
                "goArvTime": "0820",
                "rtFlightNo": "NH066",
                "rtDepTime": "1940",
                "rtArvTime": "2125"
              }
            }
          ]
        }
      ],
      "totalElements": 47,
      "totalPages": 3,
      "number": 0
    }
  }
}
```

---

## 🗺️ District Codes

| Code | Area (EN) | Area (JP) |
|------|-----------|-----------|
| `02` | Hokkaido | 北海道 |
| `03` | Tohoku | 東北 |
| `04` | Kanto/Koshinetsu | 関東・甲信越 |
| `05` | Tokai/Hokuriku | 東海・北陸 |
| `06` | Kansai/Kinki | 近畿 |
| `07` | Chugoku/Shikoku | 中国・四国 |
| `08` | Kyushu/Okinawa | 九州・沖縄 |

---

## 🗄️ MongoDB Document Schema (Planned)

Each search result page saved as one document:

```json
{
  "_id": "ObjectId",
  "scraped_at": "2026-03-13T11:00:00Z",
  "task_id": 1,
  "search_params": {
    "goDeptAirpCd": "HND",
    "goArrAirpCd": "CTS",
    "goDeptDt": "20260415",
    "rtnDeptDt": "20260418",
    "adultCnt": 2,
    "districtCd": "02"
  },
  "raw_response": { ... },
  "hotels": [
    {
      "faclCd": "01234",
      "faclNam": "ホテル名",
      "planList": [ ... ]
    }
  ]
}
```

---

## 📁 Project File Structure (Actual)

```
ana-scraper/
├── memory.md               ← This file (always read first)
├── docker-compose.yml      ← MongoDB + scraper service
├── Dockerfile              ← Python scraper container
├── requirements.txt        ← Python dependencies
├── README.md               ← Setup + run instructions
├── test_run.py             ← Quick test/dry-run script (NEW)
├── config/
│   ├── settings.py         ← DB URI, rate limits, etc.
│   └── ◆◆価格比較対象リスト(曜日別).xlsx  ← Client's actual task configs
├── scraper/
│   ├── __init__.py
│   ├── main.py             ← Entry point / task runner
│   ├── ana_client.py       ← HTTP client (init → searchHotel)
│   ├── mongo_client.py     ← MongoDB connection + save logic
│   └── task_loader.py      ← Load Excel tasks (updated 2026-03-14)
├── data/
│   └── sample_response.json  ← Sample data for testing
└── logs/
    └── .gitkeep
```

---

## ⚠️ Implementation Rules (Client Requirements)

- ✅ Python 3.x ONLY
- ✅ Direct HTTPS requests via `requests` library — NO Playwright, Selenium, Puppeteer
- ✅ All results saved to MongoDB (no filtering at scrape time)
- ✅ Docker Compose delivery: `docker compose up -d`
- ✅ 90 days price range per task (150 days for some tasks)
- ✅ Weekly execution by default (bi-weekly for some tasks)
- ✅ Idempotent: re-running same task should not cause data corruption

---

## 🔧 Key Implementation Details

### Rate Limiting
- Add `time.sleep(1.5)` between requests to avoid being blocked
- ANA servers identified (`atd11a`, `atd12a`) — sticky session via `ATDSVR` cookie

### CSRF Handling
- Token changes every session — must re-fetch for each task run
- Token is in `<meta name="_csrf" content="...">` in the init HTML response

### Pagination
- `pageNum` in request body: `""` for page 1, `"2"` for page 2, etc.
- Check `totalPages` in response to know if pagination is needed
- Must loop through all pages to get ALL hotels

### Date Range Loop (90 days example)
```python
from datetime import date, timedelta
start = date.today()
for i in range(90):
    go_date = (start + timedelta(days=i)).strftime("%Y%m%d")
    # ... run search for this date
```

### Idempotency in MongoDB
Use `update_one` with `upsert=True` keyed on `(task_id, goDeptDt)`:
```python
collection.update_one(
    {"task_id": task_id, "search_params.goDeptDt": go_date},
    {"$set": document},
    upsert=True
)
```

---

## 🐳 Docker Setup (Planned)

```yaml
# docker-compose.yml
version: '3.8'
services:
  mongodb:
    image: mongo:7.0
    ports:
      - "27017:27017"
    volumes:
      - mongo_data:/data/db
    environment:
      MONGO_INITDB_DATABASE: ana_scraper

  scraper:
    build: .
    depends_on:
      - mongodb
    environment:
      MONGO_URI: mongodb://mongodb:27017/ana_scraper
    volumes:
      - ./config:/app/config
      - ./logs:/app/logs

volumes:
  mongo_data:
```

---

## 📋 Excel Column Mapping (Actual — 2026-03-14)

File: `◆◆価格比較対象リスト(曜日別).xlsx`, Sheet: `【最新】 20251009`
Header row: **Row 21** | Data rows: **Row 22+** | Active tasks: **~303**

| 0-idx col | JP Header     | Field              | Notes                          |
|-----------|---------------|--------------------|--------------------------------|
| 2         | タスク数       | `task_id`          | sequential int                 |
| 3         | 取得曜日       | `run_weekday`      | 'Sat','Sun','Mon' etc          |
| 5         | JPE/JPW        | `jpe_jpw`          | 'JPE' or 'JPW'                 |
| 7         | 方面           | `destination`      | Area name JP, e.g. '沖縄'      |
| 10        | 取得パターン   | `pattern`          | '高シェア' or '最安値' in text |
| 11        | 往路           | `dept_airport`     | IATA e.g. 'HND'                |
| 12        | 復路           | `arr_airport`      | IATA e.g. 'ISG'                |
| 13        | ANA往路便番号  | `go_flight_filter` | 'ANA' or '最安値'              |
| 14        | (sub)          | `go_flight_num`    | int e.g. 89 → NH0089           |
| 15        | ANA復路便番号  | `rt_flight_filter` | 'ANA' or '最安値'              |
| 16        | (sub)          | `rt_flight_num`    | int                            |
| 17        | 宿泊日数       | `nights`           | 1, 2, or 3                     |
| 18        | 利用人数       | `adults`           | 1 or 2                         |
| 19        | 宿名           | `hotel_name`       | post-filter hotel name         |
| 20        | 旅作実施フラグ | `flag`             | 1=active, skip otherwise       |

---

## 📝 TODO Checklist

- [x] Reverse-engineer ANA API endpoints
- [x] Build `ana_client.py` with session + pagination
- [x] Build `mongo_client.py` with upsert logic
- [x] Build `task_loader.py` — updated to read ACTUAL Excel columns
- [x] Build `main.py` task runner with CLI args
- [x] Create `test_run.py` for dry-run testing
- [ ] Verify end-to-end: `python test_run.py --task-id 1 --dry-run`
- [ ] Verify pagination works across all pages
- [ ] Test MongoDB save: `python test_run.py --task-id 1`
- [ ] Write `Dockerfile` (done skeleton)
- [ ] Write `docker-compose.yml` (done skeleton)
- [ ] Write `README.md`
- [ ] Create sample MongoDB JSON output

---

## 🔬 Research Session Log

| Date | What was done |
|------|--------------|
| 2026-03-13 | Reverse-engineered API via browser network capture. Found 4 endpoints. Confirmed CSRF flow via HTML meta tags. Confirmed session cookie mechanism. Confirmed JSON response structure with `faclNam`, `planList`, `priceWithAir`, `flightInfo`. |
| 2026-03-14 | Received client Excel + CSV sample. Mapped all 23 Excel columns. Rewrote `task_loader.py` (header=row21, data=row22+). Created `test_run.py`. Verified 303 tasks load correctly. |
