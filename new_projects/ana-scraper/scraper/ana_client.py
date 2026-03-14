"""
ANA Domestic Tour HTTP Client
Handles session initialization, CSRF extraction, flight search, and hotel search.
All data fetched via direct HTTPS — no browser automation.

Correct API flow (confirmed by live browser network capture 2026-03-14):
    1. GET  /init            → query-params: airports, dates, pax → cookies + CSRF
    2. POST /searchFlight    → form-urlencoded (ALL fields required) → go/rt flight lists
    3. POST /searchHotel     → JSON body → hotel + plan list (paginated)

Fixed fields for searchFlight (cause of 400 errors):
    goDeptAirpCd, goArrAirpCd, rtnDeptAirpCd, rtnArrAirpCd,
    adultCnt, infantCnt, infant2Cnt, childA-HDpCnt (all 0),
    directDispFlg, checkInDt, checkOutDt,
    depCalTextName_Second, arrCalTextName_Second
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

# Japanese weekday abbreviations for date display text sent to API
_JP_WEEKDAY = ["（月）", "（火）", "（水）", "（木）", "（金）", "（土）", "（日）"]
_JP_MONTH = ["", "1月", "2月", "3月", "4月", "5月", "6月",
             "7月", "8月", "9月", "10月", "11月", "12月"]


def _jp_date_text(yyyymmdd: str) -> str:
    """Convert '20260420' → '4月20日（月）' (Japanese date display string)."""
    d = date(int(yyyymmdd[:4]), int(yyyymmdd[4:6]), int(yyyymmdd[6:8]))
    return f"{_JP_MONTH[d.month]}{d.day}日{_JP_WEEKDAY[d.weekday()]}"


class ANAClient:
    """
    Client for fetching ANA Domestic Tour (Flight + Hotel) package prices.

    Usage:
        client = ANAClient()
        result = client.search_all_pages(
            dept_code="HND",
            arr_code="ISG",
            go_date="20260420",
            return_date="20260422",
            adults=2,
            district_code="08",
        )
    """

    def __init__(self, request_delay: float = 1.5):
        self.session = requests.Session()
        self.session.headers.update(DEFAULT_HEADERS)
        self.request_delay = request_delay
        self._csrf_token: Optional[str] = None
        self._init_url: Optional[str] = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

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
        Full flow: init → searchFlight → searchHotel (all pages).

        Returns:
            {
              "search_params": {...},
              "hotels": [...],        # full list across all pages
              "total_elements": int,
              "total_pages": int,
              "go_flights": [...],    # raw go flight options
              "rt_flights": [...],    # raw return flight options
            }
        """
        search_params = {
            "goDeptAirpCd": dept_code,
            "goArrAirpCd": arr_code,
            "goDeptDt": go_date,
            "rtnDeptDt": return_date,
            "adultCnt": adults,
            "districtCd": district_code,
            "checkInDt": go_date,
            "checkOutDt": return_date,
        }

        # Fresh session per task run
        self._reset_session()

        # Step 1: GET /init → cookies + CSRF
        self._init_session(dept_code, arr_code, go_date, return_date, adults, district_code)

        # Step 2: POST /searchFlight → flight availability
        flight_data = self._search_flight(dept_code, arr_code, go_date, return_date, adults)
        go_flights = flight_data.get("goResponse", {}).get("flights", [])
        rt_flights = flight_data.get("reResponse", {}).get("flights", [])

        logger.info(
            "%s→%s [%s→%s]: %d outbound/%d return flights",
            dept_code, arr_code, go_date, return_date,
            len(go_flights), len(rt_flights),
        )

        if not go_flights and not rt_flights:
            logger.warning("No flights for %s→%s on %s", dept_code, arr_code, go_date)
            return {
                "search_params": search_params,
                "hotels": [],
                "total_elements": 0,
                "total_pages": 0,
                "go_flights": [],
                "rt_flights": [],
            }

        # Step 3: POST /searchHotel (page 1)
        page1 = self._fetch_hotel_page(
            go_date, return_date, district_code, region_code, sort_cls, page_num=""
        )
        page_info = page1.get("pageInfo", {}).get("page", {})
        total_pages = page_info.get("totalPages", 1)
        all_hotels = list(page_info.get("content", []))

        logger.info(
            "  page 1/%d: %d hotels (total=%s)",
            total_pages, len(all_hotels), page_info.get("totalElements", "?"),
        )

        # Step 4: Remaining pages
        for page in range(2, total_pages + 1):
            time.sleep(self.request_delay)
            result = self._fetch_hotel_page(
                go_date, return_date, district_code, region_code, sort_cls,
                page_num=str(page)
            )
            content = result.get("pageInfo", {}).get("page", {}).get("content", [])
            all_hotels.extend(content)
            logger.info("  page %d/%d: +%d hotels", page, total_pages, len(content))

        return {
            "search_params": search_params,
            "hotels": all_hotels,
            "total_elements": page_info.get("totalElements", len(all_hotels)),
            "total_pages": total_pages,
            "go_flights": go_flights,
            "rt_flights": rt_flights,
        }

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _reset_session(self):
        """Start a fresh requests.Session to clear old cookies/state."""
        self.session = requests.Session()
        self.session.headers.update(DEFAULT_HEADERS)
        self._csrf_token = None
        self._init_url = None

    def _init_session(
        self,
        dept_code: str,
        arr_code: str,
        go_date: str,
        return_date: str,
        adults: int,
        district_code: str,
    ):
        """
        GET /init — sets session cookies (JSESSIONID, ATDCSMSID, ATDSVR)
        and extracts CSRF token from the HTML <meta name="_csrf"> tag.
        """
        params = {
            "goDeptAirpCd": dept_code,
            "goArrAirpCd":  arr_code,
            "goDeptDt":     go_date,
            "rtnDeptDt":    return_date,
            "adultCnt":     str(adults),
            "districtCd":   district_code,
            "checkInDt":    go_date,
            "checkOutDt":   return_date,
        }
        resp = self.session.get(f"{BASE_URL}/init", params=params, timeout=30)
        resp.raise_for_status()
        self._init_url = resp.url

        match = re.search(r'<meta name="_csrf" content="([^"]+)"', resp.text)
        if not match:
            raise ValueError(
                f"CSRF token not found in /init response for "
                f"{dept_code}→{arr_code} on {go_date}. "
                f"Status={resp.status_code}, Content-Type={resp.headers.get('Content-Type')}"
            )
        self._csrf_token = match.group(1)
        logger.debug(
            "init OK — CSRF: %s | cookies: %s",
            self._csrf_token[:12] + "…",
            list(self.session.cookies.keys()),
        )

        # Set XHR headers for all subsequent AJAX requests
        self.session.headers.update({
            "X-Requested-With": "XMLHttpRequest",
            "X-CSRF-Token":     self._csrf_token,
            "Referer":          self._init_url,
            "Origin":           "https://www.ana.co.jp",
        })

    def _search_flight(
        self,
        dept_code: str,
        arr_code: str,
        go_date: str,
        return_date: str,
        adults: int,
    ) -> Dict[str, Any]:
        """
        POST /searchFlight — application/x-www-form-urlencoded.

        ALL fields are required. Missing any location / pax field causes 400.
        Fields confirmed by live browser network capture (2026-03-14):

          depCalTextName_Second  → Japanese date display text (e.g. '4月20日（月）')
          goDeptDt               → YYYYMMDD
          goDeptAirpCd           → departure IATA code
          goArrAirpCd            → arrival IATA code
          arrCalTextName_Second  → Japanese date display text
          rtnDeptDt              → YYYYMMDD
          rtnDeptAirpCd          → return departure = arr_code
          rtnArrAirpCd           → return arrival  = dept_code
          adultCnt               → number of adults
          infant2Cnt             → 0
          infantCnt              → 0
          childA-HDpCnt          → 0 each (10 child category fields)
          directDispFlg          → 0 (show all, not just direct)
          _csrf                  → CSRF token from /init
          checkInDt              → YYYYMMDD (= go_date)
          checkOutDt             → YYYYMMDD (= return_date)
        """
        form_data = {
            # Date display text (Japanese) — required by server
            "depCalTextName_Second": _jp_date_text(go_date),
            "goDeptDt":              go_date,
            # Outbound airports
            "goDeptAirpCd":          dept_code,
            "goArrAirpCd":           arr_code,
            # Return date + airports (return flight goes arr→dept)
            "arrCalTextName_Second": _jp_date_text(return_date),
            "rtnDeptDt":             return_date,
            "rtnDeptAirpCd":         arr_code,    # ← was missing! Return departs FROM arr
            "rtnArrAirpCd":          dept_code,   # ← was missing! Return arrives TO dept
            # Passenger counts
            "adultCnt":              str(adults),
            "infant2Cnt":            "0",
            "infantCnt":             "0",
            # Child categories (A-J style codes) — must send zeros
            "childADpCnt":           "0",
            "childCDpCnt":           "0",
            "childIDpCnt":           "0",
            "childJDpCnt":           "0",
            "childEDpCnt":           "0",
            "childFDpCnt":           "0",
            "childGDpCnt":           "0",
            "childHDpCnt":           "0",
            "childBDpCnt":           "0",
            "childDDpCnt":           "0",
            # Flight type: 0 = all (direct + connecting), 1 = direct only
            "directDispFlg":         "0",
            # CSRF
            "_csrf":                 self._csrf_token,
            # Hotel dates
            "checkInDt":             go_date,
            "checkOutDt":            return_date,
        }

        time.sleep(self.request_delay)
        resp = self.session.post(
            f"{BASE_URL}/searchFlight",
            data=form_data,
            headers={
                "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
                "Accept":       "application/json, text/javascript, */*; q=0.01",
            },
            timeout=60,
        )

        if not resp.ok:
            body_preview = resp.text[:600]
            logger.warning(
                "searchFlight %d for %s→%s [%s]: %s",
                resp.status_code, dept_code, arr_code, go_date, body_preview,
            )
            # Return empty — caller will handle no-flight case gracefully
            return {}

        try:
            return resp.json()
        except Exception:
            logger.error("searchFlight non-JSON response: %s", resp.text[:300])
            return {}

    def _fetch_hotel_page(
        self,
        go_date: str,
        return_date: str,
        district_code: str,
        region_code: str,
        sort_cls: str,
        page_num: str,
    ) -> Dict[str, Any]:
        """
        POST /searchHotel — application/json.

        All fields confirmed by live browser network capture (2026-03-14).
        Server uses session state (airports/pax) set during /init.
        """
        payload = {
            # Filter fields (blank = no filter)
            "criteriaCondId":         "",
            "hotelName":              "",
            "coptCd":                 "",
            "faclCd":                 "",
            "coptFaclCd":             "",       # new required field
            "fromPriceCd":            "",
            "toPriceCd":              "",
            "rcPlanDispFlg":          "0",
            "openStockPlanFlg":       "1",      # show available plans
            "stockLimitsNo":          "",
            "planCd":                 "",
            # Location
            "districtCd":             district_code,
            "regionCd":               region_code,
            "areaCd":                 "",
            # Date display text (Japanese)
            "depCalTextName_Second_3": _jp_date_text(go_date),
            "checkInDt":              go_date,
            "arrCalTextName_Second_4": _jp_date_text(return_date),
            "checkOutDt":             return_date,
            # Sort and pagination
            "sortCls":                sort_cls,
            "pageNum":                page_num,
            # CSRF
            "_csrf":                  self._csrf_token,
            # Flight dates (server matches flights loaded in session)
            "segments":               None,
            "goDeptDt":               go_date,
            "reDeptDt":               return_date,
        }

        time.sleep(self.request_delay)
        resp = self.session.post(
            f"{BASE_URL}/searchHotel",
            json=payload,
            headers={
                "Content-Type": "application/json; charset=UTF-8",
                "Accept":       "application/json, text/javascript, */*; q=0.01",
            },
            timeout=60,
        )

        if not resp.ok:
            logger.error(
                "searchHotel %d [page=%r]: %s",
                resp.status_code, page_num, resp.text[:800],
            )
            resp.raise_for_status()

        try:
            return resp.json()
        except Exception:
            logger.error("searchHotel non-JSON: %s", resp.text[:300])
            resp.raise_for_status()
            return {}
