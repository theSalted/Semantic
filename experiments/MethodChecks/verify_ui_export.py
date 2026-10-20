"""Check an exported two-message UI smoke transcript, without quality scoring."""
import argparse
import csv
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('export', type=Path)
parser.add_argument('--bank', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
data = json.loads(args.export.read_text())
assert data['format'] == 'semantic-human-comparison-v2'
with args.bank.open() as f:
    bank = list(csv.DictReader(f, delimiter='\t'))
conversations = [c for c in data['conversations'] if any(c['methods'].values())]
assert len(conversations) == 1
conversation = conversations[0]
methods = conversation['methods']
expected_inputs = ['Hello there.', 'Thank you for the greeting.']
for method, turns in methods.items():
    assert [t['user'] for t in turns] == expected_inputs, method
    history = []
    for turn in turns:
        assert not turn.get('error'), (method, turn.get('error'))
        assert turn['methodId'] == method and turn['bankRevision'] == conversation['bankRevision']
        assert turn['provenance'] in ('selected', 'generated')
        if turn['provenance'] == 'selected':
            assert turn['response'] == bank[int(turn['selectedId'])]['response']
        else:
            assert method == 'hybrid' and turn['selectedId'] is None
        history.append({'user': turn['user'], 'character': turn['response']})
        assert turn['history'] == history, (method, 'history does not match actual rendered turns')
        if method in ('light_biranker', 'poly_encoder'):
            assert turn['metadata']['candidateScope'] == 'full_bank'
            assert turn['metadata']['candidateCount'] == len(bank)
            assert turn['metadata']['adapter']['candidate_count'] == len(bank)
result = {'scope': 'UI contract smoke only; no quality scoring', 'methods': list(methods),
          'shared_messages': 2, 'completed_method_turns': 2*len(methods),
          'checks': ['identical ordered inputs', 'method-bound provenance', 'exact authored ID/text',
                     'history equals each method actual replies', 'stable bank revision',
                     'paper full-bank candidate count', 'no method errors'], 'passed': True}
args.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result))
