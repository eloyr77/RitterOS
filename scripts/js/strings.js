// String methods (27o)
var s = "Hello, World! héllo ñ 😀 end";
print(s.length, s.charAt(1), s.charCodeAt(1), s.codePointAt(22), s.at(-1), s.at(100), s[4], s["length"]);
print(s.indexOf("o"), s.indexOf("o", 5), s.lastIndexOf("o"), s.lastIndexOf("o", 5), s.indexOf(""), s.lastIndexOf(""), s.indexOf("zz"));
print(s.includes("World"), s.startsWith("Hell"), s.endsWith("end"), s.startsWith("World", 7), s.endsWith("Hello", 5));
print(s.slice(0, 5), s.slice(-3), s.slice(3, -3), s.substring(5, 2), s.substring(-1, 3), s.substr(-3, 2), s.substr(1));
print(s.toUpperCase(), s.toLowerCase(), "ÀÉÎõü".toLowerCase(), "straße".toUpperCase(), "ΣΑΣ".toLowerCase());
print("  trim me  ".trim() + "|", "  x  ".trimStart() + "|", "  x  ".trimEnd() + "|", "\t\n x  ".trim().length);
print("ab".repeat(3), "abc".padStart(6, "12"), "abc".padEnd(7, "xy"), "abc".padStart(2), "5".padStart(3, "0"));
print("a,b,,c".split(","), "a,b,,c".split(",", 2), "abc".split(""), "".split(","), "".split(""), "a b c".split(" ").length);
print("a-b-c".replace("-", "+"), "a-b-c".replaceAll("-", "+"), "aaa".replace("a", "$&$&"), "abc".replace("b", "[$`|$']"));
print("x".concat(1, 2, null), String.fromCharCode(72, 105, 0x1F600 & 0xffff), String.fromCodePoint(0x1F600, 65));
print("abc" < "abd", "a" < "B", "Z" < "a", "abc".localeCompare("abd"), "b".localeCompare("a"), "a".localeCompare("a"));
print([..."a😀b"].length, Array.from("héllo").join("|"), "😀".length, "😀".codePointAt(0), "😀".charCodeAt(1));
var t = `multi
line ${1 + 1} ${"x".toUpperCase()} \${not} A \x42`;
print(t, t.length);
function tag(strings, ...vals) { return strings.raw.join("|") + "#" + strings.join("|") + "#" + vals.join(","); }
print(tag`a${1}b\n${2}c`, tag``, String.raw`x\ny${3}`);
print("abc".split("").reverse().join(""), "Hello"[0] + "Hello".slice(1).toLowerCase());
print(escape === undefined ? "" : "", encodeURIComponent("a b&c=d/é😀"), encodeURI("http://x.com/a b?q=1&r=é#h"));
print(decodeURIComponent("a%20b%26c%3Dd%2F%C3%A9"), decodeURI("a%20b%26%3D"), decodeURIComponent("%F0%9F%98%80"));
try { decodeURIComponent("%E0%A4%A"); } catch (e) { print(e.name); }
print(String(123), String(null), String([1, [2]]), String({}), String(Symbol("d")), new String("x").length, typeof new String("x"));
print("abc".match(/b/).index, "a1b2c3".match(/\d/g), "x".match(/y/), "aaa".search(/a/), "abc".search("c"));
print("%" + "a\tb\\c\"d'e".length, JSON.stringify("a\tb\\c\"d\u0001"), "\0".length);
var big = ""; for (var i = 0; i < 1000; i++) big += i; print(big.length, big.slice(990, 1000), big.indexOf("999"));
print("abc".isWellFormed(), "a\uD800b".isWellFormed(), "a\uD800b".toWellFormed().charCodeAt(1));
print("ab".anchor("x\"y"), "Ab".link("u"), "q".bold());
print([..."abc"].map((c, i) => c + i).join(), "a-b_c".split(/[-_]/), "A1B22C333".split(/\d+/), "abc".split(/(b)/));
print("Hello World".replace(/o/g, "0"), "John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "aaa".replace(/a/g, (m, i) => i));
print("x".codePointAt(5), "abc".charAt(-1) === "", "abc".substring(NaN, 2), "abc".slice(NaN), "abcdef".substr(2, NaN));
print("  12  ".length, "ab cd".length, "tab\there".split("\t"), "1,2,3".split(",").map(Number));
