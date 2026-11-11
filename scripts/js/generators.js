// Generators (27r): next, return, throw, yield*, finally; in methods, classes and loops
function* count(n) { for (let i = 0; i < n; i++) yield i; return "end"; }
var g = count(3);
print(JSON.stringify([g.next(), g.next(), g.next(), g.next(), g.next()]));
print([...count(4)].join(","));
function* talk() { var a = yield 1; print("got " + a); try { yield 2; } finally { print("cleanup"); } yield 3; }
var t = talk(); t.next(); t.next("x"); print(JSON.stringify(t.return(9))); print(JSON.stringify(t.next()));
function* thrower() { try { yield 1; } catch (e) { print("caught " + e); yield 2; } }
var h = thrower(); h.next(); print(JSON.stringify(h.throw("boom"))); print(JSON.stringify(h.next()));
function* inner() { var x = yield "a"; print("inner got " + x); return "r"; }
function* outer() { var r = yield* inner(); print("delegated returned " + r); yield* [7, 8]; }
var o = outer(); print(JSON.stringify([o.next(), o.next("v"), o.next(), o.next(), o.next()]));
print(Object.prototype.toString.call(count(1)), typeof count(1).next, count(1)[Symbol.iterator]() !== undefined);
for (const x of count(2)) print("for-of " + x);
function* args(a, b = a * 2) { yield arguments.length; yield b; }
print([...args(5)].join(","));
var it = count(5); for (const x of it) { if (x == 2) break; } print(JSON.stringify(it.next()));
function* nested() { for (const x of [1, 2]) { try { yield x; } finally { print("fin " + x); } } }
var n2 = nested(); n2.next(); print(JSON.stringify(n2.return("stop")));
function* inExpr() { var r = [1, (yield "mid"), 3]; return r; }
var ie = inExpr(); ie.next(); print(JSON.stringify(ie.next(2)));
var ie2 = (function* () { try { var r = [1, (yield "mid"), 3]; } finally { print("fin expr"); } })(); ie2.next(); print(JSON.stringify(ie2.return(5)));
try { var self = (function* () { self.next(); })(); self.next(); } catch (e) { print(e.constructor.name + ": " + e.message); }
var o = { *items() { yield* this.list; }, list: ["x", "y"], async get(v) { return this.k + v; }, k: "K" };
print([...o.items()].join(""));
o.get(1).then(v => print("method " + v));
class C { constructor() { this.n = 3; } *[Symbol.iterator]() { for (let i = 0; i < this.n; i++) yield i * 10; }
          static async make() { return new C(); } async total() { let s = 0; for (const v of this) s += await v; return s; } }
print(Array.from(new C()).join(","));
C.make().then(c => c.total()).then(t => print("total " + t));
function* tree(n) { if (n > 0) { yield* tree(n - 1); yield n; yield* tree(n - 1); } }
print([...tree(3)].join(""));
function* sw(x) { switch (x) { case 1: yield "one"; case 2: yield "two"; break; default: yield "other"; } yield "after"; }
print([...sw(1)].join(" ") + " / " + [...sw(5)].join(" "));
function* lab() { outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; yield i + "" + j; } } yield "end"; }
print([...lab()].join(","));
function* fin() { try { yield 1; return "try"; } finally { yield "in finally"; } }
var f = fin(); print(JSON.stringify([f.next(), f.next(), f.next(), f.next()]));
function* fin2() { try { yield 1; } finally { return "override"; } }
var f2 = fin2(); f2.next(); print(JSON.stringify(f2.return("asked")));
function* closures() { let fns = []; for (let i = 0; i < 3; i++) { fns.push(() => i); yield i; } yield fns.map(f => f()).join(""); }
print([...closures()].join("|"));
var [a, b, ...rest] = (function* () { yield 1; yield 2; yield 3; yield 4; })();
print(a, b, rest);
function* infinite() { let i = 0; while (true) yield i++; }
var inf = infinite(); var taken = []; for (const v of inf) { if (v > 3) break; taken.push(v); } print(taken, JSON.stringify(inf.next()));
var arrowThis = { v: 7, run() { return (async () => this.v + (await 1))(); } };
arrowThis.run().then(v => print("arrow this " + v));
async function loop() { let out = []; for (let i = 0; i < 3; i++) { out.push(await new Promise(r => r(i * i))); } return out; }
loop().then(v => print("loop " + v));
async function rethrow() { try { await Promise.reject(new Error("deep")); } catch (e) { throw new Error("wrapped " + e.message); } }
rethrow().catch(e => print(e.message));
async function awaitThenable() { return await { then(r) { r("thenable value"); } }; }
awaitThenable().then(print);
var gen = (function* () { var x = yield; print("received " + x); })(); gen.next("ignored"); gen.next("second");
function* returnsEarly() { yield 1; return; yield 2; } print([...returnsEarly()].length);
var g3 = (function* () { yield 1; })(); print(JSON.stringify(g3.return(5)), JSON.stringify(g3.next()));
var g4 = (function* () { yield 1; })(); try { g4.throw(new Error("before start")); } catch (e) { print("thrown before start: " + e.message); } print(JSON.stringify(g4.next()));
print(typeof (function* () {}).prototype, Object.getPrototypeOf((function* () {})()) === (function* () {}).prototype);
async function* ag() {} 
print("async generators parse");
