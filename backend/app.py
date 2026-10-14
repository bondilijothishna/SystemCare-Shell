import json
import os
import sqlite3
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, jsonify, request


PROJECT_ROOT = Path(__file__).resolve().parent.parent
AGENT_PATH = PROJECT_ROOT / "os-engine" / "scagent"
DATABASE_PATH = PROJECT_ROOT / "data" / "systemcare.db"
REQUIRED_METRIC_FIELDS = {
    "cpu_counters",
    "memory",
    "disk",
    "uptime_seconds",
    "processes",
}
DEFAULT_HISTORY_LIMIT = 20
MAX_HISTORY_LIMIT = 100

app = Flask(__name__)


def initialize_database():
    """Create the snapshot table when the API first needs the database."""
    DATABASE_PATH.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(DATABASE_PATH)
    try:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS metric_snapshots (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                captured_at TEXT NOT NULL,
                metrics_json TEXT NOT NULL
            )
            """
        )
        connection.commit()
    finally:
        connection.close()


def save_snapshot(metrics):
    """Store one successful agent response with its UTC capture time."""
    captured_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    metrics_json = json.dumps(metrics, separators=(",", ":"))

    initialize_database()
    connection = sqlite3.connect(DATABASE_PATH)
    try:
        connection.execute(
            "INSERT INTO metric_snapshots (captured_at, metrics_json) VALUES (?, ?)",
            (captured_at, metrics_json),
        )
        connection.commit()
    finally:
        connection.close()

    return captured_at


@app.get("/api/health")
def health():
    return jsonify({"status": "ok"})


@app.get("/api/metrics")
def metrics():
    try:
        result = subprocess.run(
            [str(AGENT_PATH)],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return jsonify({"error": "monitoring agent timed out"}), 502
    except (OSError, UnicodeError) as error:
        return jsonify({"error": "could not run monitoring agent", "details": str(error)}), 502

    if result.returncode != 0:
        return jsonify({
            "error": "monitoring agent failed",
            "details": result.stderr.strip()[-500:],
        }), 502

    try:
        metric_data = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        return jsonify({
            "error": "monitoring agent returned invalid JSON",
            "details": str(error),
        }), 502

    if not isinstance(metric_data, dict) or not REQUIRED_METRIC_FIELDS.issubset(metric_data):
        return jsonify({"error": "monitoring agent returned an incomplete response"}), 502

    try:
        captured_at = save_snapshot(metric_data)
    except (OSError, sqlite3.Error) as error:
        app.logger.exception("Could not save metrics snapshot")
        return jsonify({"error": "could not save metrics snapshot", "details": str(error)}), 500

    return jsonify({"captured_at": captured_at, **metric_data})


@app.get("/api/history")
def history():
    limit_text = request.args.get("limit", str(DEFAULT_HISTORY_LIMIT))
    try:
        limit = int(limit_text)
    except ValueError:
        return jsonify({"error": "limit must be a whole number"}), 400

    if limit < 1 or limit > MAX_HISTORY_LIMIT:
        return jsonify({
            "error": f"limit must be between 1 and {MAX_HISTORY_LIMIT}"
        }), 400

    try:
        initialize_database()
        connection = sqlite3.connect(DATABASE_PATH)
        connection.row_factory = sqlite3.Row
        try:
            rows = connection.execute(
                """
                SELECT id, captured_at, metrics_json
                FROM metric_snapshots
                ORDER BY id DESC
                LIMIT ?
                """,
                (limit,),
            ).fetchall()
        finally:
            connection.close()

        snapshots = [
            {
                "id": row["id"],
                "captured_at": row["captured_at"],
                "metrics": json.loads(row["metrics_json"]),
            }
            for row in rows
        ]
    except (OSError, sqlite3.Error, json.JSONDecodeError) as error:
        app.logger.exception("Could not read metrics history")
        return jsonify({"error": "could not read metrics history", "details": str(error)}), 500

    return jsonify({"count": len(snapshots), "snapshots": snapshots})


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "5000"))
    app.run(host="0.0.0.0", port=port, debug=False)