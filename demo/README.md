# Demo UI (not production)

- `index.html` — open directly for a JavaScript toy book (ladder, fills, latency).
- `server.py` — `python3 demo/server.py` then http://127.0.0.1:8765. Uses `../build/lob_demo --json` when that binary exists.
- `lob_demo.cpp` — real C++ `OrderBook` / `MatchingEngine`; stdin commands, optional `--json`.
- `make_terminal_gif.py` — regenerates `docs/assets/terminal.gif` from captured logs in `demo/assets/`.
