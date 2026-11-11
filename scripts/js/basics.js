// Operators, conversions, equality, typeof (27o; compared with jsc)
var vals = [0, -0, 1, -1.5, NaN, Infinity, -Infinity, "", "0", "1", "abc", " 12 ", "0x1f", "1e3", true, false,
            null, undefined, [], [1], [1, 2], {}, function () {}];
function show(v) {
    if (typeof v === "string") return JSON.stringify(v);
    if (typeof v === "function") return "fn";
    if (Array.isArray(v)) return "[" + v.join(",") + "]";
    if (v === null) return "null";
    if (typeof v === "object") return "{}";
    if (v === 0 && 1 / v < 0) return "-0";
    return String(v);
}
for (var i = 0; i < vals.length; i++) {
    var v = vals[i];
    print(show(v), typeof v, Number(v), String(v === undefined ? "u" : v === null ? "n" : typeof v == "function" ? "f" : v), !!v, +v, -v, ~v, v + 1, v + "", v * 2);
}
for (var i = 0; i < vals.length; i++)
    for (var j = 0; j < vals.length; j++) {
        var a = vals[i], b = vals[j];
        if (typeof a == "function" || typeof b == "function") continue;
        print(show(a), show(b), a == b, a === b, a < b, a >= b, a + b, a - b);
    }
print(1 + 2 * 3 - 4 / 2 % 3, 2 ** 10, 2 ** -1, (-2) ** 3, 2 ** 3 ** 2);
print(7 % 3, -7 % 3, 7 % -3, 5.5 % 2, -0 % 1, 1 / (-0 % 1));
print(1 << 31, 1 << 32, -1 >>> 0, -1 >> 1, 0xFFFFFFFF | 0, 5 & 3, 5 | 3, 5 ^ 3, ~5, ~~3.7, 2 >>> 1);
print(0.1 + 0.2, 1 / 3, 100 / 3, 1e21, 1e-7, 123456789012345680000, 5e-324, 1.7976931348623157e308);
print(typeof typeof 1, typeof null, typeof undefined, typeof Symbol(), typeof {}, typeof [], typeof print);
print(null == undefined, null === undefined, NaN == NaN, NaN != NaN, [] == false, [0] == false, "" == 0, "1" == 1);
print([] + [], [] + {}, ({}) + "", [1, [2, 3]] + "", String([null, undefined, 1]));
var x = 5; x += 2; x -= 1; x *= 3; x /= 2; x %= 5; x **= 2; print(x);
var y = 6; y <<= 2; y >>= 1; y >>>= 1; y &= 7; y |= 8; y ^= 3; print(y);
var c = 0; print(c++, c, ++c, c, c--, c, --c, c);
var s = "a"; s += 1; s += null; print(s);
print(void 0, void "x", (1, 2, 3));
print(1 < 2 < 3, 3 > 2 > 1, "10" < "9", 10 < 9, "a" < "b", "B" < "a", null >= 0, undefined >= 0);
print(true + true, true + "1", null + 1, undefined + 1, "3" * "4", "3" - - "4", +"", +" ", +"\n", +"1_000");
print(isNaN("abc"), isNaN("12"), isFinite("12"), isFinite(Infinity), Number.isNaN("abc"));
var o = { valueOf() { return 42; }, toString() { return "str"; } };
print(o + 1, `${o}`, o * 2, String(o), o == 42, [o] + "");
print(!0, !1, !"", !"a", !null, !undefined, !{}, ![], !NaN);
print(1 && 2, 0 && 2, 1 || 2, 0 || 2, null ?? 3, 0 ?? 3, undefined ?? null ?? 4);
var n = null; n ??= 5; var z = 0; z ||= 9; var w = 1; w &&= 7; print(n, z, w);
print(typeof undeclaredVariable, delete Object.prototype, (function () { return typeof arguments; })());
print("in:", "length" in [], 0 in [1], 1 in [1], "x" in { x: 1 }, "toString" in {});
