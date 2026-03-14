"""
inspect_flight_resp.py — Print the FULL searchFlight response to see flight object structure.
Run: .venv\\Scripts\\python.exe inspect_flight_resp.py
"""
import re, json, time
from datetime import date, timedelta
import requests
from dotenv import load_dotenv
load_dotenv()

BASE = "https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390"
HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/122.0.0.0 Safari/537.36",
    "Accept-Language": "ja,en-US;q=0.7,en;q=0.3",
    "Accept-Encoding": "gzip, deflate, br",
}
_JP_WD = ["（月）","（火）","（水）","（木）","（金）","（土）","（日）"]
_JP_M  = ["","1月","2月","3月","4月","5月","6月","7月","8月","9月","10月","11月","12月"]

def jp(s):
    d = date(int(s[:4]), int(s[4:6]), int(s[6:]))
    return f"{_JP_M[d.month]}{d.day}日{_JP_WD[d.weekday()]}"

DEPT="HND"; ARR="ISG"; DIST="08"; ADULTS=2; NIGHTS=2
start = date.today() + timedelta(days=30)
GO  = start.strftime("%Y%m%d")
RET = (start + timedelta(days=NIGHTS)).strftime("%Y%m%d")

s = requests.Session()
s.headers.update(HEADERS)

# 1. init
r = s.get(f"{BASE}/init", params={
    "goDeptAirpCd":DEPT,"goArrAirpCd":ARR,"goDeptDt":GO,"rtnDeptDt":RET,
    "adultCnt":str(ADULTS),"districtCd":DIST,"checkInDt":GO,"checkOutDt":RET,
}, timeout=30)
csrf = re.search(r'<meta name="_csrf" content="([^"]+)"', r.text).group(1)
s.headers.update({"X-Requested-With":"XMLHttpRequest","X-CSRF-Token":csrf,"Referer":r.url,"Origin":"https://www.ana.co.jp"})
print(f"init OK. CSRF={csrf[:12]}...")

# 2. searchFlight
time.sleep(1.5)
r2 = s.post(f"{BASE}/searchFlight", data={
    "depCalTextName_Second":jp(GO),"goDeptDt":GO,"goDeptAirpCd":DEPT,"goArrAirpCd":ARR,
    "arrCalTextName_Second":jp(RET),"rtnDeptDt":RET,"rtnDeptAirpCd":ARR,"rtnArrAirpCd":DEPT,
    "adultCnt":str(ADULTS),"infant2Cnt":"0","infantCnt":"0",
    "childADpCnt":"0","childBDpCnt":"0","childCDpCnt":"0","childDDpCnt":"0",
    "childEDpCnt":"0","childFDpCnt":"0","childGDpCnt":"0","childHDpCnt":"0",
    "childIDpCnt":"0","childJDpCnt":"0","directDispFlg":"0",
    "_csrf":csrf,"checkInDt":GO,"checkOutDt":RET,
}, headers={"Content-Type":"application/x-www-form-urlencoded; charset=UTF-8","Accept":"application/json, */*"}, timeout=60)

data = r2.json()
gof = data.get("goResponse",{}).get("flights",[])
rtf = data.get("reResponse",{}).get("flights",[])
print(f"searchFlight OK: {len(gof)} go flights, {len(rtf)} rt flights")

# Print FULL structure of first go flight
print("\n=== FULL goFlights[0] structure ===")
print(json.dumps(gof[0] if gof else {}, ensure_ascii=False, indent=2))
print("\n=== FULL rtFlights[0] structure ===")
print(json.dumps(rtf[0] if rtf else {}, ensure_ascii=False, indent=2))

# 3. Try searchHotel WITH goFlightInfo embedded
print("\n=== Trying searchHotel with embedded flight objects ===")
time.sleep(1.5)
r3 = s.post(f"{BASE}/searchHotel", json={
    "criteriaCondId":"","hotelName":"","coptCd":"","faclCd":"","coptFaclCd":"",
    "fromPriceCd":"","toPriceCd":"","rcPlanDispFlg":"0","openStockPlanFlg":"1",
    "stockLimitsNo":"","planCd":"",
    "districtCd":DIST,"regionCd":"","areaCd":"",
    "depCalTextName_Second_3":jp(GO),"checkInDt":GO,
    "arrCalTextName_Second_4":jp(RET),"checkOutDt":RET,
    "sortCls":"recommend","pageNum":"",
    "_csrf":csrf,
    "segments": None,
    "goDeptDt":GO,"reDeptDt":RET,
    "goFlightInfo": gof[0] if gof else None,
    "rtnFlightInfo": rtf[0] if rtf else None,
}, headers={"Content-Type":"application/json; charset=UTF-8","Accept":"application/json, */*"}, timeout=60)
print(f"searchHotel status: {r3.status_code}")
try:
    b3 = r3.json()
    if r3.ok:
        page = b3.get("pageInfo",{}).get("page",{})
        print(f"✅ hotels={len(page.get('content',[]))} total={page.get('totalElements')} pages={page.get('totalPages')}")
    else:
        print("errors:", [e.get("message") for e in b3.get("errors",[])])
except: print("raw:", r3.text[:500])
