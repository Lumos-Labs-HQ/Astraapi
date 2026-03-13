"""
Main task runner for ANA Domestic Tour scraper.
Reads 255 search tasks, fetches prices for each date, saves to MongoDB.

Usage:
    python -m scraper.main                       # run all tasks due today
    python -m scraper.main --task-id 1           # run specific task only
    python -m scraper.main --task-id 1 --dry-run # test without saving
"""

import argparse
import logging
import os
import time
from datetime import date, timedelta
from typing import List, Optional

from scraper.ana_client import ANAClient
from scraper.mongo_client import MongoSaver
from scraper.task_loader import SearchTask, load_tasks

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s — %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler("logs/scraper.log", encoding="utf-8"),
    ],
)
logger = logging.getLogger(__name__)

# ------------------------------------------------------------------
# Configuration (from environment variables / .env)
# ------------------------------------------------------------------
MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017/ana_scraper")
DB_NAME = os.getenv("DB_NAME", "ana_scraper")
EXCEL_PATH = os.getenv("EXCEL_PATH", "")  # auto-discover *.xlsx in config/ if blank
REQUEST_DELAY = float(os.getenv("REQUEST_DELAY", "1.5"))


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

def should_run_today(task: SearchTask) -> bool:
    """Check if this task should run on today's weekday."""
    today_wd = date.today().weekday()   # 0=Mon … 6=Sun
    if task.run_weekday == -1:
        # biweekly — simplified: run on even weeks
        week_num = date.today().isocalendar()[1]
        return week_num % 2 == 0
    return task.run_weekday == today_wd


def date_range(days: int) -> List[str]:
    """Return list of YYYYMMDD strings from today for `days` consecutive days."""
    start = date.today()
    return [(start + timedelta(days=i)).strftime("%Y%m%d") for i in range(days)]


def run_task(
    task: SearchTask,
    client: ANAClient,
    saver: Optional[MongoSaver],
    dry_run: bool = False,
) -> int:
    """
    Execute one search task across all date range days.
    Returns number of records saved.
    """
    logger.info(
        "=== Task %d: %s → %s (%d adults, %d nights, %d days) ===",
        task.task_id,
        task.dept_airport,
        task.arr_airport,
        task.adults,
        task.nights,
        task.date_range_days,
    )

    dates = date_range(task.date_range_days)
    saved = 0
    errors = 0

    for go_date in dates:
        # Return date = go_date + nights
        go = date.fromisoformat(go_date[:4] + "-" + go_date[4:6] + "-" + go_date[6:])
        return_date = (go + timedelta(days=task.nights)).strftime("%Y%m%d")

        try:
            result = client.search_all_pages(
                dept_code=task.dept_airport,
                arr_code=task.arr_airport,
                go_date=go_date,
                return_date=return_date,
                adults=task.adults,
                district_code=task.district_code,
                region_code=task.region_code,
            )

            if dry_run:
                hotel_count = len(result.get("hotels", []))
                logger.info(
                    "[DRY RUN] Task %d / %s: %d hotels found (not saved)",
                    task.task_id, go_date, hotel_count,
                )
            else:
                saver.save_result(task.task_id, result)
                saved += 1
                logger.info(
                    "Task %d / %s: saved %d hotels",
                    task.task_id, go_date, len(result.get("hotels", [])),
                )

        except Exception as e:
            errors += 1
            logger.error("Task %d / %s: ERROR — %s", task.task_id, go_date, e)
            # Small back-off on error
            time.sleep(5)
            continue

    logger.info(
        "Task %d done: %d saved, %d errors", task.task_id, saved, errors
    )
    return saved


# ------------------------------------------------------------------
# Entry point
# ------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="ANA Domestic Tour Scraper")
    parser.add_argument("--task-id", type=int, help="Run only this task ID")
    parser.add_argument(
        "--all", action="store_true",
        help="Run all tasks regardless of scheduled weekday"
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Fetch data but do not save to MongoDB"
    )
    args = parser.parse_args()

    # Load tasks
    tasks = load_tasks(EXCEL_PATH)

    # Filter tasks
    if args.task_id:
        tasks = [t for t in tasks if t.task_id == args.task_id]
        if not tasks:
            logger.error("No task found with id=%d", args.task_id)
            return
    elif not args.all:
        tasks = [t for t in tasks if should_run_today(t)]
        logger.info("Today's tasks: %d", len(tasks))

    if not tasks:
        logger.info("No tasks to run today. Use --all to force run all.")
        return

    # Initialize clients
    client = ANAClient(request_delay=REQUEST_DELAY)
    saver = None if args.dry_run else MongoSaver(MONGO_URI, DB_NAME)

    try:
        total_saved = 0
        for task in tasks:
            saved = run_task(task, client, saver, dry_run=args.dry_run)
            total_saved += saved

        logger.info("All tasks complete. Total records saved: %d", total_saved)

    finally:
        if saver:
            saver.close()


if __name__ == "__main__":
    main()
