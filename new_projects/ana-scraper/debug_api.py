"""
debug_api.py — Clean 4-step ANA API flow test (confirmed working 2026-03-14)
Flow: GET /init → POST /searchFlight → POST /getCriteriaList → POST /searchHotel
Run: .venv\\Scripts\\python.exe debug_api.py
"""
import re, json, time
from datetime import date, timedelta
import requests
from dotenv import load_dotenv
load_dotenv()

BASE = "https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390"
HDRS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/122.0.0.0 Safari/537.36",
    "Accept-Language": "ja,en-US;q=0.7,en;q=0.3",
    "Accept-Encoding": "gzip, deflate, br",
}
_JWD = ["（月）","（火）","（水）","（木）","（金）","（土）","（日）"]
_JM  = ["","1月","2月","3月","4月","5月","6月","7月","8月","9月","10月","11月","12月"]

def jp(s):
    d = date(int(s[:4]), int(s[4:6]), int(s[6:]))
    return f"{_JM[d.month]}{d.day}日{_JWD[d.weekday()]}"

# ── params (change these to test different routes) ──────────────────
DEPT, ARR, DIST, ADULTS, NIGHTS = "HND", "CTS", "02", 2, 1
start = date.today() + timedelta(days=30)
GO  = start.strftime("%Y%m%d")
RET = (start + timedelta(days=NIGHTS)).strftime("%Y%m%d")
print(f"Route: {DEPT}→{ARR}  Go={GO} ({jp(GO)})  Return={RET} ({jp(RET)})\n")

s = requests.Session()
s.headers.update(HDRS)

# ─── Step 1: GET /init ────────────────────────────────────────────────
print("[1] GET /init...")
r1 = s.get(f"{BASE}/init", params={
    "goDeptAirpCd": DEPT, "goArrAirpCd": ARR,
    "goDeptDt": GO, "rtnDeptDt": RET,
    "adultCnt": str(ADULTS), "districtCd": DIST,
    "checkInDt": GO, "checkOutDt": RET,
}, timeout=30)
csrf = re.search(r'<meta name="_csrf" content="([^"]+)"', r1.text).group(1)
print(f"  status={r1.status_code}  CSRF={csrf[:10]}...  cookies={list(s.cookies.keys())}")
s.headers.update({"X-Requested-With":"XMLHttpRequest","X-CSRF-Token":csrf,"Referer":r1.url,"Origin":"https://www.ana.co.jp"})

# ─── Step 2: POST /searchFlight ────────────────────────────────────────
time.sleep(1.5)
print("\n[2] POST /searchFlight...")
r2 = s.post(f"{BASE}/searchFlight", data={
    "depCalTextName_Second": jp(GO), "goDeptDt": GO,
    "goDeptAirpCd": DEPT, "goArrAirpCd": ARR,
    "arrCalTextName_Second": jp(RET), "rtnDeptDt": RET,
    "rtnDeptAirpCd": ARR, "rtnArrAirpCd": DEPT,
    "adultCnt": str(ADULTS), "infant2Cnt":"0","infantCnt":"0",
    "childADpCnt":"0","childBDpCnt":"0","childCDpCnt":"0","childDDpCnt":"0",
    "childEDpCnt":"0","childFDpCnt":"0","childGDpCnt":"0","childHDpCnt":"0",
    "childIDpCnt":"0","childJDpCnt":"0","directDispFlg":"0",
    "_csrf": csrf, "checkInDt": GO, "checkOutDt": RET,
}, headers={"Content-Type":"application/x-www-form-urlencoded; charset=UTF-8","Accept":"application/json, */*"}, timeout=60)
fd = r2.json()
gof = fd.get("goResponse",{}).get("flights",[])
rtf = fd.get("reResponse",{}).get("flights",[])
print(f"  status={r2.status_code}  go_flights={len(gof)}  rt_flights={len(rtf)}")
if gof:
    fs = gof[0].get("flightSet",[{}])[0]
    print(f"  First go: NH{fs.get('flight','')} {fs.get('depTime')}→{fs.get('arrTime')}")

# ─── Step 3: POST /getCriteriaList (set flight in session) ─────────────
time.sleep(1.5)
print("\n[3] POST /getCriteriaList (set flight selection in server session)...")
r3 = s.post(f"{BASE}/getCriteriaList", json={
    "criteriaCondId":"","hotelName":"","coptCd":"","faclCd":"","coptFaclCd":"",
    "fromPriceCd":"","toPriceCd":"","rcPlanDispFlg":"0","openStockPlanFlg":"1",
    "stockLimitsNo":"","planCd":"",
    "districtCd":DIST,"regionCd":"","areaCd":"",
    "checkInDt":GO,"checkOutDt":RET,
    "_csrf":csrf,"segments":None,"goDeptDt":GO,"reDeptDt":RET,
    "goFlightInfo":  gof[0] if gof else None,   # ← REQUIRED: sets server session
    "rtnFlightInfo": rtf[0] if rtf else None,   # ← REQUIRED: sets server session
}, headers={"Content-Type":"application/json; charset=UTF-8","Accept":"application/json, */*"}, timeout=30)
print(f"  status={r3.status_code}  keys={list(r3.json().keys()) if r3.ok else 'ERROR'}")

# ─── Step 4: POST /searchHotel ─────────────────────────────────────────
time.sleep(1.5)
print("\n[4] POST /searchHotel...")
r4 = s.post(f"{BASE}/searchHotel", json={
    "criteriaCondId":"","hotelName":"","coptCd":"","faclCd":"","coptFaclCd":"",
    "fromPriceCd":"","toPriceCd":"","rcPlanDispFlg":"0","openStockPlanFlg":"1",
    "stockLimitsNo":"","planCd":"",
    "districtCd":DIST,"regionCd":"","areaCd":"",
    "depCalTextName_Second_3":jp(GO),"checkInDt":GO,
    "arrCalTextName_Second_4":jp(RET),"checkOutDt":RET,
    "sortCls":"recommend","pageNum":"",
    "_csrf":csrf,"segments":None,"goDeptDt":GO,"reDeptDt":RET,
}, headers={"Content-Type":"application/json; charset=UTF-8","Accept":"application/json, */*"}, timeout=60)
print(f"  status={r4.status_code}")
if r4.ok:
    page = r4.json().get("pageInfo",{}).get("page",{})
    hotels = page.get("content",[])
    print(f"  ✅ hotels={len(hotels)}  total={page.get('totalElements')}  pages={page.get('totalPages')}")
    if hotels:
        h = hotels[0]; pl = h.get("planList",[]); p = pl[0] if pl else {}
        pr = p.get("price",{}).get("priceWithAir",{})
        pv = pr.get("priceWithAir","?") if isinstance(pr,dict) else "?"
        fi = p.get("flightInfo",{})
        print(f"  Hotel: {h.get('faclNam')}")
        print(f"  Plan:  {'¥{:,}'.format(pv) if isinstance(pv,int) else pv}")
        print(f"  Go/Rt: {fi.get('goFlightNo')} / {fi.get('rtFlightNo')}")
else:
    errs = [e.get("message","") for e in r4.json().get("errors",[])]
    print(f"  ❌ {errs}")

print("\n✅ Done")
