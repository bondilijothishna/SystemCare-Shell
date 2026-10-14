# SystemCare Shell Dashboard

## Run in GitHub Codespaces

Start the Flask API in one terminal from the repository root:

```sh
python3 -m pip install -r backend/requirements.txt
python3 backend/app.py
```

In a second terminal, build and run the frontend:

```sh
cd frontend
npm install
npm run dev
```

Open the forwarded Vite port shown in the Codespaces **Ports** panel (Vite uses
port 5173 by default). The development server proxies `/api` requests to the
Flask API at `http://localhost:5000`.

The dashboard requests live metrics and recent snapshots from the API. It
refreshes every five seconds; use **Refresh now** for an immediate update.

## Production build

From `frontend/`, run:

```sh
npm run build
npm run preview
```