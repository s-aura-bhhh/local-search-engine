# flask bridge for the indexer


import subprocess, json, os
from flask import Flask, request, jsonify, render_template

app = Flask(__name__)

INDEXER    = os.path.join("src", "indexer.exe")
INDEX_FILE = "index.bin"
DATA_DIR   = "data"


def build_index():
    print("building index...")
    r = subprocess.run([INDEXER, "build", DATA_DIR, INDEX_FILE],
                        capture_output=True, text=True)
    if r.stderr:
        print(r.stderr, end="")
    return r.returncode == 0


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/search")
def search():
    query = request.args.get("q", "").strip()
    if not query:
        return jsonify({"error": "empty query", "phrase_hits": [], "ranked_hits": []}), 400

    try:
        r = subprocess.run([INDEXER, "search", INDEX_FILE, query],
                            capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return jsonify({"error": "search timed out"}), 504
    except FileNotFoundError:
        return jsonify({"error": f"indexer not found at '{INDEXER}', build it first"}), 500

    try:
        return jsonify(json.loads(r.stdout))
    except json.JSONDecodeError:
        return jsonify({"error": r.stderr or "invalid response from engine"}), 500


@app.route("/rebuild", methods=["POST"])
def rebuild():
    if build_index():
        return jsonify({"status": "ok", "message": "index rebuilt"})
    return jsonify({"status": "error", "message": "build failed, check server logs"}), 500


if __name__ == "__main__":
    if not os.path.exists(INDEX_FILE):
        build_index()
    app.run(debug=True, port=5000)