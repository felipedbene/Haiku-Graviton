// Inputs for Bun.TOML.parse, shared by gen-toml-vectors.js (stock Bun) and
// check-toml.mjs (Node): hand-written toml-test style edge cases (valid and
// invalid), realistic config files, argument-coercion cases, nesting-depth
// cases, and a deterministic grammar-driven fuzzer with a byte mutator.

// ── hand-written edge cases ─────────────────────────────────────────────────

export const edgeCases = [
	// trivia, documents
	"", " ", "\n", "\r\n", "\r", "#", "# c", "# c\r", "# c\r\n", "#\u0000", "#\u0001", "#\u007f", "#\t tab ok", "# é ok",
	"\uFEFF", "\uFEFFa=1", "\uFEFF\uFEFFa=1", "a=1\uFEFF", "a='\uFEFF'", "\uFEFF# c\n",
	"a=1", "a = 1", "a\t=\t1", "  a = 1  ", "a = 1 # c", "a = 1#c", "a = 1\n\n\nb = 2", "a = 1\r\nb = 2\r\n", "a = 1\rb = 2", "a = 1\n\r", "a = 1 x",
	"a =", "a = ", "a =\n1", "a =\r\n1", "a = # c", "= 1", "a b = 1", "a.=1", ".a=1", "a..b=1", "a . b = 1", "a.\tb = 1", "a. b. c = 1",
	"a = 1 b = 2", "a = 1, b = 2", "a = 1;", "a = 1\na = 1",
	// keys
	"\"\" = 1", "'' = 1", "\"a\" = 1\na = 2", "'a' = 1\n\"a\" = 2", "\"a.b\" = 1\na.b = 2", "a.b = 1\n\"a.b\" = 2",
	"1 = 1", "1.2 = 3", "1.2.3 = 4", "-_- = 1", "true = 1", "false = true", "inf = nan", "nan = inf", "1979-05-27 = 1", "07:32 = 1",
	"é = 1", "\"é\" = 1", "'é' = 1", "\"\\u00e9\" = 1\n\"é\" = 2", "\"\\n\" = 1", "\"a\\u0000b\" = 1", "\"\\uD800\" = 1", "\"\\x41\" = 1",
	"__proto__ = 1", "__proto__.x = 1", "[__proto__]\nx = 1", "a = {__proto__ = 1}", "constructor = 1", "toString = 1", "hasOwnProperty = 1\nb = 2",
	"__proto__ = 1\n__proto__ = 2", "[[__proto__]]\n[[__proto__]]", "valueOf.x = 1",
	"b=1\n2=1\n1=1\na=1\n\"-1\"=1\n4294967295=1\n4294967294=1\n01=1\n1.0=1",
	"\"a\nb\" = 1", "'a\nb' = 1", "\"a\" \"b\" = 1", "a\"b\" = 1", "\"\"\"a\"\"\" = 1", "'''a''' = 1", "\"a = 1", "'a = 1",
	"a = 1\n[a]", "a.b = 1\n[a]", "a.b = 1\n[a.b]", "a.b = 1\n[a.c]", "[a]\nb.c = 1\n[a.b]", "[a]\nb.c = 1\n[a.b.d]",
	// dotted keys and blocks
	"a.b.c = 1\na.b.d = 2", "a.b = 1\na.b.c = 2", "a = {b = 1}\na.c = 2", "a.b = 1\n[x]\na.c = 1", "[a]\nb.c = 1\n[a]\n", "[a]\nb.c = 1\n[x]\n[a.b]",
	"[a]\nb.c = 1\n[a.d]\nb.e = 1", "[t]\nx.y = 1\n[t.x.z]", "[a.b]\n[a]\nb.c = 1", "[a.b]\n[a]\nc = 1", "[a.b.c]\n[a.b]\n[a]", "[a.b.c]\n[a.b]\n[a.b]",
	"a = {b.c = 1, b.d = 2}", "a = {b.c = 1, b = 2}", "a = {b = {c = 1}, b.d = 2}", "a = {b.c = 1}\na.b.d = 2", "a = {x = {y.z = 1}, x.y.w = 2}",
	"a = [{b.c = 1, b.d = 2}]", "a = {}\na.b = 1", "a = {}\n[a]", "a = {}\n[a.b]", "a = {b = {}}\n[a.b]", "a = {}\n[[a]]",
	// tables
	"[a]", "[ a ]", "[\ta\t]", "[a ] ", "[a] # c", "[a]x", "[a] x = 1", "[a]\n[a]", "[a]\n[b]\n[a]", "[a.b]\n[a.b]", "[a]\n[a.b]\n[a]",
	"[]", "[ ]", "[a.]", "[.a]", "[a..b]", "[a", "[a\n]", "[a.b", "[\"a\"]", "['a.b']", "[\"\"]", "[a . b . c]", "[a.\"b.c\".d]", "[é]",
	"[a]\nb = 1\n[a.b]", "[a]\nb = [1]\n[a.b]", "[a]\nb = 1\n[[a.b]]", "[[a]]\n[a]", "[a]\n[[a]]", "[[a]]\n[[a]]\n[a.b]\n[[a.c]]\n[a.b.x]",
	"[[a]]", "[[ a ]]", "[[a]] # c", "[[a] ]", "[ [a]]", "[[a]", "[[a]]]", "[[a.b]]\n[[a.b]]\n[a]", "[[a.b]]\n[a]\n[[a.b]]", "[[a.b]]\n[a.b]",
	"[[a]]\nb = 1\n[[a]]\nb = 2\n[a.c]\nd = 1", "[[a]]\n[[a.b]]\n[[a.b]]\n[[a]]\n[[a.b]]", "a = []\n[[a]]", "a = [{}]\n[[a]]", "a = [{}]\n[a.b]",
	"[[a]]\n[a.b]\n[a.b]", "[[a]]\n[a.b]\n[[a]]\n[a.b]", "[x]\n[x.y]\n[x.y.z]\n[x.w]\n[x]", "[a]\n[A]", "[1.2]\n[1]\n2 = 3",
	"[[fruits]]\nname = \"apple\"\n[fruits.physical]\ncolor = \"red\"\n[[fruits.varieties]]\nname = \"red delicious\"\n[[fruits]]\nname = \"banana\"",
	"[a]\n[a.b]\nc = 1\n[a]\nb.d = 1",
	// integers
	"a = 0", "a = +0", "a = -0", "a = 00", "a = 01", "a = -01", "a = +01", "a = 0_0", "a = 1_000", "a = 1__000", "a = _1", "a = 1_", "a = -_1", "a = 1_e3",
	"a = 9007199254740991", "a = -9007199254740991", "a = 9007199254740992", "a = -9007199254740992", "a = 9223372036854775807", "a = 9223372036854775808",
	"a = -9223372036854775808", "a = -9223372036854775809", "a = 18446744073709551615", "a = 18446744073709551616", "a = 99999999999999999999999999",
	"a = 0x0", "a = 0xDEADBEEF", "a = 0xdead_beef", "a = 0x_1", "a = 0x1_", "a = 0x", "a = 0xG", "a = 0x1G", "a = -0x1", "a = +0x1", "a = 0X1", "a = 0x1F_FFFF_FFFF_FFFF",
	"a = 0x20000000000000", "a = 0x7FFFFFFFFFFFFFFF", "a = 0x8000000000000000", "a = 0xFFFFFFFFFFFFFFFFFF", "a = 0o0", "a = 0o777", "a = 0o8", "a = 0o7_7", "a = 0o",
	"a = 0b0", "a = 0b1010", "a = 0b2", "a = 0b1_0_1", "a = 0b", "a = 0b11a", "a = 0x1.0", "a = 0b1e1", "a = 0o1 ", "a = 0x00001", "a = 1x", "a = 12a", "a = 1-2",
	"a = 1,", "a = 1]", "a = 1}", "a = 1#", "a = 1\t", "a = 1.", "a = 1..2", "a = .1", "a = +.1", "a = -.1",
	// floats
	"a = 1.0", "a = -1.0", "a = +1.0", "a = 0.0", "a = -0.0", "a = +0.0", "a = 3.14_15", "a = 3._14", "a = 3.14_", "a = 1e3", "a = 1E3", "a = 1e+3", "a = 1e-3",
	"a = 1e", "a = 1e+", "a = 1e_3", "a = 1e3_0", "a = 1e3.5", "a = 1.5e3", "a = 1.5e", "a = 1.e3", "a = 01.5", "a = 0.5", "a = 00.5", "a = 0e0", "a = -0e0", "a = 1e400",
	"a = -1e400", "a = 1e-400", "a = -1e-400", "a = 4.9e-324", "a = 2.4703282292062327e-324", "a = 1.7976931348623157e308", "a = 1.7976931348623159e308",
	"a = 9007199254740993.0", "a = 0.1", "a = 0.30000000000000004", "a = 123456789012345678901234567890.5", "a = 1_2.3_4e5_6", "a = 6.626e-34",
	"a = inf", "a = +inf", "a = -inf", "a = nan", "a = +nan", "a = -nan", "a = infinity", "a = nanx", "a = inf_", "a = -infinity", "a = in", "a = na", "a = INF", "a = NaN",
	"a = inf.0", "a = nan#c", "a = [inf,-inf,nan]", "a = {x=inf}", "a = - 1", "a = +", "a = -", "a = ++1", "a = +-1", "a = 1e1e1",
	// booleans and bare words
	"a = true", "a = false", "a = tru", "a = truex", "a = True", "a = TRUE", "a = t", "a = f", "a = falsey", "a = true_", "a = true-", "a = true1", "a = hello",
	"a = hello world", "a = isolated", "a = x" + "y".repeat(80), "a = é", "a = @", "a = $", "a = !", "a = \u0000", "a = \u0001", "a = \u007f", "a = =", "a = ]", "a = }",
	// basic strings
	"a = \"\"", "a = \"x\"", "a = \"x", "a = \"x\ny\"", "a = \"x\r\ny\"", "a = \"x\ry\"", "a = \"\\\"\"", "a = \"\\\\\"", "a = \"\\b\\t\\n\\f\\r\\e\"",
	"a = \"\\a\"", "a = \"\\ \"", "a = \"\\'\"", "a = \"\\/\"", "a = \"\\0\"", "a = \"\\", "a = \"\\\u0000", "a = \"\\\u0000\"", "a = \"\\\u00e9\"",
	"a = \"\\x41\"", "a = \"\\x4\"", "a = \"\\xG1\"", "a = \"\\xff\"", "a = \"\\x00\"", "a = \"\\x7f\"", "a = \"\\u0041\"", "a = \"\\u004\"", "a = \"\\u00E9\"",
	"a = \"\\uD800\"", "a = \"\\uDFFF\"", "a = \"\\uD83D\\uDE00\"", "a = \"\\uFFFF\"", "a = \"\\U0001F600\"", "a = \"\\U00110000\"", "a = \"\\UFFFFFFFF\"", "a = \"\\U0000D800\"",
	"a = \"\\U0010FFFF\"", "a = \"\\U001F600\"", "a = \"\\u\"", "a = \"\\U\"", "a = \"\\x\"", "a = \"tab\there\"", "a = \"nul\u0000\"", "a = \"del\u007f\"", "a = \"esc\u001b\"",
	"a = \"é😀\"", "a = \"\uD800\"", "a = \"\uDC00x\"", "a = \"a\" \"b\"", "a = \"a\"b", "a = \"a\"#c", "a = \"a\",",
	// multi-line basic strings
	"a = \"\"\"\"\"\"", "a = \"\"\"x\"\"\"", "a = \"\"\"\nx\"\"\"", "a = \"\"\"\r\nx\"\"\"", "a = \"\"\"\n\nx\"\"\"", "a = \"\"\"x\ny\"\"\"", "a = \"\"\"x\r\ny\"\"\"",
	"a = \"\"\"x\ry\"\"\"", "a = \"\"\"x", "a = \"\"\"x\"\"", "a = \"\"\"x\"\"\"\"", "a = \"\"\"x\"\"\"\"\"", "a = \"\"\"x\"\"\"\"\"\"", "a = \"\"\"x\"\"\"\"\"\"\"",
	"a = \"\"\"\"x\"\"\"", "a = \"\"\"\"\"x\"\"\"", "a = \"\"\"\"\"\"x\"\"\"", "a = \"\"\"a\"b\"\"c\"\"\"", "a = \"\"\"a\"\"\"\"b\"\"\"", "a = \"\"\"\\\n   x\"\"\"",
	"a = \"\"\"a \\\n\n   b\"\"\"", "a = \"\"\"a \\  \n  b\"\"\"", "a = \"\"\"a \\\r\n  b\"\"\"", "a = \"\"\"a \\ x\"\"\"", "a = \"\"\"a\\\"\"\"", "a = \"\"\"a\\\\\n\"\"\"",
	"a = \"\"\"\\\"\"\"\"", "a = \"\"\"a\\\n\"\"\"", "a = \"\"\"a\\\n\r\n\t b\"\"\"", "a = \"\"\"a\\\r\"\"\"", "a = \"\"\"\\u00e9\\n\"\"\"", "a = \"\"\"\u0001\"\"\"",
	"a = \"\"\"\t\"\"\"", "a = \"\"\"é\"\"\"", "a = \"\"\"x\"\"\" y", "a = \"\"\"x\"\"\"# c", "a = \"\"\"\n\"\"\"", "a = \"\"\"\r\"\"\"", "a = \"\"\"\\\n\"\"\"",
	"a = \"\"\"a\nb\\\n  c\nd\"\"\"\nb = 1", "a = \"\"\"\"\"\"\"\"\"",
	// literal strings
	"a = ''", "a = 'x'", "a = 'x", "a = 'x\ny'", "a = 'x\r\ny'", "a = 'x\ry'", "a = 'C:\\path\\'", "a = '\\u0041'", "a = 'tab\there'", "a = 'nul\u0000'",
	"a = 'del\u007f'", "a = 'é'", "a = 'a''b'", "a = 'a'b", "a = '''x'''", "a = '''\nx'''", "a = '''\r\nx'''", "a = '''x\r\ny'''", "a = '''x\ry'''", "a = '''x",
	"a = '''x''''", "a = '''x'''''", "a = '''x''''''", "a = ''''x'''", "a = '''''x'''", "a = '''a'b''c'''", "a = '''\u0001'''", "a = '''\\\n'''",
	"a = ''''''", "a = '''''''", "a = '''é\n\t'''", "a = '''x''' y",
	// date/times
	"a = 1979-05-27T07:32:00Z", "a = 1979-05-27T07:32:00z", "a = 1979-05-27t07:32:00Z", "a = 1979-05-27 07:32:00Z", "a = 1979-05-27T07:32:00",
	"a = 1979-05-27T07:32:00.999999Z", "a = 1979-05-27T07:32:00.5", "a = 1979-05-27T07:32:00.123456789123", "a = 1979-05-27T07:32:00.100+01:00",
	"a = 1979-05-27T07:32:00.000Z", "a = 1979-05-27T07:32:00.Z", "a = 1979-05-27T07:32:00.", "a = 1979-05-27T07:32Z", "a = 1979-05-27T07:32", "a = 1979-05-27T07:32:00-07:00",
	"a = 1979-05-27T07:32:00+00:00", "a = 1979-05-27T07:32:00-00:00", "a = 1979-05-27T07:32:00+23:59", "a = 1979-05-27T07:32:00+24:00", "a = 1979-05-27T07:32:00+01:60",
	"a = 1979-05-27T07:32:00+1:00", "a = 1979-05-27T07:32:00+0100", "a = 1979-05-27T07:32:00+01", "a = 1979-05-27T07:32:00+", "a = 1979-05-27T07:32:00 Z",
	"a = 1979-05-27", "a = 1979-05-27 ", "a = 1979-05-27 # c", "a = 1979-05-27 x", "a = 1979-05-27 07:32", "a = 1979-05-27 7:32", "a = 1979-05-27T", "a = 1979-05-27t",
	"a = 1979-05-27T7:32:00", "a = 1979-05-27T07:3", "a = 1979-05-27T07:32:0", "a = 1979-05-27T07-32", "a = 1979-05-27T24:00:00", "a = 1979-05-27T23:60:00",
	"a = 1979-05-27T23:59:60", "a = 1979-05-27T23:59:61", "a = 1979-05-27T23:59:60Z", "a = 1979-05-27T23:59:60.999Z", "a = 1979-05-27T07:32:00Zx", "a = 1979-05-27x",
	"a = 1979-5-27", "a = 1979-05-7", "a = 1979-05", "a = 1979-", "a = 197-05-27", "a = 19790-05-27", "a = 1979-13-01", "a = 1979-00-01", "a = 1979-01-00",
	"a = 1979-01-32", "a = 1979-04-31", "a = 1979-04-30", "a = 1979-02-29", "a = 1980-02-29", "a = 1900-02-29", "a = 2000-02-29", "a = 2100-02-29", "a = 2400-02-29",
	"a = 1979-02-30", "a = 0000-01-01", "a = 0000-02-29", "a = 9999-12-31", "a = 0000-01-01T00:00:00Z", "a = 0000-01-01T00:00:00+00:01", "a = 0000-01-01T00:00:00-23:59",
	"a = 9999-12-31T23:59:59-23:59", "a = 9999-12-31T23:59:60-01:00", "a = 0001-01-01T00:00:00+01:00", "a = 1970-01-01T00:00:00Z", "a = 1969-12-31T23:59:59.999999999Z",
	"a = 07:32:00", "a = 07:32", "a = 07:32:00.5", "a = 07:32:00.000", "a = 07:32:00.0010", "a = 00:00:00", "a = 23:59:59.999999999999", "a = 24:00:00", "a = 23:60",
	"a = 23:59:60", "a = 23:59:61", "a = 7:32:00", "a = 07:2", "a = 07:", "a = 07:32:", "a = 07:32:0", "a = 07:32:00.", "a = 07:32:00Z", "a = 07:32:00+01:00", "a = 07:32x",
	"a = 07:32:00 # c", "a = 07:32:00,", "a = [07:32, 1979-05-27, 1979-05-27T07:32:00Z, 1979-05-27 07:32:00]", "a = {d = 1979-05-27}", "1979-05-27T07:32:00Z = 1",
	"a = 1979-05-27T07:32:00.12345678901234567890123+05:45", "a = 2024-02-29T12:00:00+14:00", "a = 2024-03-01T00:00:00-12:00", "a = 2024-12-31T23:30:00-01:00",
	"a = 1979-05-27T07:32:00Z\nb = 1979-05-27T07:32:00Z", "a = 1979-05-27\n[a]", "a = 1979-05-27\na.b = 1", "a = 1979-05-27\n[[a]]",
	// arrays
	"a = []", "a = [ ]", "a = [,]", "a = [1,]", "a = [1,,]", "a = [,1]", "a = [1 2]", "a = [1", "a = [1,", "a = [", "a = [\n", "a = [\n1,\n2,\n]", "a = [ # c\n1 # c\n, # c\n2 # c\n]",
	"a = [1, \"a\", 1.5, true, [], {}, 1979-05-27]", "a = [[1, 2], [3, [4, [5]]]]", "a = [\r\n1\r\n]", "a = [1\r]", "a = [1]x", "a = [1] # c", "a = [1]]", "a = [{a=1},{a=2}]",
	"a = [1,\n#c\u0001\n]", "a = [\"a\", 'b', \"\"\"c\"\"\", '''d''']", "a = [-0, -0.0, +0.0]", "a = [\n\n]", "a = [\t]", "a = [ , ]", "a = [1 , 2 ,3]",
	// inline tables
	"a = {}", "a = { }", "a = {,}", "a = {b=1,}", "a = {b=1,,}", "a = {,b=1}", "a = {b=1 c=2}", "a = {b=1", "a = {b=1,", "a = {", "a = {b}", "a = {b=}", "a = {=1}",
	"a = {b=1, b=2}", "a = {b=1}\nb = 2", "a = {\nb = 1,\nc = 2\n}", "a = { # c\n b = 1 # c\n , c = 2 # c\n}", "a = {b = {c = {d = 1}}}", "a = {b = [1, {c = 2}]}",
	"a = {b=1}x", "a = {b=1}}", "a = {\"x y\" = 1, 'z' = 2}", "a = {b.c = 1}", "a = { b . c = 1 }", "a = {b=1}\n[a]\nc=1", "a = {b=1}\n[a.c]", "a = {b={}}\na.b.c=1",
	"a = {b = 1\n}", "a = {\n}", "a = {b = 1}, c = 2", "a = {b = \"}\"}", "a = {b = 1\r\n}", "a = {b = 1\r}", "a = {__proto__ = {x = 1}, y = 2}",
	// line endings and junk between expressions
	"a = 1 # c\r\nb = 2 # c\r\n", "[a] # c\r\nb = 1", "[a]\r\n[b]\r\n", "a = 1\n\t\n  \nb = 2", "a = 1 \u000b", "a = 1 \u000c", "a = 1\u0000", "\u0000",
	"a = 1\n\u0001", "\"a\" = \"b\" # é", "a = '''\r\n'''", "a = \"\"\"\r\n\"\"\"",
	// error ordering: which error is reported first
	"a = 1x\nb = \"", "a = \"\\q\nb = 1", "[a]\n[a]\nx = \"\\q\"", "a = 9223372036854775808_", "a = 9223372036854775808x", "a = 01_", "a = 00x", "a = 0x_", "a = 1979-13-40",
	"a = 1979-02-30T25:61:61", "a = 1979-05-27T23:59:61+25:00", "a = 1979-05-27T07:32:00+25:61", "a = 23:61:00", "a = 25:00", "a = \"\\uD800\\q\"", "a = 1\na = \"\\q\"",
	"[a]\na = 1\n[a]", "a = [1, 2\nb = 1", "a.b = 1\na = 2", "a = 1\na.b = 2", "a = [1]\na.b = 2", "a = [1]\n[a.b]", "a = [1]\n[[a]]", "a = \"x\"\n[a.b]", "a = 1\n[[a]]",
	"[a]\n[a.b]\n[a.b.c]\na.b.c = 1",
];

// Byte inputs (passed as Buffers): invalid UTF-8 and raw bytes the
// string path cannot express.
export const byteCases = [
	[0xff], [0xfe, 0xff], [0xef, 0xbb], [0xef, 0xbb, 0xbf], [0xef, 0xbb, 0xbf, 0x61, 0x3d, 0x31], [0x61, 0x3d, 0x22, 0xff, 0x22], [0x61, 0x3d, 0x22, 0xc3, 0x22],
	[0x61, 0x3d, 0x22, 0xed, 0xa0, 0x80, 0x22], [0x61, 0x3d, 0x22, 0xc0, 0xaf, 0x22], [0x61, 0x3d, 0x22, 0xf4, 0x90, 0x80, 0x80, 0x22], [0x23, 0x80],
	[0x61, 0x3d, 0x31, 0x20, 0x23, 0xe2, 0x82], [0x61, 0x3d, 0x31, 0x0a, 0xc3, 0xa9, 0x3d, 0x31], [0x00], [0x61, 0x3d, 0x00], [0x61, 0x00, 0x3d, 0x31],
	[0x61, 0x3d, 0x22, 0x5c, 0x00], [0x61, 0x3d, 0x22, 0x5c, 0x00, 0x22], [0x61, 0x3d, 0x22, 0x5c],
];

// ── realistic config files ──────────────────────────────────────────────────

export const configFiles = [
	`# Codex CLI config
model = "gpt-5-codex"
model_provider = "openai"
approval_policy = "on-request"
sandbox_mode = "workspace-write"
model_reasoning_effort = "high"
model_reasoning_summary = "detailed"
hide_agent_reasoning = false
disable_response_storage = true
notify = ["notify-send", "Codex"]
project_doc_max_bytes = 32768

[sandbox_workspace_write]
network_access = false
writable_roots = ["/tmp", "~/.cache"]
exclude_tmpdir_env_var = false

[shell_environment_policy]
inherit = "core"
ignore_default_excludes = false
exclude = ["AWS_*", "AZURE_*"]
set = { CI = "1", TERM = "xterm-256color" }
include_only = ["PATH", "HOME"]

[mcp_servers.docs]
command = "npx"
args = ["-y", "@upstash/context7-mcp"]
env = { "API_KEY" = "value", LOG_LEVEL = "debug" }
startup_timeout_sec = 20
tool_timeout_sec = 60.5

[mcp_servers.github]
url = "https://api.githubcopilot.com/mcp/"
bearer_token_env_var = "GITHUB_TOKEN"
http_headers = { "X-Custom" = "yes" }

[mcp_servers."local.server"]
command = '/usr/local/bin/server'
args = []

[model_providers.azure]
name = "Azure"
base_url = "https://YOUR_PROJECT_NAME.openai.azure.com/openai"
env_key = "AZURE_OPENAI_API_KEY"
query_params = { api-version = "2025-04-01-preview" }
wire_api = "responses"
request_max_retries = 4
stream_max_retries = 10
stream_idle_timeout_ms = 300_000

[profiles.o3]
model = "o3"
model_provider = "openai"
approval_policy = "never"
model_reasoning_effort = "high"

[profiles.local]
model = "llama3.1"
model_provider = "ollama"

[projects."/Users/me/code/project"]
trust_level = "trusted"

[projects.'C:\\Users\\me\\repo']
trust_level = "untrusted"

[tui]
notifications = ["agent-turn-complete", "approval-requested"]

[history]
persistence = "save-all"
max_bytes = 10485760

[tools]
web_search = true

[features]
streamable_shell = true
`,
	`# Codex prompt command (~/.codex/prompts style, TOML flavour)
description = "Review the staged diff"
argument-hint = "[focus]"
prompt = """
Review the staged changes with \`git diff --cached\`.

Focus on: $ARGUMENTS
- correctness
- "quoted" text and \\ backslashes
"""
`,
	`description = 'Write a commit message'
prompt = '''
Summarise !\`git diff --cached --stat\` in an imperative subject line.
No escapes are processed here: \\n stays literal.
'''
model = "sonnet"
`,
	"model = \"o4-mini\"\r\napproval_policy = \"untrusted\"\r\n\r\n[mcp_servers.fs]\r\ncommand = \"node\"\r\nargs = [\r\n  \"server.js\",\r\n  \"--root\", \"/srv\",\r\n]\r\n",
	`[package]
name = "demo"
version = "0.1.0"
edition = "2021"
authors = ["A <a@example.com>"]

[dependencies]
serde = { version = "1.0", features = ["derive"] }
tokio = { version = "1", features = ["full"], optional = true }

[dev-dependencies]
criterion = "0.5"

[[bin]]
name = "demo"
path = "src/main.rs"

[[bin]]
name = "tool"
path = "src/tool.rs"

[profile.release]
lto = true
opt-level = 3
codegen-units = 1

[target.'cfg(unix)'.dependencies]
libc = "0.2"
`,
	`[build-system]
requires = ["setuptools>=61", "wheel"]
build-backend = "setuptools.build_meta"

[project]
name = "pkg"
version = "1.2.3"
dependencies = [
  "requests>=2.31", # http
  "rich",
]
optional-dependencies.dev = ["pytest", "ruff"]
classifiers = [
    "Programming Language :: Python :: 3",
]

[tool.ruff]
line-length = 100
lint.select = ["E", "F"]
lint.ignore = []

[tool.pytest.ini_options]
addopts = "-q"
testpaths = ["tests"]
`,
	`# TOML spec example
title = "TOML Example"

[owner]
name = "Tom Preston-Werner"
dob = 1979-05-27T07:32:00-08:00

[database]
enabled = true
ports = [ 8000, 8001, 8002 ]
data = [ ["delta", "phi"], [3.14] ]
temp_targets = { cpu = 79.5, case = 72.0 }

[servers]

[servers.alpha]
ip = "10.0.0.1"
role = "frontend"

[servers.beta]
ip = "10.0.0.2"
role = "backend"
`,
	// common mistakes in hand-edited configs
	"model = gpt-5\n", "[mcp_servers.x]\ncommand = \"npx\"\nargs = [\"a\" \"b\"]\n", "[mcp_servers.x]\nenv = { A = \"1\", }\n", "[mcp_servers.x]\nenv = {\n  A = \"1\"\n}\n",
	"[mcp_servers.x]\ncommand = \"a\"\n[mcp_servers.x]\nargs = []\n", "approval_policy = \"never\"\napproval_policy = \"never\"\n", "[mcp_servers]\nx = { command = \"a\" }\n[mcp_servers.x]\n",
	"[profiles.a]\nmodel = \"o3\"\n[profiles]\na.model_provider = \"openai\"\n", "notify = [\"a\",\n\"b\"\n", "windows_path = \"C:\\Users\\me\"\n",
];

// ── argument coercion (functions, so both runtimes build the same value) ────

export const coerceCases = [
	["no-args", null],
	["undefined", () => undefined], ["null", () => null], ["number", () => 123], ["float", () => 1.5], ["true", () => true], ["bigint", () => 1n],
	["symbol", () => Symbol("x")], ["object-toString", () => ({ toString() { return "a = 1"; } })], ["object-valueOf", () => ({ valueOf() { return "a = 2"; } })],
	["object-plain", () => ({ a: 1 })], ["array", () => ["a = 1"]], ["array-2", () => ["a = 1", "b = 2"]], ["string-object", () => new String("a = 1")],
	["toString-throws", () => ({ toString() { throw new Error("boom"); } })], ["function", () => function f() {}],
	["buffer", () => Buffer.from("a = 1")], ["uint8array", () => new Uint8Array([0x61, 0x3d, 0x31])], ["arraybuffer", () => new Uint8Array([0x61, 0x3d, 0x31]).buffer],
	["dataview", () => new DataView(new Uint8Array([0x61, 0x3d, 0x31]).buffer)], ["uint16array", () => new Uint16Array([0x3d61, 0x0a31])],
	["subarray", () => new Uint8Array([0x78, 0x61, 0x3d, 0x31, 0x78]).subarray(1, 4)], ["dataview-offset", () => new DataView(new Uint8Array([0x78, 0x61, 0x3d, 0x32, 0x78]).buffer, 1, 3)],
	["sharedarraybuffer", () => { const s = new SharedArrayBuffer(3); new Uint8Array(s).set([0x61, 0x3d, 0x31]); return s; }],
	["shared-view", () => { const s = new SharedArrayBuffer(3); new Uint8Array(s).set([0x61, 0x3d, 0x31]); return new Uint8Array(s); }],
	["empty-arraybuffer", () => new ArrayBuffer(0)], ["float64array", () => new Float64Array([0])], ["string-lone-surrogate", () => "a = \"\uD800\""],
	["string-lone-surrogate-key", () => "\"\uDFFF\" = 1"], ["string-surrogate-pair", () => "a = \"\uD83D\uDE00\""], ["blob", () => new Blob(["a = 1"])],
	["over-2gib", () => Buffer.allocUnsafe(2 ** 31)],
];

// ── nesting depth ───────────────────────────────────────────────────────────

export const depthCases = [];
for (const n of [100, 1000, 3000, 5000])
	depthCases.push(
		["array-" + n, "a = " + "[".repeat(n) + "]".repeat(n)],
		["inline-" + n, "a = " + "{b=".repeat(n) + "1" + "}".repeat(n)],
		["header-" + n, "[" + Array(n).fill("a").join(".") + "]"],
		["dotted-" + n, Array(n).fill("a").join(".") + " = 1"],
		["aot-" + n, "[[" + Array(n).fill("a").join(".") + "]]"],
	);
for (const n of [10000, 20000, 30000, 50000, 100000])
	depthCases.push(
		["array-" + n, "a = " + "[".repeat(n) + "]".repeat(n)],
		["inline-" + n, "a = " + "{b=".repeat(n) + "1" + "}".repeat(n)],
		["header-" + n, "[" + Array(n).fill("a").join(".") + "]"],
		["dotted-" + n, Array(n).fill("a").join(".") + " = 1"],
	);

// ── grammar-driven fuzzer ───────────────────────────────────────────────────

export function makeFuzzer(seed) {
	let st = BigInt(seed);
	const M = (1n << 64n) - 1n;
	const rnd = (n) => { st ^= st << 13n; st &= M; st ^= st >> 7n; st ^= st << 17n; st &= M; return Number(st % BigInt(n)); };
	const pick = (a) => a[rnd(a.length)];
	const chance = (k) => rnd(k) === 0;

	const bareKeys = ["a", "b", "c", "x", "key", "1", "0", "-", "_", "a-b", "a_b", "true", "inf", "nan", "__proto__", "constructor", "toString", "4294967295", "01", "Z9"];
	const ws = () => pick(["", "", "", " ", "\t", "  ", " \t"]);
	const nl = () => pick(["\n", "\n", "\n", "\r\n", "\n\n", " # c\n", "\r\n\r\n", "#\u00e9\n"]);
	const esc = () => pick(["\\n", "\\t", "\\\\", "\\\"", "\\b", "\\f", "\\r", "\\e", "\\x41", "\\xe9", "\\u00e9", "\\uD800", "\\U0001F600", "\\U00110000", "\\q", "\\u12", "\\x4"]);
	const chars = () => pick(["a", "b", " ", "é", "😀", "'", "\"", "#", "=", "\t", "[", "{", ".", "\u0001", "\u007f", "\\"]);
	const strBody = (quote) => { let s = ""; const n = rnd(6); for (let i = 0; i < n; i++) { const c = chars(); if (c === quote && rnd(3)) continue; if (c === "\\" && quote === "\"") s += esc(); else s += c; } return s; };
	const basic = () => "\"" + strBody("\"") + "\"";
	const literal = () => "'" + strBody("'") + "'";
	const mlBasic = () => "\"\"\"" + pick(["", "\n", "\r\n"]) + strBody("") + pick(["", "\n", "\\\n  ", "\"", "\"\"", "\r\n"]) + strBody("") + pick(["\"\"\"", "\"\"\"\"", "\"\"\"\"\"", "\"\"\"\"\"\"", "\"\""]);
	const mlLiteral = () => "'''" + pick(["", "\n", "\r\n"]) + strBody("").replaceAll("\\", "/") + pick(["", "\n", "'", "''", "\r\n"]) + "'''" + pick(["", "", "'", "''", "'''"]);
	const key = () => (rnd(10) < 7 ? pick(bareKeys) : rnd(2) ? (chance(3) ? "\"" + strBody("\"") + "\"" : "\"" + pick(bareKeys) + "\"") : "'" + pick(bareKeys) + "'");
	const dkey = () => { let k = key(); const n = rnd(3) ? 0 : rnd(3) + 1; for (let i = 0; i < n; i++) k += ws() + "." + ws() + key(); return k; };
	const digits = (n) => { let s = String(1 + rnd(9)); for (let i = 1; i < n; i++) s += String(rnd(10)); return s; };
	const us = (s) => (chance(4) ? s.replace(/(\d)(\d)/, "$1_$2") : s);
	const int = () => pick(["", "", "+", "-"]) + pick([() => "0", () => us(digits(1 + rnd(4))), () => us(digits(15 + rnd(5))), () => pick(["9007199254740991", "9007199254740992", "9223372036854775807", "9223372036854775808", "18446744073709551616", "00", "01", "1__2", "_1", "1_"])])();
	const radix = () => pick(["", "", "", "+", "-"]) + pick(["0x", "0o", "0b"]) + pick(["", "0", "1", "7", "8", "f", "F", "g", "1_0", "_1", "1_", "DEAD_BEEF", "7FFFFFFFFFFFFFFF", "8000000000000000", "1FFFFFFFFFFFFF", "20000000000000"]);
	const float = () => pick(["", "", "+", "-"]) + pick([() => us(digits(1 + rnd(3))) + "." + us(digits(1 + rnd(4))), () => digits(1 + rnd(2)) + pick(["e", "E"]) + pick(["", "+", "-"]) + digits(1 + rnd(3)), () => "0." + digits(1 + rnd(18)) + pick(["", "e-5", "e400", "E+3"]), () => pick(["inf", "nan", "1.", ".5", "1e", "1.e1", "1e_1", "1.5_", "infinity", "0.0", "1e1e1", "01.5"])])();
	const two = (n) => String(n).padStart(2, "0");
	const date = () => String(rnd(10) ? 1900 + rnd(200) : pick([0, 1, 9999, 400, 100])).padStart(4, "0") + "-" + two(rnd(10) ? 1 + rnd(12) : pick([0, 13, 2])) + "-" + two(rnd(8) ? 1 + rnd(28) : pick([0, 29, 30, 31, 32]));
	const time = () => two(rnd(10) ? rnd(24) : pick([24, 25, 7])) + ":" + two(rnd(10) ? rnd(60) : 60) + (rnd(4) ? ":" + two(rnd(10) ? rnd(60) : pick([60, 61])) + (chance(3) ? "." + digits(1 + rnd(14)) + (chance(3) ? "000" : "") : "") : "");
	const offset = () => pick(["", "", "Z", "z", "+00:00", "-00:00", "+05:30", "-08:00", "+23:59", "+24:00", "-01:60", "+1:00", "+0100"]);
	const datetime = () => pick([() => date(), () => time(), () => date() + pick(["T", "t", " "]) + time() + offset(), () => date() + "T" + time()])();
	const scalar = () => pick([int, int, radix, float, basic, basic, literal, mlBasic, mlLiteral, datetime, datetime, () => pick(["true", "false", "tru", "truex", "hello", "nanx", "TRUE"])])();
	const value = (d) => {
		const r = rnd(10);
		if (d < 4 && r === 0) { let s = "[" + ws(); const n = rnd(4); for (let i = 0; i < n; i++) s += (i ? ws() + "," + pick([ws(), nl()]) : "") + value(d + 1); return s + ws() + pick(["]", ",]", ", ]", "\n]", "", ",,]", " # c\n]"]); }
		if (d < 4 && r === 1) { let s = "{" + ws(); const n = rnd(4); for (let i = 0; i < n; i++) s += (i ? ws() + "," + pick([ws(), ws(), "\n"]) : "") + dkey() + ws() + "=" + ws() + value(d + 1); return s + ws() + pick(["}", ",}", " }", "\n}", "", ",,}"]); }
		return scalar();
	};
	const header = () => { const aot = chance(3); let p = ws() + key(); const n = rnd(3); for (let i = 0; i < n; i++) p += ws() + "." + ws() + key(); return (aot ? "[[" : "[") + p + ws() + (aot ? pick(["]]", "]]", "]]", "] ]", "]"]) : pick(["]", "]", "]", "]]"])); };

	const document = (maxLines = 8) => {
		let s = chance(20) ? "\uFEFF" : "";
		const n = 1 + rnd(maxLines);
		for (let i = 0; i < n; i++) {
			const r = rnd(10);
			if (r < 3) s += header();
			else if (r < 4) s += pick(["# comment", "", "   ", "#\u0001", "\t# x"]);
			else s += ws() + dkey() + ws() + "=" + ws() + value(0);
			s += pick(["", "", " # trailing", " ", "\t"]) + (i + 1 < n ? nl() : pick(["", "\n", "\r\n", "\r"]));
		}
		return s;
	};

	// Byte-level mutation of a string document (keeps it a JS string).
	const junk = ["", "\n", "\r", "\r\n", " ", "\"", "'", "[", "]", "{", "}", ",", ".", "=", "#", "\\", "_", "0", "1", "e", "x", ":", "-", "+", "T", "Z", "é", "\u0000", "\u0001", "\uFEFF", "\"\"\"", "'''"];
	const mutate = (s) => {
		const n = 1 + rnd(3);
		for (let k = 0; k < n; k++) {
			const i = rnd(s.length + 1);
			const r = rnd(4);
			if (r === 0) s = s.slice(0, i) + s.slice(i + 1);
			else if (r === 1) s = s.slice(0, i) + pick(junk) + s.slice(i);
			else if (r === 2) s = s.slice(0, i) + pick(junk) + s.slice(i + 1);
			else { const j = rnd(s.length + 1); s = s.slice(0, Math.min(i, j)) + s.slice(Math.max(i, j)); }
		}
		return s;
	};
	return { document, mutate, rnd };
}

// ── value encoding (same code under both runtimes) ──────────────────────────
//
// Lossless and order-preserving: numbers by their IEEE bits (-0, NaN, inf),
// objects by own property names in order with a flag for anything that is
// not a plain, default-descriptor data property, and date/time values by
// type name + String(v). `dtName(v)` returns the date/time type name or null.

const f64 = new DataView(new ArrayBuffer(8));
export function encode(v, dtName) {
	if (typeof v === "number") { f64.setFloat64(0, v); return { n: f64.getBigUint64(0).toString(16) }; }
	if (typeof v === "string") return { s: v };
	if (typeof v === "boolean") return { b: v };
	if (v === null || typeof v !== "object") return { x: typeof v };
	const dt = dtName(v);
	if (dt) return { T: dt, s: String(v) };
	const odd = (o, proto) => {
		if (Object.getPrototypeOf(o) !== proto || !Object.isExtensible(o) || Object.getOwnPropertySymbols(o).length) return true;
		for (const k of Object.getOwnPropertyNames(o)) {
			if (Array.isArray(o) && k === "length") continue;
			const d = Object.getOwnPropertyDescriptor(o, k);
			if (!("value" in d) || !d.writable || !d.enumerable || !d.configurable) return true;
		}
		return false;
	};
	if (Array.isArray(v)) {
		const r = { a: v.map((x) => encode(x, dtName)) };
		if (odd(v, Array.prototype) || Object.getOwnPropertyNames(v).length !== v.length + 1) r.odd = true;
		return r;
	}
	const r = { o: Object.getOwnPropertyNames(v).map((k) => [k, encode(v[k], dtName)]) };
	if (odd(v, Object.prototype)) r.odd = true;
	return r;
}

// The same value as a flat string, built without recursion: for the depth
// cases, whose nesting overflows a recursive walk (or JSON.stringify) on V8.
export function encodeFlat(root, dtName) {
	const out = [];
	const stack = [root];
	while (stack.length) {
		const v = stack.pop();
		if (typeof v === "string" && v.startsWith("\u0000")) { out.push(v.slice(1)); continue; }	// markup
		if (v === null || typeof v !== "object" || dtName(v)) { out.push(JSON.stringify(encode(v, dtName))); continue; }
		const isArr = Array.isArray(v);
		const keys = isArr ? null : Object.getOwnPropertyNames(v);
		const parts = [isArr ? "\u0000[" : "\u0000{"];
		if (isArr) for (const x of v) parts.push(x, "\u0000,");
		else for (const k of keys) parts.push("\u0000" + JSON.stringify(k) + ":", v[k], "\u0000,");
		parts.push(isArr ? "\u0000]" : "\u0000}");
		for (let i = parts.length - 1; i >= 0; i--) stack.push(parts[i]);
	}
	return out.join("");
}

export function encodeError(e) {
	return { e: e && e.constructor ? e.constructor.name : typeof e, m: e && e.message, c: e && e.code !== undefined ? e.code : null };
}

// Checker-side: what a date/time looks like in parse()'s string mode.
export function stringifyDateTimes(enc) {
	if (enc.T) return { s: enc.s };
	if (enc.a) return { ...enc, a: enc.a.map(stringifyDateTimes) };
	if (enc.o) return { ...enc, o: enc.o.map(([k, x]) => [k, stringifyDateTimes(x)]) };
	return enc;
}
