"""
MongoDB client for saving ANA scrape results.
Uses upsert so re-running the same task is idempotent.
"""

import logging
from datetime import datetime, timezone
from typing import Any, Dict

from pymongo import MongoClient, UpdateOne
from pymongo.collection import Collection

logger = logging.getLogger(__name__)


class MongoSaver:
    """
    Handles saving raw ANA search results to MongoDB.

    Each document stored per (task_id, goDeptDt) — upserted on re-run.

    Collection structure:
        {
          task_id: int,
          scraped_at: datetime,
          search_params: {...},
          hotels: [...],             # raw hotel + plan list
          total_elements: int,
          total_pages: int,
        }
    """

    def __init__(self, mongo_uri: str, db_name: str = "ana_scraper"):
        self.client = MongoClient(mongo_uri)
        self.db = self.client[db_name]
        self.collection: Collection = self.db["search_results"]

        # Index for fast lookups
        self.collection.create_index(
            [("task_id", 1), ("search_params.goDeptDt", 1)],
            unique=True,
            name="task_date_unique",
        )
        logger.info("MongoDB connected: %s / %s", mongo_uri, db_name)

    def save_result(self, task_id: int, result: Dict[str, Any]) -> str:
        """
        Upsert one search result.
        Returns the MongoDB _id of the upserted document.
        """
        go_date = result["search_params"]["goDeptDt"]
        doc = {
            "task_id": task_id,
            "scraped_at": datetime.now(timezone.utc),
            "search_params": result["search_params"],
            "hotels": result["hotels"],
            "total_elements": result.get("total_elements", 0),
            "total_pages": result.get("total_pages", 1),
        }

        res = self.collection.update_one(
            filter={
                "task_id": task_id,
                "search_params.goDeptDt": go_date,
            },
            update={"$set": doc},
            upsert=True,
        )

        doc_id = str(res.upserted_id or "updated")
        logger.debug(
            "Saved task_id=%d date=%s -> %s (matched=%d)",
            task_id, go_date, doc_id, res.matched_count,
        )
        return doc_id

    def save_batch(self, task_id: int, results: list[Dict[str, Any]]) -> int:
        """
        Bulk upsert multiple results (one per date).
        Returns count of upserted/updated documents.
        """
        if not results:
            return 0

        ops = []
        for result in results:
            go_date = result["search_params"]["goDeptDt"]
            doc = {
                "task_id": task_id,
                "scraped_at": datetime.now(timezone.utc),
                "search_params": result["search_params"],
                "hotels": result["hotels"],
                "total_elements": result.get("total_elements", 0),
                "total_pages": result.get("total_pages", 1),
            }
            ops.append(
                UpdateOne(
                    filter={
                        "task_id": task_id,
                        "search_params.goDeptDt": go_date,
                    },
                    update={"$set": doc},
                    upsert=True,
                )
            )

        res = self.collection.bulk_write(ops, ordered=False)
        count = res.upserted_count + res.modified_count
        logger.info("bulk_write task_id=%d: %d ops done", task_id, count)
        return count

    def get_hotels_by_flight(
        self, task_id: int, go_date: str, flight_no: str
    ) -> list:
        """
        Post-query filter: find hotel plans matching a specific flight number.
        The flight_no is matched against flightInfo.goFlightNo or rtFlightNo.
        """
        doc = self.collection.find_one(
            {"task_id": task_id, "search_params.goDeptDt": go_date}
        )
        if not doc:
            return []

        matched = []
        for hotel in doc.get("hotels", []):
            for plan in hotel.get("planList", []):
                fi = plan.get("flightInfo", {})
                if flight_no in (fi.get("goFlightNo", ""), fi.get("rtFlightNo", "")):
                    matched.append(
                        {
                            "faclNam": hotel.get("faclNam"),
                            "faclCd": hotel.get("faclCd"),
                            "planNam": plan.get("planNam"),
                            "flightInfo": fi,
                            "price": plan.get("price", {}),
                        }
                    )
        return matched

    def close(self):
        self.client.close()
