from __future__ import annotations

import json
import unittest

from tools.ninfer_serve.openai_responses import ResponsesStreamAdapter
from tools.streaming_http.sse import SseEvent


class ResponsesStreamAdapterTest(unittest.TestCase):
    def test_cancelled_response_is_terminal(self) -> None:
        payload = {
            "type": "response.cancelled",
            "response": {"id": "resp_cancelled", "status": "cancelled"},
        }
        events = ResponsesStreamAdapter().consume(
            SseEvent(
                event="response.cancelled",
                data=json.dumps(payload),
                received_ns=123,
            )
        )

        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].kind, "terminal")
        self.assertEqual(events[0].event_type, "response.cancelled")
        self.assertEqual(events[0].response_id, "resp_cancelled")


if __name__ == "__main__":
    unittest.main()
