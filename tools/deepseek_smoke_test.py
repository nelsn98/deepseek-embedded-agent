"""One DeepSeek HTTPS text request; Python standard library only."""
import argparse
import getpass
import json
import os
import urllib.request
import urllib.error

URL = 'https://api.deepseek.com/chat/completions'
LIMIT = 65536

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

def payload(model, prompt):
    return {'model': model, 'messages': [{'role': 'user', 'content': prompt}],
            'stream': False, 'thinking': {'type': 'disabled'}, 'max_tokens': 256}

def parse_reply(raw):
    if len(raw) > LIMIT:
        raise ValueError('Response exceeds 64 KiB')
    data = json.loads(raw)
    choices = data.get('choices') or []
    text = choices[0].get('message', {}).get('content') if choices else None
    if not isinstance(text, str) or not text.strip():
        raise ValueError('Response lacks text content')
    return text

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', default='deepseek-flash')
    p.add_argument('--prompt', default='Say hello in one short sentence.')
    args = p.parse_args()
    key = (os.environ.get('DEEPSEEK_API_KEY') or getpass.getpass('DeepSeek API key (hidden): ')).strip()
    if not key or any(ord(c) < 33 or ord(c) > 126 for c in key):
        print('Invalid input: copy only the DeepSeek API key.'); return 1
    request = urllib.request.Request(URL, data=json.dumps(payload(args.model,args.prompt)).encode(),
        headers={'Authorization': 'Bearer '+key, 'Content-Type': 'application/json'}, method='POST')
    print('Model:',args.model)
    try:
        with urllib.request.build_opener(NoRedirect()).open(request, timeout=60) as response:
            raw = response.read(LIMIT + 1)
            print('HTTP status:',response.status)
        print('DeepSeek:',parse_reply(raw))
        print('DEEPSEEK V0 PASS'); return 0
    except urllib.error.HTTPError as e:
        hints={400:'Check model/request parameters.',401:'DeepSeek API key rejected.',
               402:'Check DeepSeek account balance.',429:'Rate limited; retry later.'}
        print(f'HTTP {e.code}: {hints.get(e.code,"Server rejected request.")}'); return 1
    except Exception as e:
        # Never print a request object or raw error body containing credentials.
        print(f'DEEPSEEK V0 FAIL ({type(e).__name__}); check TLS/network/response.'); return 1

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
