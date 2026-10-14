# SystemCare Shell API

This local Flask API runs the Linux monitoring agent in `os-engine/` and stores
successful metric snapshots in `data/systemcare.db`. The database and its table
are created automatically when metrics or history are first requested.

## Setup and run

From the repository root, build the C programs and install the Python dependency:

```sh
make -C os-engine
python3 -m pip install -r backend/requirements.txt
python3 backend/app.py
```

The API listens on port 5000 and is available locally at `http://localhost:5000`.
In GitHub Codespaces, open port 5000 from the **Ports** panel to use its forwarded
URL. Stop the server with `Ctrl+C`.

## Endpoints

- `GET /api/health` returns `{"status":"ok"}` when the API is running.
- `GET /api/metrics` runs `os-engine/scagent`, returns the live metrics, and saves
  that snapshot with a UTC timestamp.
- `GET /api/history` returns the newest 20 saved snapshots by default. Use
  `GET /api/history?limit=5` to choose a limit from 1 through 100.

Example checks from another terminal:

```sh
curl http://localhost:5000/api/health
curl http://localhost:5000/api/metrics
curl 'http://localhost:5000/api/history?limit=5'
```