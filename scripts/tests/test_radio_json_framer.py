import unittest


def frame(text):
    p = 0
    n = len(text)
    def ws():
        nonlocal p
        while p < n and text[p].isspace(): p += 1
    ws()
    if p == n or text[p] != "[": return None
    p += 1; ws(); out = []
    if p < n and text[p] == "]":
        p += 1; ws(); return out if p == n else None
    while True:
        ws()
        if p == n or text[p] != "{": return None
        start = p; braces = 1; arrays = 0; string = False; escape = False; p += 1
        while p < n:
            c = text[p]; p += 1
            if string:
                if escape: escape = False
                elif c == "\\": escape = True
                elif c == '"': string = False
            elif c == '"': string = True
            elif c == '{': braces += 1
            elif c == '}':
                braces -= 1
                if braces == 0 and arrays == 0: break
            elif c == '[': arrays += 1
            elif c == ']':
                if not arrays: return None
                arrays -= 1
        if braces or arrays or string or escape: return None
        out.append(text[start:p]); ws()
        if p == n: return None
        if text[p] == ']':
            p += 1; ws(); return out if p == n else None
        if text[p] != ',': return None
        p += 1


class JsonFramerTests(unittest.TestCase):
    def test_valid_cases(self):
        cases = {"[]": [], "[{}]": ["{}"], '[{"x":"} ]"},{"n":{"a":[]}}]': ['{"x":"} ]"}', '{"n":{"a":[]}}']}
        for source, expected in cases.items(): self.assertEqual(frame(source), expected)

    def test_invalid_cases(self):
        for source in ("{}", "[{}", "[{]", "[{\"x\":\"a]", "[{},]", "[{} {}]", "[]x"):
            self.assertIsNone(frame(source))

    def test_sequential_ranges(self):
        self.assertEqual(frame('[{"a":1}, {"b":2}]'), ['{"a":1}', '{"b":2}'])


if __name__ == "__main__":
    unittest.main()
