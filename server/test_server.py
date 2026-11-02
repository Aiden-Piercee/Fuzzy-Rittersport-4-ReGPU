import json, os, struct, tempfile, threading, unittest, http.client
import fm4_live_server as s


class T(unittest.TestCase):
    def setUp(self):
        self.log = os.path.join(tempfile.mkdtemp(), "c.jsonl")
        self.srv = s.serve("127.0.0.1", 0, self.log)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.port = self.srv.server_address[1]

    def tearDown(self): self.srv.shutdown()

    def call(self, cls, m, body=b""):
        c = http.client.HTTPConnection("127.0.0.1", self.port)
        c.request("POST", "/Services.F4Release/o.xtsw", body, {
            "Content-Type": "bin/xtsw", "X-ClassName": cls, "X-Method": m,
            "X-UserId": "1", "X-TitleId": "1297287625"})
        r = c.getresponse(); return r.status, r.read()

    def test_unknown_is_empty_success_and_logged(self):
        st, b = self.call("Forza.WebServices.Auction, Forza.WebServices", "GetAuctions", b"\1\2")
        self.assertEqual(st, 200)
        self.assertEqual(struct.unpack(">III", b[:12]), (0, 0, 0))
        rec = json.loads(open(self.log).read().splitlines()[0])
        self.assertFalse(rec["handled"]); self.assertEqual(rec["body"], "0102")

    def test_handler_and_error(self):
        @s.handler("Test", "Echo")
        def echo(ctx, body): return body
        @s.handler("Test", "Boom")
        def boom(ctx, body): raise ValueError("nope")
        _, b = self.call("Forza.WebServices.Test, X", "Echo", b"abc")
        self.assertEqual(b[12:], b"abc")
        _, b = self.call("Forza.WebServices.Test, X", "Boom")
        self.assertEqual(struct.unpack(">II", b[:8]), (2, 1)); self.assertIn(b"nope", b)

if __name__ == "__main__": unittest.main()
