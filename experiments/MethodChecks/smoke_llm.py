"""Five distinct adapter smoke turns, never a comparison or quality benchmark.

Requires the three-row contract-bank.tsv created by MethodChecks and the explicitly
provided library profile. The fixture is deliberately trivial and public to the
implementation; it supplies no held-out evidence or general accuracy estimate.
"""
import argparse
import json
import time
import urllib.request
import uuid
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://127.0.0.1:5188')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = []

    def api(path, data=None):
        body = json.dumps(data).encode() if data is not None else None
        req = urllib.request.Request(args.url + path, body, {'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=180) as response:
            return json.load(response)

    config = api('/api/methods')
    assert all(any(m['id'] == name and m['available'] for m in config['methods']) for name in ('hybrid', 'llm_id', 'rewrite'))
    sessions = {}

    def turn(method, query, expected_provenance, expected_id=None):
        session = sessions.setdefault(method, api('/api/session', {'methodId': method})['sessionId']) if method not in sessions else sessions[method]
        t0 = time.perf_counter()
        result = api('/api/respond', {'methodId': method, 'sessionId': session,
                                     'input': query, 'requestId': str(uuid.uuid4())})
        rows.append({'method': method, 'input': query, 'elapsed_ms': (time.perf_counter()-t0)*1000, 'result': result})
        args.output.write_text(json.dumps({'scope': __doc__, 'configuration': config, 'turns': rows}, indent=2))
        assert result['provenance'] == expected_provenance, result
        if expected_id is not None:
            assert result['selectedId'] == expected_id, result
        meta = result['metadata']
        assert meta['promptTokens'] + meta['reservedOutputTokens'] <= meta['contextLimit']
        assert meta['usage']['prompt_tokens'] == meta['promptTokens'], ('template/tokenizer mismatch', meta)
        assert result['history'][-1] == {'user': query, 'character': result['response']}
        print(method, result['provenance'], repr(result['response']), flush=True)
        return result

    try:
        turn('hybrid', 'Hello.', 'selected', '0')
        turn('hybrid', 'What is the exact current price of a ticket to Mars?', 'generated')
        turn('llm_id', 'What should I call you?', 'selected', '2')
        turn('rewrite', 'Tell me the color of the book.', 'selected', '1')
        result = turn('rewrite', 'What color was it again?', 'selected', '1')
        assert 'book' in result['metadata']['rewrittenQuery'].lower()
        print(f'{len(rows)} actual Qwen adapter smoke turns passed; no comparative benchmark.', flush=True)
    finally:
        for session in sessions.values():
            api('/api/reset', {'sessionId': session})


if __name__ == '__main__':
    main()
