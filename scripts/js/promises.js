// Promises and async functions (27r): the order of their jobs as JavaScriptCore runs them
var log = [];
function out(s) { print(s); }
out("start");
Promise.resolve(1).then(v => out("then " + v));
new Promise((res, rej) => { out("executor"); res("a"); }).then(v => { out("res " + v); return v + "b"; }).then(v => out("chain " + v));
Promise.reject(new Error("no")).catch(e => out("caught " + e.message)).finally(() => out("finally"));
async function f(x) { out("f start " + x); var y = await x; out("f after await " + y); return y * 2; }
f(5).then(v => out("f gave " + v));
async function g() { try { await Promise.reject("bad"); } catch (e) { out("g caught " + e); } finally { out("g finally"); } return "g done"; }
g().then(out);
async function h() { throw new TypeError("thrown"); }
h().catch(e => out("h rejected " + e.name));
(async () => { out("arrow " + (await 1) + (await Promise.resolve(2))); })();
var thenable = { then(r) { out("thenable then"); r("T"); } };
Promise.resolve(thenable).then(v => out("thenable gave " + v));
Promise.all([1, Promise.resolve(2), new Promise(r => r(3))]).then(v => out("all " + v));
Promise.allSettled([Promise.resolve(1), Promise.reject("x")]).then(v => out("settled " + JSON.stringify(v)));
Promise.race([new Promise(() => {}), Promise.resolve("fast")]).then(v => out("race " + v));
Promise.any([Promise.reject(1), Promise.reject(2)]).catch(e => out("any " + e.constructor.name + " " + e.errors));
Promise.any([Promise.reject(1), Promise.resolve(2)]).then(v => out("any ok " + v));
var { promise, resolve } = Promise.withResolvers(); promise.then(v => out("withResolvers " + v)); resolve("w");
out(Object.prototype.toString.call(Promise.resolve()) + " " + (Promise.resolve() instanceof Promise));
async function order() { out("o1"); await null; out("o2"); await null; out("o3"); }
order(); Promise.resolve().then(() => out("p1")).then(() => out("p2")).then(() => out("p3"));
class MyP extends Promise {}
var mp = MyP.resolve(1).then(x => x);
out("subclass " + (mp instanceof MyP));
try { Promise(); } catch (e) { out(e.constructor.name); }

out("end");
