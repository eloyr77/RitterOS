// JSON (27o)
var v = { a: 1, b: [true, false, null], c: "str\n\"q\"\\ \u0001 é 😀", d: { e: {} }, f: undefined, g: () => 1, h: NaN, i: -0, j: 1e21, k: [undefined, function () {}] };
print(JSON.stringify(v));
print(JSON.stringify(v, null, 2));
print(JSON.stringify(v, ["a", "c"]), JSON.stringify(v, (k, x) => typeof x === "number" ? x * 2 : x));
print(JSON.stringify([1, [2, [3]]], null, "--"), JSON.stringify({ a: [] }, null, 4), JSON.stringify({}, null, 2), JSON.stringify([], null, 2));
print(JSON.stringify("x"), JSON.stringify(1), JSON.stringify(null), JSON.stringify(undefined), JSON.stringify(Symbol()), JSON.stringify(() => 1));
print(JSON.stringify(new Date(0)), JSON.stringify({ toJSON() { return "custom"; } }), JSON.stringify(new String("s")), JSON.stringify(new Number(3)), JSON.stringify(Object(false)));
print(JSON.stringify("\uD800"), JSON.stringify("\uDC00x"), JSON.stringify("  "), JSON.stringify("\t\b\f\r"));
var cyc = {}; cyc.self = cyc; try { JSON.stringify(cyc); } catch (e) { print(e.name); }
print(JSON.stringify({ [Symbol()]: 1, s: Symbol() }), JSON.stringify([Symbol()]), JSON.stringify({ a: 1 }, null, 20).length);
var p = JSON.parse('{"a":[1,2,{"b":null}],"c":"\\u00e9\\n","d":-1.5e3,"e":true,"f":{}}');
print(p.a[2].b, p.c, p.d, p.e, typeof p.f, Object.keys(p));
print(JSON.parse("[]").length, JSON.parse('"x"'), JSON.parse("  1  "), JSON.parse("null"), JSON.parse("-0") === 0, 1 / JSON.parse("-0"));
print(JSON.stringify(JSON.parse('{"__proto__": 1, "a": 2}')), JSON.parse('{"a":1,"a":2}').a);
print(JSON.stringify(JSON.parse('{"a":{"b":[1,2]}}', (k, v) => Array.isArray(v) ? v.length : v)));
var bad = ["{", "[1,]", "{'a':1}", '{"a":1,}', "01", "1.", ".5", "+1", "NaN", "undefined", '"\\x41"', '"a\nb"', "[1 2]", "", "tru", '{"a" 1}'];
for (var i = 0; i < bad.length; i++) { try { JSON.parse(bad[i]); print("parsed", bad[i]); } catch (e) { print(e.name); } }
print(JSON.stringify({ date: new Date(Date.UTC(2020, 0, 2, 3, 4, 5, 6)) }));
print(JSON.stringify([new Map(), new Set([1]), /re/g, new Error("e")]));
var nested = []; for (var i = 0; i < 100; i++) nested = [nested]; print(JSON.stringify(nested).length);
print(JSON.stringify("\u007f\u0080ÿ"), JSON.stringify({ "": 1, " ": 2 }), JSON.stringify([0.1, 1e-7, 123456789012]));
print(JSON[Symbol.toStringTag], JSON.stringify.length, JSON.parse.length);
