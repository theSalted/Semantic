#!/usr/bin/env python3
"""Additional adversarial dialogue probes, not a production quality benchmark.
Full replies retained for manual review; no model-as-judge and no output shortening.
"""
import argparse,csv,hashlib,json,math,time,urllib.request,urllib.error,uuid
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label',required=True)
parser.add_argument('--bank',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True,help='Fresh output directory; complete replies need manual review')
parser.add_argument('--port',type=int,default=5193)
args=parser.parse_args()
bankhash=hashlib.sha256(args.bank.read_bytes()).hexdigest()
if bankhash != '30971f18424c19ba1b5fcc2f30936c3eaf15ecee143ddbe67a4ea643e519aceb':
 parser.error('Expected the measured 33-row Pell bank; checks refer to its exact authored IDs.')
folder=args.output;folder.mkdir(parents=True,exist_ok=False)
with args.bank.open() as bank_file:authored=list(csv.DictReader(bank_file,delimiter='\t'))
rows=[]
def req(route,data):
 with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{args.port}'+route,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=90) as r:return json.load(r)
def session():return req('/api/session',{'bankSha256':bankhash})['sessionId']
def close(s):req('/api/close',{'sessionId':s})
def turn(s,q,case,review,contains=(),not_contains=(),selected=None,not_selected=(),empty_memory=False,evicted=None,phase='quality'):
 start=time.perf_counter();r={'case':case,'input':q,'manualReviewRequirement':review,'phase':phase}
 try:
  x=req('/api/respond',{'sessionId':s,'input':q,'requestId':str(uuid.uuid4()),'bankSha256':bankhash})
  r.update(elapsedMs=(time.perf_counter()-start)*1000,result=x,response=x['response'],path=x['metadata']['path'],modelInvoked=x['metadata']['modelInvoked'])
  checks={'nonempty':bool(x['response'].strip()),'history_committed':x['history'][-1]['user']==q and x['history'][-1]['character']==x['response']}
  if x['provenance']=='selected':
   a=authored[int(x['selectedId'])];checks['authored_exact']=x['response']==a['response'] and x['emoteTag']==a['emote_tag']
  else:checks['generated_no_tag']=x['selectedId'] is None and x['emoteTag'] is None
  checks.update({'contains_'+v:v.casefold() in x['response'].casefold() for v in contains})
  checks.update({'excludes_'+v:v.casefold() not in x['response'].casefold() for v in not_contains})
  if selected is not None:checks['selected_expected']=x['selectedId']==str(selected)
  if not_selected:checks['excluded_authored_ids']=x['selectedId'] not in [str(v) for v in not_selected]
  if empty_memory:checks['memory_empty']=x['metadata']['sessionMemory']==[]
  if evicted:checks['declaration_evicted']=all(evicted not in t['user'] for t in x['metadata']['historyUsed'])
  r.update(checks=checks,checksPassed=all(checks.values()))
 except Exception as e:r.update(elapsedMs=(time.perf_counter()-start)*1000,error=str(e),checksPassed=False)
 rows.append(r);(folder/f'{len(rows):03d}.json').write_text(json.dumps(r,indent=2)+'\n')
 m=r.get('result',{}).get('metadata',{});t=m.get('llamaTimings') or {}
 print(json.dumps({k:r.get(k) for k in ['case','elapsedMs','path','response','checksPassed','error']}|{'outputTokens':t.get('predicted_n'),'prefillMs':t.get('prompt_ms'),'decodeMs':t.get('predicted_ms')}),flush=True)
 return r

s=session()
try:
 turn(s,'May I have a beer, please?','beer-paraphrase','Select exact beer reply.',selected=14)
 turn(s,'Actually, I do not want any alcohol. What could you offer?','alcohol-negation','Offer a non-alcoholic choice or clarify; do not serve alcohol.',not_selected=(14,15,16,17,18,19))
 turn(s,'Why did you choose that?','negation-followup','Reference non-alcoholic preference, not earlier beer request.')
finally:close(s)
s=session()
try:
 turn(s,'My name is Selene. My favorite drink is black tea.','facts-declared','Acknowledge without changing identity.')
 turn(s,'My friend is called Varo. Which of those names is mine?','friend-not-player','Identify player as Selene, friend as Varo.',contains=('Selene',))
 turn(s,'Actually call me Nyx. My favorite drink is cocoa.','two-facts-correction','Acknowledge current corrected facts.')
 turn(s,'Use my current name and drink in a greeting.','corrected-greeting','Use Nyx and cocoa, not obsolete values.',contains=('Nyx','cocoa'),not_contains=('Selene','black tea'))
 req('/api/reset',{'sessionId':s})
 turn(s,'Which name and drink do you remember for me?','reset-broad-recall','Honest unknown name/drink after reset.',not_contains=('Nyx','Selene','cocoa','black tea'),empty_memory=True)
finally:close(s)
s=session()
try:
 turn(s,'In a story a traveler says, "My name is Rhea." I am quoting them.','quoted-identity','Do not extract the quoted name as player identity.',empty_memory=True)
 turn(s,'What facts about me, as distinct from that traveler, do you know?','quoted-broad-recall','No attribution of Rhea to player; clarify unknown personal facts.',empty_memory=True)
 turn(s,"What's my name?",'quoted-direct-recall','Explicit unknown player name.',contains=("haven't told",),empty_memory=True)
finally:close(s)
s=session()
try:
 turn(s,'Who is the mayor of this town?','unknown-mayor','Do not invent a mayor name; state uncertainty or ask.')
 turn(s,'Where exactly is the closest hospital?','unknown-location','Do not invent a local hospital or directions.')
 turn(s,'What day and year is it?','unknown-date','Do not invent a date or era.')
finally:close(s)
s=session()
try:
 turn(s,'My name is Arin. My favorite color is orange.','eviction-declaration','Store both explicit facts.')
 for i in range(24):turn(s,"I'll have whiskey",f'eviction-fill-{i+1}','Real fast-path history insertion.',selected=16,phase='history-filler')
 turn(s,'Tell me my name and favorite color.','evicted-broad-memory','Recall both ledger facts after actual transcript eviction.',contains=('Arin','orange'),evicted='My name is Arin')
 turn(s,'Actually, call me Wren.','evicted-correction','Replace the old name with Wren.',contains=('Wren',))
 turn(s,'Use my current name and favorite color in a short sentence.','evicted-corrected-broad-memory','Use corrected name and retained color.',contains=('Wren','orange'),not_contains=('Arin',))
finally:close(s)
s=session()
try:
 turn(s,'I lost a copper key.','lost-key','Acknowledge or ask for useful details; no invented location.')
 turn(s,'It was inside my green bag when I came here. Where did I last say it was?','key-history-recall','Recall green bag from actual prior utterance.',contains=('green bag',))
 turn(s,'Please distinguish what I told you from your guesses about where it might be.','evidence-vs-guess','Only the key and green bag are known, location guesses remain uncertain.')
 turn(s,'Give me four practical checks I can do with the information I gave you.','four-steps','Provide four useful checks with no invented local places; complete all requested steps.')
finally:close(s)
s=session()
try:
 turn(s,'Who owns the Last Drop?','known-owner','Pell Cask owns bar.',contains=('Pell',))
 turn(s,'Can you describe how it looks inside?','unknown-interior','Do not invent detailed decor absent profile/history.')
 turn(s,'Please summarize only facts about your bar you know for certain.','grounded-summary','Use profile or established facts only.')
finally:close(s)
s=session()
try:
 turn(s,'My name is Ivo.','forget-intro','Remember Ivo.',contains=('Ivo',))
 turn(s,'Forget my name.','forget-command','Forget name and acknowledge.',empty_memory=True)
 turn(s,'What is my name?','forgotten-direct','Unknown after explicit forget.',contains=("haven't told",),empty_memory=True)
 turn(s,'Please call me by my name.','forgotten-broad','Do not resurrect forgotten Ivo from transcript.',not_contains=('Ivo',),empty_memory=True)
finally:close(s)

def summary(a):
 v=sorted(r['elapsedMs'] for r in a)
 return {'n':len(a),'p50Ms':v[math.ceil(.5*len(v))-1] if v else None,'p95Ms':v[math.ceil(.95*len(v))-1] if v else None,'p99Ms':v[math.ceil(.99*len(v))-1] if v else None,'maxMs':max(v) if v else None,'under1s':sum(t<1000 for t in v),'failures':[r['case'] for r in a if not r['checksPassed']]}
result={'label':args.label,'llm':summary([r for r in rows if r.get('modelInvoked')]),'allAttempts':summary(rows),'errors':[r['case'] for r in rows if 'error' in r],'manualSemanticReviewPending':True}
(folder/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print('SUMMARY '+json.dumps(result),flush=True)

raise SystemExit(1 if any(not r["checksPassed"] for r in rows) else 0)
