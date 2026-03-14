"""
ANA Domestic Tour HTTP Client — correct 4-step flow (confirmed working 2026-03-14)
All requests via direct HTTPS; no browser automation.

Flow:
  1. GET  /init             → session cookies + CSRF token
  2. POST /searchFlight     → available go/return flights (form-urlencoded)
  3. POST /getCriteriaList  → sets chosen flight in server-side session (JSON)
  4. POST /searchHotel      → returns hotel list + plans + prices (JSON, paginated)

Why this works:
  • Only step 3 (getCriteriaList with goFlightInfo + rtnFlightInfo) makes the server
    aware of which flight is selected, satisfying SERR0034/SERR0035 validation.
  • searchHotel then returns ALL hotels regardless of flight (client requirement:
    save everything, filter later by flight number or hotel name on MongoDB side).
"""

import re
import time
import logging
from datetime import date
from typing import Optional, Dict, Any, List

import requests

logger = logging.getLogger(__name__)

BASE_URL = "https://www.ana.co.jp/domtour/booking/csm/search/DSCP0390"

DEFAULT_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/122.0.0.0 Safari/537.36"
    ),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "ja,en-US;q=0.7,en;q=0.3",
    "Accept-Encoding": "gzip, deflate, br",
    "Connection": "keep-alive",
}

_JP_WEEKDAY = ["（月）", "（火）", "（水）", "（木）", "（金）", "（土）", "（日）"]
_JP_MONTH   = ["", "1月", "2月", "3月", "4月", "5月", "6月",
               "7月", "8月", "9月", "10月", "11月", "12月"]


def _jp_date_text(yyyymmdd: str) -> str:
    """'20260413' → '4月13日（月）'"""
    d = date(int(yyyymmdd[:4]), int(yyyymmdd[4:6]), int(yyyymmdd[6:8]))
    return f"{_JP_MONTH[d.month]}{d.day}日{_JP_WEEKDAY[d.weekday()]}"


class ANAClient:
    """
    ANA Domestic Tour (Flight + Hotel) scraper.

    Returns ALL search results per date so that MongoDB filtering by flight
    number and/or hotel name can be done post-query.
    """

    def __init__(self, request_delay: float = 1.5):
        self.request_delay = request_delay
        self._session: Optional[requests.Session] = None
        self._csrf: Optional[str] = None
        self._init_url: Optional[str] = None

    # ------------------------------------------------------------------ #
    # Public                                                               #
    # ------------------------------------------------------------------ #

    def search_all_pages(
        self,
        dept_code: str,
        arr_code: str,
        go_date: str,
        return_date: str,
        adults: int = 2,
        district_code: str = "08",
        region_code: str = "",
        sort_cls: str = "recommend",
    ) -> Dict[str, Any]:
        """
        Full 4-step flow: init → searchFlight → getCriteriaList → searchHotel (all pages).

        Returns:
          {
            "search_params": { goDeptAirpCd, goArrAirpCd, goDeptDt, ... },
            "hotels":        [ ... all hotel+plan objects across all pages ... ],
            "total_elements": int,
            "total_pages":    int,
            "go_flights":     [ ... ],  # from searchFlight
            "rt_flights":     [ ... ],
          }
        """
        search_params = {
            "goDeptAirpCd": dept_code,
            "goArrAirpCd":  arr_code,
            "goDeptDt":     go_date,
            "rtnDeptDt":    return_date,
            "adultCnt":     adults,
            "districtCd":   district_code,
            "checkInDt":    go_date,
            "checkOutDt":   return_date,
        }

        self._new_session()

        # Step 1 — GET /init
        self._init_session(dept_code, arr_code, go_date, return_date, adults, district_code)

        # Step 2 — POST /searchFlight
        go_flights, rt_flights = self._search_flight(
            dept_code, arr_code, go_date, return_date, adults)
        logger.info(
            "%s→%s [%s]: go=%d rt=%d flights",
            dept_code, arr_code, go_date, len(go_flights), len(rt_flights),
        )

        if not go_flights or not rt_flights:
            logger.warning("No flights found — skipping hotel search")
            return {
                "search_params":  search_params,
                "hotels":         [],
                "total_elements": 0,
                "total_pages":    0,
                "go_flights":     go_flights,
                "rt_flights":     rt_flights,
            }

        # Step 3 — POST /getCriteriaList (set flight selection in server session)
        self._select_flight(go_flights[0], rt_flights[0], go_date, return_date, district_code)

        # Step 4 — POST /searchHotel (get ALL hotels, all pages)
        page1       = self._fetch_hotel_page(go_date, return_date,
                                              district_code, region_code, sort_cls, "")
        page_info   = page1.get("pageInfo", {}).get("page", {})
        total_pages  = page_info.get("totalPages", 1)
        all_hotels   = list(page_info.get("content", []))

        logger.info(
            "  page 1/%d: %d hotels (total=%s)",
            total_pages, len(all_hotels), page_info.get("totalElements", "?"),
        )

        for page in range(2, total_pages + 1):
            time.sleep(self.request_delay)
            result  = self._fetch_hotel_page(go_date, return_date,
                                              district_code, region_code, sort_cls, str(page))
            content = result.get("pageInfo", {}).get("page", {}).get("content", [])
            all_hotels.extend(content)
            logger.info("  page %d/%d: +%d hotels", page, total_pages, len(content))

        return {
            "search_params":  search_params,
            "hotels":         all_hotels,
            "total_elements": page_info.get("totalElements", len(all_hotels)),
            "total_pages":    total_pages,
            "go_flights":     go_flights,
            "rt_flights":     rt_flights,
        }

    # ------------------------------------------------------------------ #
    # Internal helpers                                                     #
    # ------------------------------------------------------------------ #

    def _new_session(self):
        s = requests.Session()
        s.headers.update(DEFAULT_HEADERS)
        self._session  = s
        self._csrf     = None
        self._init_url = None

    def _init_session(self, dept: str, arr: str, go: str, ret: str,
                      adults: int, district: str):
        """GET /init — seeds session cookies and extracts CSRF token."""
        resp = self._session.get(
            f"{BASE_URL}/init",
            params={
                "goDeptAirpCd": dept, "goArrAirpCd": arr,
                "goDeptDt": go, "rtnDeptDt": ret,
                "adultCnt": str(adults), "districtCd": district,
                "checkInDt": go, "checkOutDt": ret,
            },
            timeout=30,
        )
        resp.raise_for_status()
        self._init_url = resp.url

        m = re.search(r'<meta name="_csrf" content="([^"]+)"', resp.text)
        if not m:
            raise ValueError(f"CSRF not found for {dept}→{arr} on {go}")
        self._csrf = m.group(1)
        logger.debug("init OK — CSRF=%s… cookies=%s",
                     self._csrf[:12], list(self._session.cookies.keys()))

        self._session.headers.update({
            "X-Requested-With": "XMLHttpRequest",
            "X-CSRF-Token":     self._csrf,
            "Referer":          self._init_url,
            "Origin":           "https://www.ana.co.jp",
        })

    def _search_flight(self, dept: str, arr: str, go: str, ret: str,
                       adults: int):
        """POST /searchFlight → (go_flights, rt_flights)."""
        time.sleep(self.request_delay)
        resp = self._session.post(
            f"{BASE_URL}/searchFlight",
            data={
                "depCalTextName_Second": _jp_date_text(go),
                "goDeptDt":             go,
                "goDeptAirpCd":         dept,
                "goArrAirpCd":          arr,
                "arrCalTextName_Second": _jp_date_text(ret),
                "rtnDeptDt":            ret,
                "rtnDeptAirpCd":        arr,   # return departs FROM destination
                "rtnArrAirpCd":         dept,  # return arrives TO origin
                "adultCnt":             str(adults),
                "infant2Cnt":           "0",
                "infantCnt":            "0",
                "childADpCnt": "0", "childBDpCnt": "0",
                "childCDpCnt": "0", "childDDpCnt": "0",
                "childEDpCnt": "0", "childFDpCnt": "0",
                "childGDpCnt": "0", "childHDpCnt": "0",
                "childIDpCnt": "0", "childJDpCnt": "0",
                "directDispFlg": "0",
                "_csrf":         self._csrf,
                "checkInDt":     go,
                "checkOutDt":    ret,
            },
            headers={
                "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
                "Accept":       "application/json, */*; q=0.01",
            },
            timeout=60,
        )
        if not resp.ok:
            logger.warning("searchFlight %d: %s", resp.status_code, resp.text[:300])
            return [], []
        data = resp.json()
        return (
            data.get("goResponse", {}).get("flights", []),
            data.get("reResponse", {}).get("flights", []),
        )

    def _select_flight(self, go_flight: Dict, rt_flight: Dict,
                       go: str, ret: str, district: str):
        """
        POST /getCriteriaList with goFlightInfo + rtnFlightInfo.
        This registers the flight selection in the server-side session,
        which is required before searchHotel will accept the request.
        """
        time.sleep(self.request_delay)
        resp = self._session.post(
            f"{BASE_URL}/getCriteriaList",
            json={
                "criteriaCondId":   "",
                "hotelName":        "",
                "coptCd":           "",
                "faclCd":           "",
                "coptFaclCd":       "",
                "fromPriceCd":      "",
                "toPriceCd":        "",
                "rcPlanDispFlg":    "0",
                "openStockPlanFlg": "1",
                "stockLimitsNo":    "",
                "planCd":           "",
                "districtCd":       district,
                "regionCd":         "",
                "areaCd":           "",
                "checkInDt":        go,
                "checkOutDt":       ret,
                "_csrf":            self._csrf,
                "segments":         None,
                "goDeptDt":         go,
                "reDeptDt":         ret,
                "goFlightInfo":     go_flight,
                "rtnFlightInfo":    rt_flight,
            },
            headers={
                "Content-Type": "application/json; charset=UTF-8",
                "Accept":       "application/json, */*; q=0.01",
            },
            timeout=30,
        )
        if not resp.ok:
            logger.warning(
                "getCriteriaList %d: %s", resp.status_code, resp.text[:300])
        else:
            logger.debug("getCriteriaList OK (flight set in session)")

    def _fetch_hotel_page(self, go: str, ret: str, district: str,
                          region: str, sort_cls: str, page_num: str) -> Dict:
        """POST /searchHotel — one page of hotels."""
        time.sleep(self.request_delay)
        resp = self._session.post(
            f"{BASE_URL}/searchHotel",
            json={
                "criteriaCondId":   "",
                "hotelName":        "",
                "coptCd":           "",
                "faclCd":           "",
                "coptFaclCd":       "",
                "fromPriceCd":      "",
                "toPriceCd":        "",
                "rcPlanDispFlg":    "0",
                "openStockPlanFlg": "1",
                "stockLimitsNo":    "",
                "planCd":           "",
                "districtCd":       district,
                "regionCd":         region,
                "areaCd":           "",
                "depCalTextName_Second_3": _jp_date_text(go),
                "checkInDt":              go,
                "arrCalTextName_Second_4": _jp_date_text(ret),
                "checkOutDt":             ret,
                "sortCls":   sort_cls,
                "pageNum":   page_num,
                "_csrf":     self._csrf,
                "segments":  None,
                "goDeptDt":  go,
                "reDeptDt":  ret,
            },
            headers={
                "Content-Type": "application/json; charset=UTF-8",
                "Accept":       "application/json, */*; q=0.01",
            },
            timeout=60,
        )
        if not resp.ok:
            logger.error("searchHotel %d [page=%r]: %s",
                         resp.status_code, page_num, resp.text[:800])
            resp.raise_for_status()
        return resp.json()
