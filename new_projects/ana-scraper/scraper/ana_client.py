"""
ANA Domestic Tour HTTP Client
Handles session initialization, CSRF extraction, and API requests.
All data fetched via direct HTTPS — no browser automation.
"""

import re
import time
import logging
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
    "Accept-Language": "ja,en-US;q=0.7,en;q=0.3",
    "Accept-Encoding": "gzip, deflate, br",
    "Connection": "keep-alive",
}


class ANAClient:
    """
    Client for fetching ANA Domestic Tour (Flight + Hotel) package prices.

    Usage:
        client = ANAClient()
        results = client.search_all_pages(
            dept_code="HND",
            arr_code="CTS",
            go_date="20260415",
            return_date="20260418",
            adults=2,
            district_code="02",
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
        district_code: str = "02",
        region_code: str = "",
        sort_cls: str = "recommend",
    ) -> Dict[str, Any]:
        """
        Initialize session and fetch ALL pages of hotel search results.

        Returns a dict:
            {
              "search_params": {...},
              "hotels": [...],   # full flattened list across all pages
              "total_elements": 47,
              "total_pages": 3,
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

        # Fresh session for each task
        self._reset_session()

        # Step 1: init
        self._init_session(
            dept_code, arr_code, go_date, return_date, adults, district_code
        )

        # Step 2: fetch page 1 to learn total pages
        page1 = self._fetch_hotel_page(
            go_date, return_date, district_code, region_code, sort_cls, page_num=""
        )
        page_info = page1.get("pageInfo", {}).get("page", {})
        total_pages = page_info.get("totalPages", 1)
        all_hotels = list(page_info.get("content", []))

        logger.info(
            "Task %s→%s [%s→%s]: page 1/%d, %d hotels",
            dept_code, arr_code, go_date, return_date, total_pages, len(all_hotels),
        )

        # Step 3: fetch remaining pages
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
        }

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _reset_session(self):
        """Start a fresh requests.Session to clear old cookies."""
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
        GET /init — sets session cookies and extracts CSRF token.
        Raises ValueError if CSRF token cannot be found.
        """
        params = {
            "goDeptAirpCd": dept_code,
            "goArrAirpCd": arr_code,
            "goDeptDt": go_date,
            "rtnDeptDt": return_date,
            "adultCnt": str(adults),
            "districtCd": district_code,
            "checkInDt": go_date,
            "checkOutDt": return_date,
        }

        url = f"{BASE_URL}/init"
        resp = self.session.get(url, params=params, timeout=30)
        resp.raise_for_status()
        self._init_url = resp.url

        match = re.search(r'<meta name="_csrf" content="([^"]+)"', resp.text)
        if not match:
            raise ValueError(
                f"CSRF token not found in init response for "
                f"{dept_code}→{arr_code} on {go_date}"
            )
        self._csrf_token = match.group(1)
        logger.debug("CSRF token obtained: %s", self._csrf_token)
        logger.debug("Session cookies: %s", dict(self.session.cookies))

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
        POST /searchHotel — returns raw JSON for one page of results.
        """
        url = f"{BASE_URL}/searchHotel"

        self.session.headers.update(
            {
                "Content-Type": "application/json; charset=UTF-8",
                "Accept": "application/json, text/javascript, */*; q=0.01",
                "X-Requested-With": "XMLHttpRequest",
                "X-CSRF-Token": self._csrf_token,
                "Referer": self._init_url or url,
                "Origin": "https://www.ana.co.jp",
            }
        )

        payload = {
            "criteriaCondId": "",
            "hotelName": "",
            "coptCd": "",
            "faclCd": "",
            "districtCd": district_code,
            "regionCd": region_code,
            "areaCd": "",
            "checkInDt": go_date,
            "checkOutDt": return_date,
            "sortCls": sort_cls,
            "pageNum": page_num,
            "_csrf": self._csrf_token,
            "goDeptDt": go_date,
            "reDeptDt": return_date,
        }

        time.sleep(self.request_delay)
        resp = self.session.post(url, json=payload, timeout=30)
        resp.raise_for_status()
        return resp.json()
