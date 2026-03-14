"""
test_full_flow.py — Tests complete flow with HND→CTS (Sapporo) direct flight.
Run: .venv\\Scripts\\python.exe test_full_flow.py
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

# --- Test params: HND→CTS (direct flight route), 1 night ---
DEPT="HND"; ARR="CTS"; DIST="02"; ADULTS=2; NIGHTS=1
start = date.today() + timedelta(days=30)
GO  = start.strftime("%Y%m%d")
RET = (start + timedelta(days=NIGHTS)).strftime("%Y%m%d")
print(f"Route: {DEPT}→{ARR}  Go={GO} ({jp(GO)})  Ret={RET} ({jp(RET)})")

s = requests.Session()
s.headers.update(HEADERS)

# 1. init
print("\n[1] init...")
r = s.get(f"{BASE}/init", params={
    "goDeptAirpCd":DEPT,"goArrAirpCd":ARR,"goDeptDt":GO,"rtnDeptDt":RET,
    "adultCnt":str(ADULTS),"districtCd":DIST,"checkInDt":GO,"checkOutDt":RET,
}, timeout=30)
csrf = re.search(r'<meta name="_csrf" content="([^"]+)"', r.text).group(1)
s.headers.update({"X-Requested-With":"XMLHttpRequest","X-CSRF-Token":csrf,"Referer":r.url,"Origin":"https://www.ana.co.jp"})
print(f"  OK. CSRF={csrf[:10]}... cookies={list(s.cookies.keys())}")

# 2. searchFlight
print("\n[2] searchFlight...")
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
f_data = r2.json()
gof = f_data.get("goResponse",{}).get("flights",[])
rtf = f_data.get("reResponse",{}).get("flights",[])
print(f"  Status={r2.status_code}  go={len(gof)} rt={len(rtf)}")

if gof:
    fs0 = gof[0].get("flightSet",[{}])[0]
    print(f"  First go: flt={fs0.get('flight')} dep={fs0.get('depTime')} arr={fs0.get('arrTime')} {fs0.get('depAirpCd')}→{fs0.get('arrAirpCd')}")
if rtf:
    fs0 = rtf[0].get("flightSet",[{}])[0]
    print(f"  First rt: flt={fs0.get('flight')} dep={fs0.get('depTime')} arr={fs0.get('arrTime')} {fs0.get('depAirpCd')}→{fs0.get('arrAirpCd')}")

# 3. searchHotel with embedded flight objects (use cheapest = index 0)
print("\n[3] searchHotel (with full go/rt flight objects embedded)...")
time.sleep(1.5)

# Try with goFlightInfo / rtnFlightInfo keys
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
    "goFlightInfo":  gof[0] if gof else None,
    "rtnFlightInfo": rtf[0] if rtf else None,
}, headers={"Content-Type":"application/json; charset=UTF-8","Accept":"application/json, */*"}, timeout=60)

print(f"  Status={r3.status_code}")
try:
    b3 = r3.json()
    if r3.ok:
        page = b3.get("pageInfo",{}).get("page",{})
        hotels = page.get("content",[])
        print(f"  ✅ hotels={len(hotels)} total={page.get('totalElements')} pages={page.get('totalPages')}")
        if hotels:
            h = hotels[0]
            pl = h.get("planList",[])
            p = pl[0] if pl else {}
            pr = p.get("price",{}).get("priceWithAir",{})
            pr_val = pr.get("priceWithAir","?") if isinstance(pr,dict) else "?"
            fi = p.get("flightInfo",{})
            print(f"  First hotel: {h.get('faclNam')}")
            print(f"  First plan:  ¥{pr_val:,}" if isinstance(pr_val,int) else f"  First plan:  {pr_val}")
            print(f"  Flight: go={fi.get('goFlightNo')} rt={fi.get('rtFlightNo')}")
    else:
        errs = [e.get("message","") for e in b3.get("errors",[])]
        print(f"  ❌ {errs}")
        # Save full response for inspection
        with open("hotel_error_resp.json","w",encoding="utf-8") as f:
            json.dump(b3, f, ensure_ascii=False, indent=2)
        print("  Saved error response to hotel_error_resp.json")
except Exception as ex:
    print(f"  raw: {r3.text[:600]}")
    print(f"  exception: {ex}")

print("\nDone.")
