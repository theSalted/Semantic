#!/usr/bin/env python3
"""Bounded real Unity-bridge latency workload. Synthetic player facts only.

Timing starts before HTTP serialization/send and ends after the complete response
body has been received and parsed. Includes retrieval, queue, Qwen, validation,
commit and loopback transport; excludes Unity frame/render time and session setup.
Concurrency one, no retries, real rolling history. Failures remain in results.
"""
import argparse, csv, hashlib, json, math, time, urllib.request, urllib.error, uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
parser.add_argument('--bank', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True, help='Fresh directory for complete responses and summary')
parser.add_argument('--rounds', type=int, default=5)
parser.add_argument('--port', type=int, default=5193)
args = parser.parse_args()
if args.rounds < 1: parser.error('--rounds must be positive')
BANK_HASH = hashlib.sha256(args.bank.read_bytes()).hexdigest()
if BANK_HASH != '30971f18424c19ba1b5fcc2f30936c3eaf15ecee143ddbe67a4ea643e519aceb':
    parser.error('This workload requires the measured 33-row Pell bank; its expected IDs are bank-specific.')
with args.bank.open() as bank_file:
    ROWS = list(csv.DictReader(bank_file, delimiter='\t'))
outdir = args.output
outdir.mkdir(parents=True, exist_ok=False)
records=[]

def request(route, data=None):
    url=f'http://127.0.0.1:{args.port}'+route
    req=urllib.request.Request(url, data=None if data is None else json.dumps(data).encode(), headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=90) as r: return json.load(r)

def session(): return request('/api/session', {'bankSha256':BANK_HASH})['sessionId']
def close(s): request('/api/close', {'sessionId':s})
def reset(s): request('/api/reset', {'sessionId':s})

def turn(s, query, case, phase, expected=None):
    start=time.perf_counter()
    row={'case':case,'phase':phase,'input':query}
    try:
        result=request('/api/respond', {'sessionId':s,'input':query,'requestId':str(uuid.uuid4()),'bankSha256':BANK_HASH})
        row['elapsedMs']=(time.perf_counter()-start)*1000
        row['result']=result
        meta=result['metadata']
        row.update(path=meta['path'], modelInvoked=meta['modelInvoked'], response=result['response'],provenance=result['provenance'])
        checks={'nonempty':bool(result['response'].strip()),'history_committed':result['history'][-1]['user']==query and result['history'][-1]['character']==result['response']}
        if result['provenance']=='selected':
            authored=ROWS[int(result['selectedId'])]
            checks['exact_authored_text_tag']=result['response']==authored['response'] and result['emoteTag']==authored['emote_tag']
        else:
            checks['generated_has_no_authored_tag']=result['selectedId'] is None and result['emoteTag'] is None
        if expected:
            checks.update(expected(result))
        row['checks']=checks
        row['checksPassed']=all(checks.values())
    except Exception as e:
        row.update(elapsedMs=(time.perf_counter()-start)*1000,error=str(e),checksPassed=False)
    records.append(row)
    (outdir/f'{len(records):03d}.json').write_text(json.dumps(row,indent=2)+'\n')
    m=row.get('result',{}).get('metadata',{}); timing=m.get('llamaTimings') or {}
    print(json.dumps({k:row.get(k) for k in ['case','phase','elapsedMs','path','response','checksPassed','error']}|{'prompt':timing.get('prompt_n'),'cached':timing.get('cache_n'),'output':timing.get('predicted_n'),'prefillMs':timing.get('prompt_ms'),'decodeMs':timing.get('predicted_ms')}),flush=True)
    return row

def contains(*values):
    return lambda r:{'contains_'+value:value.lower() in r['response'].lower() for value in values}
def exact(value): return lambda r:{'expected_response':r['response']==value}

request('/health')
# First bridge request after server start is reported separately, not silently discarded.
s=session()
turn(s,"I'll have a beer",'first-request','first-request',lambda r:{'beer_selected':r['selectedId']=='14'})
close(s)

names=[('Mira','Jun','tea'),('Nora','Eli','cider'),('Ravi','Sam','coffee'),('Lina','Ash','juice'),('Omar','Kai','cocoa')]
for n in range(args.rounds):
    name, corrected, drink=names[n%len(names)]
    phase='warm'
    prefix=f'round-{n+1}'
    s=session()
    try:
        turn(s,"I'll have whiskey",prefix+'-fast-control','retrieval-control')
    finally: close(s)
    s=session()
    try:
        turn(s,"I'll have a beer",prefix+'-authored-beer',phase,lambda r:{'beer_selected':r['selectedId']=='14'})
        turn(s,'Who are you?',prefix+'-authored-identity',phase,lambda r:{'identity_authored':r['selectedId'] in ('4','5')})
        turn(s,'Do you drink? Have one with me',prefix+'-authored-duty',phase)
    finally: close(s)
    s=session()
    try:
        turn(s,"What's my name?",prefix+'-unknown-name',phase,exact("You haven't told me your name yet."))
        turn(s,f'My name is {name}. My favorite drink is {drink}.',prefix+'-declare-facts',phase)
        turn(s,'What are my name and favorite drink?',prefix+'-two-facts',phase,contains(name,drink))
        turn(s,f'Actually, call me {corrected}.',prefix+'-correct-name',phase,exact(f"All right, I'll call you {corrected}."))
        turn(s,"What's my name?",prefix+'-corrected-recall',phase,exact(f'You told me your name is {corrected}.'))
        reset(s)
        turn(s,"What's my name?",prefix+'-reset-name',phase,lambda r:{'reset_unknown':r['response']=="You haven't told me your name yet.",'reset_no_history':r['metadata']['historyUsed']==[],'reset_no_memory':r['metadata']['sessionMemory']==[]})
    finally: close(s)
    s=session()
    try:
        turn(s,'I lost a blue scarf on my way here. Can you help me think where to look?',prefix+'-generated-lost',phase)
        turn(s,'What did I lose?',prefix+'-lost-recall',phase,contains('blue scarf'))
        turn(s,'I last had it just before I reached your door. Give me three practical steps to look for it, without assuming where it is.',prefix+'-long-fallback',phase)
        turn(s,'Which parts of that advice depend on things I actually told you?',prefix+'-context-followup',phase)
    finally: close(s)
    s=session()
    try:
        for i,q in enumerate(['Who are you?','What is this place?','What do you have?',"I'll have a beer",'Got anything to eat?',"My favorite color is blue. I don't drink alcohol.",'Can you remind me which drink I ordered earlier, and my favorite color?']):
            expectation = contains('beer','blue') if i==6 else None
            if i==0: expectation=lambda r:{'identity_authored':r['selectedId'] in ('4','5')}
            if i==2: expectation=lambda r:{'availability_authored':r['selectedId'] in ('11','12')}
            if i==3: expectation=lambda r:{'beer_selected':r['selectedId']=='14'}
            turn(s,q,prefix+f'-history-{i+1}',phase,expectation)
    finally: close(s)

def pct(values,q):
    values=sorted(values); return values[max(0,math.ceil(q*len(values))-1)] if values else None
def summarize(rows):
    values=[r['elapsedMs'] for r in rows]
    return {'n':len(rows),'p50Ms':pct(values,.5),'p95Ms':pct(values,.95),'p99Ms':pct(values,.99),'maxMs':max(values) if values else None,'under1Second':sum(v<1000 for v in values),'errors':sum('error' in r for r in rows),'checksFailed':[r['case'] for r in rows if not r['checksPassed']]}
summary={'label':args.label,'rounds':args.rounds,'bankSha256':BANK_HASH,'quantileMethod':'nearest-rank','concurrency':1,'warmLlm':summarize([r for r in records if r['phase']=='warm' and r.get('modelInvoked')]),'warmUnconstrainedLlm':summarize([r for r in records if r['phase']=='warm' and r.get('path')=='qwen-hybrid']),'warmMemoryConstrainedLlm':summarize([r for r in records if r['phase']=='warm' and r.get('path')=='qwen-memory']),'warmAuthoredLlm':summarize([r for r in records if r['phase']=='warm' and r.get('modelInvoked') and r.get('provenance')=='selected']),'warmGeneratedLlm':summarize([r for r in records if r['phase']=='warm' and r.get('modelInvoked') and r.get('provenance')=='generated']),'retrievalControls':summarize([r for r in records if r['phase']=='retrieval-control']),'firstRequest':summarize([r for r in records if r['phase']=='first-request']),'allErrors':[r['case'] for r in records if 'error' in r],'allChecksFailed':[r['case'] for r in records if not r['checksPassed']]}
summary['allWarmAttempts']=summarize([r for r in records if r['phase']=='warm'])
summary['failurePolicy']='Keep every failed attempt; failed durations are not successful completion latencies. Any error prevents an SLO pass.'
(outdir/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print('SUMMARY '+json.dumps(summary),flush=True)

# Automated success remains insufficient for semantic approval; review complete replies.
raise SystemExit(1 if summary['allErrors'] or summary['allChecksFailed'] else 0)
