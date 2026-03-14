#!/usr/bin/env python3
"""
test_run.py — Quick end-to-end test for the ANA scraper.

Usage examples:
    # Just verify Excel loads (no network, no MongoDB)
    python test_run.py --excel-only

    # Dry-run first 10 tasks (real ANA API, NO MongoDB save)
    python test_run.py --limit 10 --dry-run

    # REAL run: first 10 tasks → fetch ANA data → save to MongoDB
    python test_run.py --limit 10

    # Single task dry-run
    python test_run.py --task-id 1 --dry-run

    # Single task, real save to MongoDB
    python test_run.py --task-id 1
"""

import argparse
import logging
import os
import sys
from datetime import date, timedelta
from typing import Optional

try:
    from dotenv import load_dotenv
    load_dotenv()  # loads .env into os.environ
except ImportError:
    pass  # python-dotenv not installed, use system env vars

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s — %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)],
)
logger = logging.getLogger("test_run")

MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017/ana_scraper")
DB_NAME   = os.getenv("DB_NAME", "ana_scraper")


def test_excel():
    from scraper.task_loader import load_tasks, get_go_flight_label, get_rt_flight_label
    tasks = load_tasks()
    print(f"\n✅  Loaded {len(tasks)} tasks\n")
    print(f"{'ID':>4}  {'Day':<4}  {'Route':<10}  {'N':>2}  {'A':>2}  "
          f"{'Go flight':<10}  {'Rt flight':<10}  Hotel")
    print("-" * 90)
    days = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun']
    for t in tasks[:20]:          # show first 20 rows as preview
        go  = get_go_flight_label(t) or "cheapest"
        rt  = get_rt_flight_label(t) or "cheapest"
        print(f"{t.task_id:>4}  {days[t.run_weekday]:<4}  "
              f"{t.dept_airport}→{t.arr_airport:<5}  {t.nights:>2}n  {t.adults:>2}p  "
              f"{go:<10}  {rt:<10}  {(t.hotel_name or '-')[:30]}")
    if len(tasks) > 20:
        print(f"  … {len(tasks)-20} more tasks …")


def run_tasks(
    task_id:  Optional[int],
    limit:    int,
    dry_run:  bool,
    days:     int,
):
    from scraper.task_loader import load_tasks
    from scraper.ana_client  import ANAClient
    from scraper.mongo_client import MongoSaver

    all_tasks = load_tasks()
    if task_id:
        tasks = [t for t in all_tasks if t.task_id == task_id]
    else:
        tasks = all_tasks[:limit]

    if not tasks:
        print("❌  No matching tasks.")
        return

    print(f"\n{'DRY RUN — ' if dry_run else ''}Running {len(tasks)} task(s) × {days} day(s)\n")

    client = ANAClient(request_delay=1.5)
    saver  = None if dry_run else MongoSaver(MONGO_URI, DB_NAME)

    total_saved = 0
    total_errors = 0

    try:
        for task in tasks:
            days_map = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun']
            print(f"\n── Task {task.task_id}: {task.dept_airport}→{task.arr_airport} "
                  f"[{days_map[task.run_weekday]}] {task.nights}n {task.adults}p ──")
            print(f"   Hotel : {task.hotel_name}")
            print(f"   Go flt: {task.go_flight_filter} {task.go_flight_num}")

            start = date.today()
            test_dates = [
                (start + timedelta(days=i)).strftime("%Y%m%d")
                for i in range(days)
            ]

            for go_date in test_dates:
                go_d = date.fromisoformat(f"{go_date[:4]}-{go_date[4:6]}-{go_date[6:]}")
                ret_date = (go_d + timedelta(days=task.nights)).strftime("%Y%m%d")

                try:
                    result = client.search_all_pages(
                        dept_code=task.dept_airport,
                        arr_code=task.arr_airport,
                        go_date=go_date,
                        return_date=ret_date,
                        adults=task.adults,
                        district_code=task.district_code,
                        region_code=task.region_code,
                    )
                    hotels = result.get("hotels", [])
                    total  = result.get("total_elements", "?")

                    if dry_run:
                        print(f"   {go_date} → {len(hotels)} hotels (total={total}) [NOT saved]")
                    else:
                        doc_id = saver.save_result(task.task_id, result)
                        total_saved += 1
                        print(f"   {go_date} → {len(hotels)} hotels saved ✅  (id={doc_id})")

                    # Print first hotel as sample
                    if hotels:
                        h = hotels[0]
                        plans = h.get("planList", [])
                        p  = plans[0] if plans else {}
                        fi = p.get("flightInfo", {})
                        pr_obj = p.get("price", {}).get("priceWithAir", {})
                        pr = pr_obj.get("priceWithAir", "?") if isinstance(pr_obj, dict) else "?"
                        pr_str = f"¥{pr:,}" if isinstance(pr, int) else str(pr)
                        print(f"   └ {h.get('faclNam','')} | "
                              f"{fi.get('goFlightNo','')}→{fi.get('rtFlightNo','')} | {pr_str}")

                except Exception as exc:
                    total_errors += 1
                    print(f"   {go_date} ✗ ERROR: {exc}")

    finally:
        if saver:
            saver.close()

    print(f"\n{'─'*60}")
    if dry_run:
        print(f"DRY RUN complete — no data saved.")
    else:
        print(f"✅  Saved {total_saved} records  |  ✗ {total_errors} errors")
        print(f"\nCheck MongoDB:")
        print(f"  mongo shell : use {DB_NAME}; db.search_results.countDocuments()")
        print(f"  or Compass  : {MONGO_URI}/{DB_NAME}")


def main():
    p = argparse.ArgumentParser(description="ANA Scraper — test runner")
    p.add_argument("--excel-only", action="store_true",
                   help="Load + display Excel tasks only (no network)")
    p.add_argument("--task-id", type=int, default=None,
                   help="Run a single task ID")
    p.add_argument("--limit", type=int, default=10,
                   help="Run first N tasks (default 10)")
    p.add_argument("--days", type=int, default=3,
                   help="Date range to test per task (default 3 days)")
    p.add_argument("--dry-run", action="store_true",
                   help="Fetch ANA data but do NOT save to MongoDB")
    args = p.parse_args()

    print("=" * 60)
    print("ANA Scraper — Test Runner")
    print("=" * 60)

    if args.excel_only:
        test_excel()
    else:
        run_tasks(args.task_id, args.limit, args.dry_run, args.days)


if __name__ == "__main__":
    main()
