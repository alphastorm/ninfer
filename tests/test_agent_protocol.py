from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
from email.message import Message

from tools.smoke import agent_protocol as protocol
from tools.smoke import serve_contract as contract


class SchemaOutputTests(unittest.TestCase):
    def test_accepts_typed_schema_and_rejects_wrong_types_extra_keys_and_truncation(self) -> None:
        protocol.require_schema_output('{"city":"Paris","days":2}', True, 'Chat')
        for text, completed in (
            ('{"city":"Paris","days":true}', True),
            ('{"city":"Paris","days":2.0}', True),
            ('{"city":"Paris","days":2,"extra":0}', True),
            ('{"city":"London","days":2}', True),
            ('not JSON', True),
            ('{"city":"Paris","days":2}', False),
            (None, True),
        ):
            with self.subTest(text=text, completed=completed), self.assertRaises(protocol.ProtocolError):
                protocol.require_schema_output(text, completed, 'Chat')

    def test_schema_requests_use_both_upstream_wire_formats(self) -> None:
        answer = '{"city":"Paris","days":2}'
        replies = [
            {'choices': [{'message': {'content': answer}, 'finish_reason': 'stop'}]},
            {'status': 'completed', 'output': [{'type': 'message', 'content': [{'type': 'output_text', 'text': answer}]}]},
        ]
        with patch.object(protocol, 'json_response', side_effect=replies) as request:
            result = protocol.structured_output_checks('http://fixture', 'model', {'Authorization': 'Bearer fixture'}, {'ninfer_session': 'a' * 64})
        self.assertEqual(result, {'chat_json_schema': True, 'responses_json_schema': True})
        chat, responses = [call.args[3] for call in request.call_args_list]
        self.assertEqual(chat['response_format']['json_schema']['schema'], responses['text']['format']['schema'])
        self.assertFalse(chat['enable_thinking'])
        self.assertEqual(responses['reasoning'], {'effort': 'none'})
        self.assertNotIn('enable_thinking', responses)
        self.assertEqual(chat['ninfer_session'], responses['ninfer_session'])

    def test_responses_lineage_uses_responses_reasoning_not_chat_extension(self) -> None:
        first, continuation, branch = ({'id': f'resp_{index}'} for index in (1, 2, 3))
        replies = [
            first, continuation, branch, first, {'object': 'list'},
            {'error': {'code': 'background_not_supported'}},
            {'id': 'resp_1', 'deleted': True},
            {'error': {'code': 'response_not_found'}},
            {'error': {'code': 'response_not_found'}},
            {'id': 'resp_4'}, branch,
        ]
        with patch.object(protocol, 'json_response', side_effect=replies) as request:
            result = protocol.responses_session_lifecycle('http://fixture', 'model', {}, None, None)
        self.assertTrue(result['surviving_descendant_continued'])
        payloads = [call.args[3] for call in request.call_args_list
                    if call.args[1:3] == ('POST', '/v1/responses')]
        self.assertEqual(len(payloads), 5)
        for payload in payloads:
            self.assertEqual(payload['reasoning'], {'effort': 'none'})
            self.assertNotIn('enable_thinking', payload)
        wrong_contract = [*replies]
        wrong_contract[8] = {'error': {'code': 'previous_response_not_found'}}
        with patch.object(protocol, 'json_response', side_effect=wrong_contract):
            with self.assertRaisesRegex(protocol.ProtocolError, 'wrong continuation not-found'):
                protocol.responses_session_lifecycle('http://fixture', 'model', {}, None, None)


class BatterySelectionTests(unittest.TestCase):
    def args(self, kind: str) -> argparse.Namespace:
        return argparse.Namespace(base_url='http://fixture', model='model', contract=kind,
                                  api_key_file=None, health_timeout=1,
                                  expect_binary_sha256=None, expect_model_artifact_sha256=None,
                                  expect_config_sha256=None, expect_deployment_profile=None)

    def test_public_has_no_status_or_private_identity_assertions(self) -> None:
        with patch.object(protocol, 'wait_for_health'), \
             patch.object(protocol, 'json_response', return_value={'input_tokens': 5}) as request, \
             patch.object(protocol, 'responses_session_lifecycle', return_value={}) as responses, \
             patch.object(protocol, 'authenticated_identity_checks') as private, \
             patch.object(protocol, 'structured_output_checks', return_value={}), \
             patch.object(protocol, 'tool_round_trip', return_value='stop'), \
             patch.object(protocol, 'streaming_and_session_checks', return_value=('stop', 'stop', 3, None, None)):
            receipt = protocol.exercise(self.args('public'))
        private.assert_not_called()
        self.assertEqual(responses.call_args.args[-2:], (None, None))
        self.assertEqual([call.args[2] for call in request.call_args_list], ['/v1/messages/count_tokens'])
        self.assertEqual(receipt['schema_version'], 2)
        self.assertEqual(receipt['contract'], 'public')
        self.assertFalse(receipt['status_identity_verified'])
        self.assertFalse(receipt['authenticated_session_identity'])
        self.assertIsNone(receipt['same_session_reuse_path'])

    def test_product_status_requires_real_schema_two_identity_and_counters(self) -> None:
        status = {
            'artifact_type': 'ninfer_server_status', 'schema_version': 2, 'status': 'ok',
            'identity': dict(binary_sha256='a' * 64, model_artifact_sha256='b' * 64,
                             config_sha256='c' * 64, patch_stack_sha='d' * 40,
                             upstream_base_sha='e' * 40, deployment_profile='fixture',
                             model_id='model', build_profile='fixture', source_dirty=False),
            'runtime': {'public_model_id': 'model'},
            'scheduler': dict(max_concurrency=1, running=0, prefilling=0, decode_ready=0,
                              waiting=0, materializing=0, capture_pending=0,
                              committed_decode_tokens=0, computed_prefill_tokens=0),
            'cache': {'reused_prompt_tokens': 0, 'last_selection': {'path': 'root'}},
            'mtp': {},
        }
        protocol.validate_status_identity(status, self.args('product'))
        for group, field in (('identity', 'source_dirty'), ('identity', 'patch_stack_sha'),
                             ('scheduler', 'computed_prefill_tokens'), ('cache', 'reused_prompt_tokens')):
            original = status[group][field]
            status[group][field] = None
            with self.subTest(group=group, field=field), self.assertRaises(protocol.ProtocolError):
                protocol.validate_status_identity(status, self.args('product'))
            status[group][field] = original
        status['schema_version'] = 1
        with self.assertRaisesRegex(protocol.ProtocolError, 'envelope'):
            protocol.validate_status_identity(status, self.args('product'))

    def test_product_never_falls_back_when_status_is_missing(self) -> None:
        with patch.object(protocol, 'wait_for_health'), \
             patch.object(protocol, 'json_response', side_effect=[{'error': {'code': 'invalid_api_key'}}, contract.ContractError('status missing')]) as request, \
             patch.object(protocol, 'responses_session_lifecycle') as responses:
            with self.assertRaisesRegex(contract.ContractError, 'status missing'):
                protocol.exercise(self.args('product'))
        self.assertEqual(request.call_args.args[2], '/v1/ninfer/status')
        responses.assert_not_called()

    def test_default_cli_requires_product_api_key_before_network(self) -> None:
        with patch('sys.argv', ['agent_protocol.py', '--model', 'fixture']), \
             contextlib.redirect_stderr(io.StringIO()), \
             patch.object(protocol, 'exercise') as exercise:
            with self.assertRaises(SystemExit) as raised:
                protocol.main()
        self.assertEqual(raised.exception.code, 2)
        exercise.assert_not_called()

    def test_public_cli_rejects_unexercisable_identity_expectations(self) -> None:
        with patch('sys.argv', ['agent_protocol.py', '--model', 'fixture', '--contract', 'public', '--expect-config-sha256', 'a' * 64]), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                protocol.main()
        self.assertEqual(raised.exception.code, 2)


class SessionReplayTests(unittest.TestCase):
    @staticmethod
    def status(path: str, decode: int) -> dict:
        return {
            'identity': {'fixture_secret': 'do-not-log'},
            'scheduler': dict(max_concurrency=1, running=0, prefilling=0, decode_ready=0,
                              waiting=0, materializing=0, capture_pending=0,
                              committed_decode_tokens=decode, computed_prefill_tokens=1600),
            'cache': {
                'last_selection': {'path': path, 'frontier_tokens': 1590,
                                   'fixture_secret': 'do-not-log'},
                'reused_prompt_tokens': 1590,
                'device_state': {'occupied': 3, 'capacity': 3},
                'fixture_secret': 'do-not-log',
            },
        }

    def replay(self, repeat_path: str, isolated_path: str = 'root') -> tuple:
        usage = {
            'prompt_tokens': 1600, 'completion_tokens': 2, 'total_tokens': 1602,
            'prompt_tokens_details': {'cached_tokens': 0},
            'completion_tokens_details': {'reasoning_tokens': 0},
        }
        response = {
            'choices': [{'message': {'content': 'Yes'}, 'finish_reason': 'stop'}],
            'usage': usage,
        }
        replies = [
            response, self.status('root', 10), self.status(repeat_path, 11),
            response, self.status(isolated_path, 12),
        ]
        streamed_usage = {**usage, 'prompt_tokens_details': {'cached_tokens': 1590}}
        with patch.object(protocol, 'json_response', side_effect=replies), \
             patch.object(protocol, 'request'), \
             patch.object(protocol, 'parse_openai_stream',
                          return_value=('Yes', '', 'stop', streamed_usage)):
            return protocol.streaming_and_session_checks(
                'http://fixture', 'model', {}, {}, 'a' * 64, 'b' * 64)

    def test_public_keeps_tiny_prompt_and_product_uses_private_user_context(self) -> None:
        prompts = []
        for session_a, session_b in ((None, None), ('a' * 64, 'b' * 64)):
            with patch.object(protocol, 'json_response', side_effect=RuntimeError('fixture')) as request:
                with self.assertRaisesRegex(RuntimeError, 'fixture'):
                    protocol.streaming_and_session_checks(
                        'http://fixture', 'model', {}, {}, session_a, session_b)
            messages = request.call_args.args[3]['messages']
            self.assertEqual(len(messages), 1)
            self.assertEqual(messages[0]['role'], 'user')
            prompts.append(messages[0]['content'])
        self.assertEqual(prompts[0], 'Reply with one short word.')
        self.assertTrue(prompts[1].endswith('\n\n' + prompts[0]))
        self.assertGreater(len(prompts[1].split()), 1000)

    def test_private_repeat_and_public_foreign_path_pass(self) -> None:
        result = self.replay('private_input_replay')
        self.assertEqual(result[-2:], ('private_input_replay', 'root'))

    def test_root_repeat_still_fails_with_content_safe_cache_evidence(self) -> None:
        with self.assertRaises(protocol.ProtocolError) as raised:
            self.replay('root')
        evidence = json.loads(str(raised.exception).split(': ', 1)[1])
        self.assertEqual(evidence['actual_path'], 'root')
        self.assertEqual(evidence['after_repeat']['cache']['device_state']['capacity'], 3)
        self.assertEqual(evidence['after_repeat']['scheduler']['committed_decode_tokens'], 11)
        self.assertNotIn('do-not-log', str(raised.exception))

    def test_foreign_session_still_rejects_private_continuation(self) -> None:
        with self.assertRaisesRegex(protocol.ProtocolError, 'non-public continuation'):
            self.replay('private_input_replay', 'private_endpoint')


class HttpHelperTests(unittest.TestCase):
    def test_greedy_parity_keeps_token_accounting_but_allows_real_cache_hit_change(self) -> None:
        first = {
            'prompt_tokens': 19, 'completion_tokens': 2, 'total_tokens': 21,
            'prompt_tokens_details': {'cached_tokens': 0},
            'completion_tokens_details': {'reasoning_tokens': 0},
        }
        repeat = {**first, 'prompt_tokens_details': {'cached_tokens': 10}}
        contract.require_greedy_usage_parity(repeat, first)
        self.assertEqual(first['prompt_tokens_details']['cached_tokens'], 0)
        self.assertEqual(repeat['prompt_tokens_details']['cached_tokens'], 10)
        for changed in (
            {**repeat, 'completion_tokens': 3, 'total_tokens': 22},
            {**repeat, 'completion_tokens_details': {'reasoning_tokens': 1}},
            {**repeat, 'total_tokens': 99},
            {**repeat, 'prompt_tokens_details': {'cached_tokens': 20}},
            {**repeat, 'prompt_tokens_details': {'cached_tokens': None}},
        ):
            with self.subTest(usage=changed), self.assertRaises(contract.ContractError):
                contract.require_greedy_usage_parity(changed, first)

    def tearDown(self) -> None:
        contract.configure_api_key(None)

    def test_api_key_file_is_single_line_and_never_added_to_url(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'key'
            path.write_bytes(b'fixture-key\r\n')
            contract.configure_api_key(path)
            self.assertEqual(protocol.read_api_key(path), 'fixture-key')
            self.assertEqual(contract._DEFAULT_HEADERS, {'Authorization': 'Bearer fixture-key'})
            for invalid in (b'', b'first\nsecond', b'bad\x00key', b'\xff'):
                path.write_bytes(invalid)
                with self.assertRaises(contract.ContractError):
                    contract.configure_api_key(path)
                with self.assertRaises(protocol.ProtocolError):
                    protocol.read_api_key(path)

    def test_expected_http_error_is_parsed_and_auth_header_forwarded(self) -> None:
        headers = Message()
        headers['Content-Type'] = 'application/json'
        body = json.dumps({'error': {'code': 'invalid_api_key'}}).encode()
        error = urllib.error.HTTPError('http://fixture/v1/models', 401, 'Unauthorized', headers, io.BytesIO(body))
        with patch.object(contract.urllib.request, 'urlopen', side_effect=error) as opened:
            value = contract.json_response('http://fixture', 'GET', '/v1/models', headers={'Authorization': 'Bearer fixture'}, expected_status=401)
        self.assertEqual(value['error']['code'], 'invalid_api_key')
        sent = opened.call_args.args[0]
        self.assertEqual(sent.get_header('Authorization'), 'Bearer fixture')
        self.assertEqual(sent.full_url, 'http://fixture/v1/models')

    def test_unexpected_http_error_does_not_pass(self) -> None:
        headers = Message()
        headers['Content-Type'] = 'application/json'
        error = urllib.error.HTTPError('http://fixture', 401, 'Unauthorized', headers, io.BytesIO(b'{}'))
        with patch.object(contract.urllib.request, 'urlopen', side_effect=error):
            with self.assertRaisesRegex(contract.ContractError, 'expected 200'):
                contract.json_response('http://fixture', 'GET', '/v1/models')


if __name__ == '__main__':
    unittest.main()
