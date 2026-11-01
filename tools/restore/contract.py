#!/usr/bin/env python3
"""Never-merge restore experiment transport; no deployment or host authority."""
from __future__ import annotations
import hashlib
import http.client
import json
import math
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

ORDER = ((1, 'ABDCHGEF'), (2, 'FEGHCDBA'), (3, 'CDBAEFHG'))
ARMS = {name: {'engine': 'incumbent' if name in 'ACEG' else 'candidate',
               'schedule': 'L' if name in 'ABEF' else 'F',
               'keep_warm_ms': 60000 if name in 'ABCD' else 0} for name in 'ABCDEFGH'}
IDENTITY = ('deployment_profile', 'model_id', 'model_artifact_sha256', 'binary_sha256',
            'upstream_base_sha', 'patch_stack_sha', 'source_dirty', 'build_profile', 'config_sha256')

def cells(window):
    if window not in (1, 2): raise ValueError('window must be 1 or 2')
    all_cells = [(block, cell) for block, order in ORDER for cell in order]
    return all_cells[(window-1)*12:window*12]

def identity(status):
    if (status.get('artifact_type') != 'ninfer_server_status' or status.get('schema_version') not in (1, 2)
            or status.get('status') != 'ok'): raise ValueError('invalid authenticated status')
    for group in ('identity', 'runtime', 'scheduler', 'cache', 'mtp'):
        if not isinstance(status.get(group), dict): raise ValueError('missing status group')
    result = {key: status['identity'][key] for key in IDENTITY}
    if result['source_dirty'] is not False: raise ValueError('dirty source')
    if any(not isinstance(result[k], str) or not result[k] for k in IDENTITY if k != 'source_dirty'):
        raise ValueError('incomplete identity')
    return result

def counter(status):
    result = status['scheduler']['computed_prefill_tokens']
    if type(result) is not int or result < 0: raise ValueError('invalid prefill counter')
    return result

def fixture(block):
    keys = {name: str(100000 + int(hashlib.sha256(f'p17-restore-{block}-{name}'.encode()).hexdigest()[:12], 16) % 900000)
            for name in ('ORCHID', 'COLOR', 'SIGIL')}
    entries = ['Operations ledger entry %d: throughput nominal, cache warm, retrieval verified. ' % i
               for i in range(max(3, (48000 * 3 // 4) // 11))]
    for name, pos in zip(keys, (.05, .5, .95)):
        entries[min(len(entries)-1, int(len(entries)*pos))] += f'Ledger key {name}={keys[name]}. '
    return {'block': block, 'base_tokens_requested': 48000, 'keys': keys,
            'input': [{'role':'user', 'content':[{'type':'input_text', 'text':
              'Hold this operations ledger in context for later analysis; reply OK only.\n' + ''.join(entries)}]}]}

def retrieval(keys, label):
    return f"{label}: quote the {', '.join(keys)} ledger keys from the operations ledger exactly, as KEY=VALUE separated by spaces, with no other words."

def oracle(keys, document):
    text = ''.join(p['text'] for item in document.get('output', []) for p in item.get('content', []) or []
                   if isinstance(p, dict) and isinstance(p.get('text'), str))
    missing = [name for name, value in keys.items() if value not in text]
    return {'exact': not missing, 'missing_keys': missing, 'output_text': text,
            'output_sha256': hashlib.sha256(text.encode()).hexdigest()}

@dataclass(frozen=True)
class Stamp:
    instance: str
    domain: str
    ns: int

def elapsed(begin, end):
    if (begin.instance, begin.domain) != (end.instance, end.domain):
        raise ValueError('cannot subtract unrelated clocks')
    if end.ns < begin.ns: raise ValueError('clock regressed')
    return (end.ns - begin.ns) / 1e9

def join_key(event):
    request = event['request']
    result = (event['server_instance_id'], request['http_request_id'], request['response_id'])
    if any(not isinstance(value, str) or not value for value in result): raise ValueError('missing join identity')
    return result

@dataclass
class Budget:
    window_start: float
    chain_start: float
    now: object = time.monotonic
    def remaining(self, individual):
        value = min(individual, self.chain_start+300-self.now(), self.window_start+3000-self.now())
        if value <= 0: raise TimeoutError('chain/window budget exhausted')
        return value
    def admit(self): return self.now() < self.window_start + 2700
    def action(self):
        age = self.now() - self.window_start
        return 'hard-stop' if age >= 7200 else 'restore' if age >= 3600 else 'cancel' if age >= 3000 else 'stop-new' if age >= 2700 else 'run'

class Events:
    def __init__(self, emit, instance):
        self.emit, self.instance = emit, instance
    def __call__(self, name, **fields):
        self.emit(dict(event=name, clock_domain='observer.monotonic', units='ns',
                       process_instance=self.instance, monotonic_ns=time.monotonic_ns(),
                       wall_ns=time.time_ns(), **fields))

class Transport:
    """One attempt per operation. A deadline limits total reads, not just each socket read."""
    def __init__(self, url, key, session, events, budget):
        self.url, self.key, self.session, self.events, self.budget = urlsplit(url), key, session, events, budget
        self.sequence = 0
        self.lock = threading.Lock()
    def request(self, path, payload=None, timeout=120, method=None):
        with self.lock:
            self.sequence += 1
            sequence = self.sequence
        deadline = time.monotonic() + self.budget.remaining(timeout)
        method = method or ('POST' if payload is not None else 'GET')
        self.events('http.begin', sequence=sequence, path=path, method=method)
        conn = http.client.HTTPConnection(self.url.hostname, self.url.port, timeout=max(.001, deadline-time.monotonic()))
        try:
            conn.request(method, path, body=json.dumps(payload).encode() if payload is not None else None,
                         headers={'Authorization':'Bearer '+self.key, 'Content-Type':'application/json',
                                  'X-NInfer-Session':self.session})
            response = conn.getresponse()
            # HTTP status/header arrival is the first observable response byte boundary.
            self.events('http.first_byte', sequence=sequence, status=response.status,
                        http_request_id=response.getheader('x-request-id'))
            chunks, size = [], 0
            while True:
                remaining = deadline-time.monotonic()
                if remaining <= 0: raise TimeoutError('HTTP total deadline exceeded')
                if conn.sock is not None: conn.sock.settimeout(remaining)
                chunk = response.read1(65536)
                if not chunk: break
                size += len(chunk)
                if size > 16*1024*1024: raise RuntimeError('HTTP evidence exceeds 16 MiB')
                chunks.append(chunk)
            document = json.loads(b''.join(chunks) or b'{}')
            self.events('http.end', sequence=sequence, status=response.status, document=document,
                        http_request_id=response.getheader('x-request-id'))
            if response.status >= 400: raise RuntimeError(f'HTTP {response.status} on {path}')
            return document
        except Exception as error:
            self.events('http.error', sequence=sequence, error=type(error).__name__, detail=str(error))
            raise
        finally: conn.close()
    def status(self, timeout): return self.request('/v1/ninfer/status', timeout=timeout)
    def respond(self, content, previous, max_output):
        body = dict(model='q38-ninfer', input=content, store=True, temperature=0,
                    max_output_tokens=max_output, ninfer_session=self.session)
        if previous: body['previous_response_id'] = previous
        return self.request('/v1/responses', body)
    def save(self):
        result = self.request('/v1/ninfer/checkpoints', {'session_sha256':self.session})
        if result.get('committed') is not True: raise RuntimeError('save not committed')
        status = self.request('/v1/ninfer/checkpoints/status')
        if status.get('state') != 'available' or not status.get('generation'): raise RuntimeError('saved generation missing')
        return {'save': result, 'manifest_status': status}

def ready(status, schedule, budget, events, now=time.monotonic, sleep=time.sleep):
    timeout, delay, limit = (10., 3., 2400) if schedule == 'L' else (.250, .050, 2400)
    started = now(); deadline = started + budget.remaining(120)
    for attempt in range(limit):
        if now() >= deadline: break
        events('status.attempt.begin', attempt=attempt, timeout=min(timeout, deadline-now()))
        try:
            document = status(min(timeout, deadline-now()))
            identity(document)
            events('status.attempt.end', attempt=attempt, status=document)
            return document, now()-started
        except (OSError, http.client.HTTPException) as error:
            events('status.attempt.end', attempt=attempt, error=type(error).__name__, detail=str(error))
        interval = min(delay, max(0., deadline-now()))
        events('status.sleep.begin', seconds=interval)
        sleep(interval)
        events('status.sleep.end')
    raise TimeoutError('readiness deadline/attempt cap exhausted')

def measured(transport, previous, prompt, events):
    stop = threading.Event(); changed = []; failure = []; last = None
    def observe():
        nonlocal last
        while not stop.is_set():
            try: value = transport.request('/v1/ninfer/checkpoints/status', timeout=30)
            except Exception as error: value = {'error':type(error).__name__, 'detail':str(error)}
            events('checkpoint.observation', status=value)
            if value != last: changed.append(value); last = value
            stop.wait(1.)
    thread = threading.Thread(target=observe)
    thread.start(); started = time.monotonic()
    try: document = transport.respond(prompt, previous, 256)
    finally:
        stop.set(); thread.join(timeout=31)
        if thread.is_alive(): raise RuntimeError('checkpoint observer did not stop')
    return document, {'wall_s':round(time.monotonic()-started, 3), 'status_polls':changed}

def self_test():
    assert cells(1) == [(1,c) for c in 'ABDCHGEF'] + [(2,c) for c in 'FEGH']
    assert cells(2) == [(2,c) for c in 'CDBA'] + [(3,c) for c in 'CDBAEFHG']
    assert len({(b,c) for w in (1,2) for b,c in cells(w)}) == 24
    tick = [0.]; b = Budget(0,0,lambda:tick[0])
    for value, action in ((2699,'run'),(2700,'stop-new'),(3000,'cancel'),(3600,'restore'),(7200,'hard-stop')):
        tick[0]=value; assert b.action()==action
    tick[0]=299.; assert b.remaining(120)==1
    for a,z in ((Stamp('x','a',1),Stamp('y','a',2)),(Stamp('x','a',1),Stamp('x','b',2))):
        try: elapsed(a,z)
        except ValueError: pass
        else: raise AssertionError('cross-clock subtraction admitted')
    assert elapsed(Stamp('x','a',10),Stamp('x','a',1000000010)) == 1
    a={'server_instance_id':'one','request':{'http_request_id':'h','response_id':'r'}}
    z=dict(a,server_instance_id='two'); assert join_key(a)!=join_key(z)
    valid={'artifact_type':'ninfer_server_status','schema_version':2,'status':'ok',
           'identity':{k:False if k=='source_dirty' else k for k in IDENTITY},
           'runtime':{},'scheduler':{'computed_prefill_tokens':0},'cache':{},'mtp':{}}
    for schedule, durations in (('L',[.0005,10.01,10.01,.002]),('F',[.25,.25,.002])):
        tick[0]=0.; b=Budget(0,0,lambda:tick[0]); attempts=iter(durations); calls=[]
        def status(timeout):
            duration=next(attempts); tick[0]+=duration
            if duration != .002: raise TimeoutError('fake readiness wait')
            return valid
        doc, wait=ready(status,schedule,b,lambda *a,**k:calls.append((a,k)),
                       now=lambda:tick[0], sleep=lambda v:tick.__setitem__(0,tick[0]+v))
        expected=sum(durations)+(3 if schedule=='L' else .05)*(len(durations)-1)
        assert math.isclose(wait,expected,abs_tol=1e-10)
        assert sum(a[0]=='status.attempt.begin' for a,k in calls)==len(durations)
    print(json.dumps({'passed':True,'tests':['frozen-order','deadline-boundaries','chain-cap','clock-domains','instance-ID-join','L/F-poll-lattice']}))

if __name__ == '__main__': self_test()
