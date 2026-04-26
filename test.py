import requests
import json
import sys

# Server configuration
URL = "http://127.0.0.1:8080/"

# Prepare the test queries (based on the sample text you provided)
queries = [
    {"query": "companies ordinance", "k": 3},
    {"query": "financial secretary regulations", "k": 2},
    {"query": "sole director death", "k": 1},
    {"query": "paperless holding and transfer", "k": 2},
    {"query": "registered holder of the shares", "k": 3}
]

# Convert queries to a JSON Lines string
payload = "\n".join(json.dumps(q) for q in queries) + "\n"

print("--- Sending Request ---")
print(f"URL: POST {URL}")
print("Body (JSON Lines):")
print(payload.strip())
print("-" * 23)

try:
    # Send the POST request with the body
    # Note: While POST requests with bodies are usual in REST APIs,
    # the C++ httplib server and Python's requests library fully support it.
    response = requests.post(URL, data=payload, headers={"Content-Type": "application/jsonlines"})

    # Check if the request was successful
    response.raise_for_status()

    print("\n--- Server Response ---")
    print(f"Status Code: {response.status_code}")

    # Parse and display the response line by line
    lines = response.text.strip().split('\n')

    for i, (query_obj, result_line) in enumerate(zip(queries, lines)):
        query_str = query_obj["query"]
        k = query_obj["k"]
        print(f"\nResults for query [{i+1}]: '{query_str}' (Top {k})")

        try:
            results = json.loads(result_line)
            if not results:
                print("  No matches found.")
            for rank, doc in enumerate(results):
                print(f"  Rank {rank+1}: Doc ID {doc['id']:<10} | Score: {doc['score']:.4f}")
        except json.JSONDecodeError:
            print(f"  Failed to parse JSON response line: {result_line}")

except requests.exceptions.ConnectionError:
    print(f"\nError: Could not connect to the server at {URL}.")
    print("Make sure the C++ BM25 server is compiled and running on port 8080.")
    sys.exit(1)
except requests.exceptions.RequestException as e:
    print(f"\nHTTP Request failed: {e}")
    if response is not None:
        print(f"Response text: {response.text}")
    sys.exit(1)
