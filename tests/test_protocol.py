import json
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from deepseek_smoke_test import parse_reply,payload,NoRedirect
class ProtocolTests(unittest.TestCase):
    def test_text_response(self):
        self.assertEqual(parse_reply(json.dumps({'choices':[{'message':{'content':'你好'}}]}).encode()),'你好')
    def test_empty_response_rejected(self):
        with self.assertRaises(ValueError): parse_reply(b'{"choices":[]}')
    def test_oversize_rejected(self):
        with self.assertRaises(ValueError): parse_reply(b' '*65537)
    def test_prompt_serialization(self):
        request=payload('deepseek-flash','A "quote"\nnext line')
        self.assertEqual(json.loads(json.dumps(request))['messages'][0]['content'],'A "quote"\nnext line')
        self.assertFalse(request['stream'])
        self.assertEqual(request['thinking']['type'],'disabled')
    def test_redirect_does_not_forward_auth(self):
        self.assertIsNone(NoRedirect().redirect_request(None,None,302,'',{},'https://elsewhere.invalid'))
if __name__=='__main__': unittest.main()
