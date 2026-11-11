// Functions: closures, this, call/apply/bind, arguments, parameters (27o)
function add(a, b = 10, ...rest) { return a + b + rest.length; }
print(add(1), add(1, 2), add(1, 2, 3, 4), add.length, add.name, (function () {}).name, (() => 1).name);
var named = function inner() { return typeof inner; }; print(named(), named.name, typeof inner);
var counter = (function () { var n = 0; return { inc: () => ++n, get: () => n }; })(); counter.inc(); counter.inc(); print(counter.get());
function makeAdders() { var fs = []; for (var i = 0; i < 3; i++) fs.push(function () { return i; }); return fs; }
print(makeAdders().map(f => f()));
function makeAdders2() { var fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); return fs; } print(makeAdders2().map(f => f()));
function who() { return this === undefined ? "undef" : this === globalThis ? "global" : typeof this; }
function strictWho() { "use strict"; return this === undefined ? "undef" : typeof this; }
print(who(), strictWho(), who.call(5), strictWho.call(5), who.call(null), strictWho.call(null), who.apply("s"));
var bound = who.bind({}); print(bound(), bound.name, bound.length, add.bind(null, 1).length, add.bind(null, 1)(5));
function Point(x, y) { this.x = x; this.y = y; } Point.prototype.norm = function () { return Math.hypot(this.x, this.y); };
var pt = new Point(3, 4); print(pt.norm(), pt instanceof Point, pt.constructor === Point, Object.keys(pt));
var BP = Point.bind(null, 1); var bp = new BP(2); print(bp.x, bp.y, bp instanceof Point, bp instanceof BP);
function args() { return arguments.length + ":" + Array.prototype.join.call(arguments, ",") + ":" + typeof arguments; }
print(args(), args(1, 2, 3), (function (a) { arguments[0] = 9; return a; })(1), (function () { return [...arguments]; })(4, 5));
var arrowThis = { m() { return [1, 2].map(x => this.k * x); }, k: 3 }; print(arrowThis.m());
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); } print(fact(20), fact(170) > 1e306, fact(171));
function deep(n) { return n === 0 ? 0 : 1 + deep(n - 1); } print(deep(5000));
try { (function r() { r(); })(); } catch (e) { print("overflow:", e instanceof RangeError); }
var compose = (...fs) => x => fs.reduceRight((v, f) => f(v), x); print(compose(x => x + 1, x => x * 2)(5));
function defaults(a, b = a + 1, c = b * 2) { return [a, b, c]; } print(defaults(1), defaults(1, 5), defaults(1, undefined, 3));
function destr({ a, b: [c, d = 4] = [], ...rest } = {}) { return [a, c, d, JSON.stringify(rest)]; } print(destr({ a: 1, b: [2], z: 9 }), destr());
print((function () { return typeof this; }).call(true), (function () { "use strict"; return typeof this; }).call(true));
var self = function () { return self; }; print(self() === self);
print(Function.prototype.toString.call(function f(a) { return a; }), (x => x * 2).toString(), Math.max.toString());
var sum = new Function("a", "b", "return a + b"); print(sum(2, 3), sum.name, sum.length, new Function("return 7")());
print((function () { return arguments.callee.name; })(), Function.prototype.call.length, typeof Function.prototype);
function hoisted() { return inner(); function inner() { return "hoisted inner"; } } print(hoisted());
print(typeof laterFn, laterFn()); function laterFn() { return "later"; }
var v1 = 1; { let v1 = 2; print(v1); } print(v1);
try { print(tdz); let tdz = 1; } catch (e) { print(e.name); }
const cc = 1; try { cc = 2; } catch (e) { print(e.name); }
function counterGen() { let c = 0; return { next() { return ++c; } }; } var cg = counterGen(); cg.next(); print(cg.next());
var memo = (function () { var cache = {}; return function fib(n) { if (n < 2) return n; if (cache[n]) return cache[n]; return cache[n] = fib(n - 1) + fib(n - 2); }; })();
print(memo(80));
var o = { f() { return function () { return this; }; } }; print(o.f()() === globalThis);
var call = Function.prototype.call; print(call.call(function () { return this.v; }, { v: "cc" }));
print([1, 2, 3].map(parseInt), ["1", "2", "3"].map(Number), [" a ", "b "].map(Function.prototype.call, String.prototype.trim));
var x = 10; function shadow(x) { { var x = 20; } return x; } print(shadow(1));
print((function (a, b) {}).length, ((a, ...b) => 0).length, ((a = 1, b) => 0).length, (function (...a) {}).length);
