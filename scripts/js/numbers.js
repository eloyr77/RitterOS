// Number formatting and parsing, Math (27o)
var nums = [0, 1, -1, 0.5, 1.5, 2.5, -2.5, 0.1, 0.000001, 0.0000001, 123.456, 1e21, 1e-21, 12345678901234567890,
            Math.PI, Math.E, 1 / 3, 2 / 3, 255, 1e100, 5e-324, 1.7976931348623157e308, 0.1 + 0.2, 4.35, 1.005, -0];
for (var i = 0; i < nums.length; i++) {
    var n = nums[i];
    print(n, String(n), n.toFixed(2), n.toFixed(0), n.toPrecision(3), n.toPrecision(1), n.toExponential(2),
          n.toExponential(), n.toString(16), n.toString(2).length, n.toString(36));
}
print((25).toString(2), (255).toString(16), (-255).toString(36), (0.5).toString(2), (0.1).toString(3));
print((1e21).toFixed(2), (123.456).toFixed(10), (0.000123).toPrecision(2), (123456).toPrecision(2), (1.45).toFixed(1));
print((0).toPrecision(5), (-0).toFixed(2), (1.23e-10).toFixed(5), (99.99).toPrecision(3), (9.995).toFixed(2));
var strs = ["", " ", "12", "  12  ", "1.5e3", ".5", "5.", "0x10", "0X1f", "0b101", "0o17", "-0x10", "1e", "e1",
            "Infinity", "-Infinity", "+Infinity", "infinity", "12px", "1,5", "\t\n42\n", "1_0", "0.0000001",
            "123456789012345678901234567890", "-0", "+5", "--5", "0x", "00012", "1e1000", "-1e-1000"];
for (var i = 0; i < strs.length; i++) {
    var s = strs[i];
    print(JSON.stringify(s), Number(s), parseInt(s), parseFloat(s), parseInt(s, 16), parseInt(s, 2), parseInt(s, 36));
}
print(parseInt("123abc"), parseInt("abc"), parseInt("  -42  "), parseInt("0x"), parseInt("1e3"), parseInt(0.0000005), parseInt("12", 37), parseInt("12", 1), parseInt("z", 36));
print(Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, Number.EPSILON, Number.MAX_VALUE, Number.MIN_VALUE);
print(Number.isInteger(5), Number.isInteger(5.5), Number.isSafeInteger(2 ** 53), Number.isSafeInteger(2 ** 53 - 1), Number.isFinite("5"));
print(Math.max(), Math.min(), Math.max(1, 3, 2), Math.min(1, -3, 2), Math.max(1, NaN), Math.max(-0, 0), 1 / Math.min(0, -0));
print(Math.round(2.5), Math.round(-2.5), Math.round(0.49999999999999994), Math.round(-0.4), 1 / Math.round(-0.4), Math.round(1.5));
print(Math.floor(-1.5), Math.ceil(-1.5), Math.trunc(-1.5), Math.sign(-3), Math.sign(0), Math.abs(-7), Math.cbrt(27));
print(Math.sqrt(2), Math.pow(2, 0.5), Math.pow(NaN, 0), Math.pow(1, Infinity), Math.hypot(3, 4), Math.hypot(), Math.hypot(1, 1, 1));
print(Math.clz32(1), Math.clz32(0), Math.imul(3, 4), Math.imul(0xffffffff, 5), Math.fround(5.5), Math.fround(5.05));
print(Math.log(Math.E), Math.log10(1000), Math.log2(8), Math.exp(0), Math.sin(0), Math.cos(0), Math.atan2(1, 1));
print(Math.sinh(0), Math.cosh(0), Math.tanh(0), Math.asinh(0), Math.expm1(0), Math.log1p(0));
var r = Math.random(); print(r >= 0 && r < 1, typeof r);
print((123.456).toString(), (1e21).toString(), (-1e-7).toString(), (1.5e300 * 1.5e300).toString());
print(0.1 * 3, 0.7 + 0.1, 1.1 * 1.1, 3 * 1.1, 1e16 + 1, 2 ** 53 + 1, 9007199254740993, 0.000001234);
print((12345.6789).toFixed(), (0.5).toFixed(0), (1.5).toFixed(0), (2.5).toFixed(0), (-1.5).toFixed(0), (1000000000000000128).toString(), (1000000000000000128).toFixed(0));
print(Number("1e+21"), 1e21 + 1, 123e-20, 1.0, 100, 1e2, 0.1e1);
