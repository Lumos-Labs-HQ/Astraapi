"""
debug_api.py — Live test: GET /init → POST /searchFlight → POST /searchHotel
Verifies the correct full API flow with all required fields.
Run: .venv\\Scripts\\python.exe debug_api.py
"""
import re, json, time, sys
from datetime import date, timedelta

import requests
from dotenv import load_dotenv
load_dotenv()

BASE_URL = "https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390"
HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"
    ),
    "Accept-Language": "ja,en-US;q=0.7,en;q=0.3",
    "Accept-Encoding": "gzip, deflate, br",
}

# ── search params ──────────────────────────────────────────────────────────
DEPT   = "HND"
ARR    = "ISG"
DIST   = "08"
ADULTS = 2
NIGHTS = 2
start  = date.today() + timedelta(days=30)
GO     = start.strftime("%Y%m%d")
RET    = (start + timedelta(days=NIGHTS)).strftime("%Y%m%d")

_JP_WEEKDAY = ["（月）", "（火）", "（水）", "（木）", "（金）", "（土）", "（日）"]
_JP_MONTH   = ["", "1月","2月","3月","4月","5月","6月","7月","8月","9月","10月","11月","12月"]

def jp_date(yyyymmdd: str) -> str:
    d = date(int(yyyymmdd[:4]), int(yyyymmdd[4:6]), int(yyyymmdd[6:]))
    return f"{_JP_MONTH[d.month]}{d.day}日{_JP_WEEKDAY[d.weekday()]}"

# ──────────────────────────────────────────────────────────────────────────
print(f"Route: {DEPT}→{ARR}  Go: {GO} ({jp_date(GO)})  Return: {RET} ({jp_date(RET)})")
print("=" * 65)

session = requests.Session()
session.headers.update(HEADERS)

# ── Step 1: GET /init ──────────────────────────────────────────────────────
print("\n[1] GET /init ...")
r = session.get(f"{BASE_URL}/init", params={
    "goDeptAirpCd": DEPT, "goArrAirpCd": ARR,
    "goDeptDt": GO, "rtnDeptDt": RET,
    "adultCnt": str(ADULTS), "districtCd": DIST,
    "checkInDt": GO, "checkOutDt": RET,
}, timeout=30)
print(f"  Status:  {r.status_code}")
print(f"  Cookies: {list(session.cookies.keys())}")

m = re.search(r'<meta name="_csrf" content="([^"]+)"', r.text)
if not m:
    print("  ❌ CSRF token NOT found — aborting")
    sys.exit(1)
csrf = m.group(1)
print(f"  CSRF:    {csrf}")

session.headers.update({
    "X-Requested-With": "XMLHttpRequest",
    "X-CSRF-Token":     csrf,
    "Referer":          r.url,
    "Origin":           "https://www.ana.co.jp",
})

# ── Step 2: POST /searchFlight ─────────────────────────────────────────────
print("\n[2] POST /searchFlight ...")
time.sleep(1.5)
form = {
    "depCalTextName_Second": jp_date(GO),
    "goDeptDt":              GO,
    "goDeptAirpCd":          DEPT,
    "goArrAirpCd":           ARR,
    "arrCalTextName_Second": jp_date(RET),
    "rtnDeptDt":             RET,
    "rtnDeptAirpCd":         ARR,    # return departs FROM destination
    "rtnArrAirpCd":          DEPT,   # return arrives TO origin
    "adultCnt":              str(ADULTS),
    "infant2Cnt":            "0",
    "infantCnt":             "0",
    "childADpCnt":           "0",
    "childBDpCnt":           "0",
    "childCDpCnt":           "0",
    "childDDpCnt":           "0",
    "childEDpCnt":           "0",
    "childFDpCnt":           "0",
    "childGDpCnt":           "0",
    "childHDpCnt":           "0",
    "childIDpCnt":           "0",
    "childJDpCnt":           "0",
    "directDispFlg":         "0",
    "_csrf":                 csrf,
    "checkInDt":             GO,
    "checkOutDt":            RET,
}
r2 = session.post(f"{BASE_URL}/searchFlight", data=form, headers={
    "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
    "Accept":       "application/json, */*; q=0.01",
}, timeout=60)

print(f"  Status: {r2.status_code}")
try:
    b2 = r2.json()
    if r2.ok:
        gof = b2.get("goResponse", {}).get("flights", [])
        rtf = b2.get("reResponse", {}).get("flights", [])
        print(f"  ✅ go_flights={len(gof)}  rt_flights={len(rtf)}")
        if gof:
            fs = gof[0].get("flightSet", [{}])[0]
            print(f"     First go:  {fs.get('flightNo')} {fs.get('depTime')}→{fs.get('arvTime')}")
        if rtf:
            fs = rtf[0].get("flightSet", [{}])[0]
            print(f"     First rt:  {fs.get('flightNo')} {fs.get('depTime')}→{fs.get('arvTime')}")
    else:
        errs = [e.get("message", "") for e in b2.get("errors", [])]
        print(f"  ❌ errors: {errs}")
except Exception as ex:
    print(f"  non-JSON: {r2.text[:400]}")

# ── Step 3: POST /searchHotel ──────────────────────────────────────────────
print("\n[3] POST /searchHotel ...")
time.sleep(1.5)
payload = {
    "criteriaCondId":          "",
    "hotelName":               "",
    "coptCd":                  "",
    "faclCd":                  "",
    "coptFaclCd":              "",
    "fromPriceCd":             "",
    "toPriceCd":               "",
    "rcPlanDispFlg":           "0",
    "openStockPlanFlg":        "1",
    "stockLimitsNo":           "",
    "planCd":                  "",
    "districtCd":              DIST,
    "regionCd":                "",
    "areaCd":                  "",
    "depCalTextName_Second_3": jp_date(GO),
    "checkInDt":               GO,
    "arrCalTextName_Second_4": jp_date(RET),
    "checkOutDt":              RET,
    "sortCls":                 "recommend",
    "pageNum":                 "",
    "_csrf":                   csrf,
    "segments":                None,
    "goDeptDt":                GO,
    "reDeptDt":                RET,
}
r3 = session.post(f"{BASE_URL}/searchHotel", json=payload, headers={
    "Content-Type": "application/json; charset=UTF-8",
    "Accept":       "application/json, */*; q=0.01",
}, timeout=60)
print(f"  Status: {r3.status_code}")
try:
    b3 = r3.json()
    if r3.ok:
        page = b3.get("pageInfo", {}).get("page", {})
        hotels = page.get("content", [])
        total  = page.get("totalElements", "?")
        pages  = page.get("totalPages", "?")
        print(f"  ✅ hotels={len(hotels)} / total={total} / pages={pages}")
        if hotels:
            h  = hotels[0]
            pl = h.get("planList", [])
            p  = pl[0] if pl else {}
            pr = p.get("price", {}).get("priceWithAir", {})
            pr_val = pr.get("priceWithAir", "?") if isinstance(pr, dict) else "?"
            fi = p.get("flightInfo", {})
            print(f"     {h.get('faclNam')} | ¥{pr_val:,}" if isinstance(pr_val, int) else
                  f"     {h.get('faclNam')} | {pr_val}")
            print(f"     flight: {fi.get('goFlightNo')} →{fi.get('rtFlightNo')}")
    else:
        errs = [e.get("message", "") for e in b3.get("errors", [])]
        print(f"  ❌ errors: {errs}")
        print(f"  raw: {r3.text[:500]}")
except Exception as ex:
    print(f"  non-JSON: {r3.text[:400]}")

print("\n✅ Done")
