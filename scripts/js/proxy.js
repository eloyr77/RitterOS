// Proxy (64r): every trap, the invariants, revocation, and the library
// functions that look through a proxy. Errors print only their kind (the
// messages are each engine's own).
function show(label, f) {
    try {
        print(label, f());
    } catch (e) {
        print(label, "threw", e.constructor.name);
    }
}

// A handler that logs every trap and forwards it with Reflect.
var log = [];
function logging(extra) {
    var h = {};
    ["get", "set", "has", "deleteProperty", "ownKeys", "getOwnPropertyDescriptor",
     "defineProperty", "getPrototypeOf", "setPrototypeOf", "isExtensible",
     "preventExtensions", "apply", "construct"].forEach(function (name) {
        h[name] = function (t, a) {
            log.push(name + (typeof a === "string" ? ":" + a : typeof a === "symbol" ? ":sym" : ""));
            return Reflect[name].apply(null, arguments);
        };
    });
    for (var k in extra)
        h[k] = extra[k];
    return h;
}
function flush(label) {
    print(label, log.join(" "));
    log = [];
}

var target = { a: 1, b: 2 };
var p = new Proxy(target, logging());
print(p.a, p.b, p.c);
flush("get");
p.c = 3;
flush("set");
print(target.c, "a" in p, "z" in p);
flush("has");
delete p.a;
flush("delete");
print(Object.keys(p).join(","));
flush("keys");
print(JSON.stringify(p));
flush("stringify");
var seen = [];
for (var k in p)
    seen.push(k);
print(seen.join(","));
flush("for-in");
Object.defineProperty(p, "d", { value: 4, enumerable: true, configurable: true, writable: true });
flush("define");
print(Object.getOwnPropertyDescriptor(p, "d").value);
flush("gopd");
print(Object.getPrototypeOf(p) === Object.prototype);
flush("getPrototypeOf");
print(Object.isExtensible(p));
flush("isExtensible");
print(Object.assign({}, p).d);
flush("assign");
print({ ...p }.b);
flush("spread");
print(Object.entries(p).length, Object.values(p).length);
flush("entries");
print(p.hasOwnProperty("b"), Object.hasOwn(p, "zz"));
flush("hasOwn");

// the receiver: a getter on the target sees the proxy as this
var withGetter = { get me() { return this === pg; } };
var pg = new Proxy(withGetter, {});
print("getter this", pg.me);

// a trap that changes values, a trap that hides keys
var doubled = new Proxy({ x: 2, y: 5 }, {
    get(t, k, r) { return typeof t[k] === "number" ? t[k] * 2 : Reflect.get(t, k, r); },
    has(t, k) { return k !== "y" && k in t; },
    ownKeys(t) { return Reflect.ownKeys(t).filter(k => k !== "y"); },
});
print(doubled.x, doubled.y, "y" in doubled, Object.keys(doubled).join(","), JSON.stringify(doubled));

// default values from a get trap
var defaults = new Proxy({}, { get: (t, k) => k in t ? t[k] : "default:" + String(k) });
defaults.set = 1;
print(defaults.set, defaults.missing, defaults[7]);

// functions: apply and construct
function add(a, b) { return a + b; }
var padd = new Proxy(add, logging());
print(typeof padd, padd(2, 3), padd.call(null, 4, 5), padd.apply(null, [6, 7]));
flush("apply");
function Point(x, y) { this.x = x; this.y = y; }
var PP = new Proxy(Point, logging());
var pt = new PP(1, 2);
print(pt.x, pt.y, pt instanceof Point, pt instanceof PP);
flush("construct");
var counted = 0;
var Counting = new Proxy(class Base { constructor(v) { this.v = v; } }, {
    construct(t, args, nt) { counted++; return Reflect.construct(t, args, nt); },
});
class Derived extends Counting { constructor() { super(42); } }
var dv = new Derived();
print("derived", dv.v, dv instanceof Derived, counted);
show("construct returns a primitive", () => new (new Proxy(function () {}, { construct: () => 1 }))());
show("call a proxy of an object", () => new Proxy({}, {})());
show("construct a proxy of an arrow", () => new (new Proxy(() => 1, {}))());
print(typeof new Proxy({}, {}), typeof new Proxy(function () {}, {}), typeof new Proxy(class {}, {}));

// arrays
var arr = new Proxy([1, 2, 3], {});
print(Array.isArray(arr), arr.length, JSON.stringify(arr), Object.prototype.toString.call(arr));
print([0].concat(arr).join(","), arr.map(x => x * 10).join(","), Array.from(arr).join(","));
arr.push(4);
print(arr.length, arr.join("-"), [...arr].length);
print(Array.isArray(new Proxy(new Proxy([], {}), {})), Array.isArray(new Proxy({}, {})));

// prototypes
var protoTarget = {};
var pp = new Proxy(protoTarget, { getPrototypeOf: () => Array.prototype });
print(Object.getPrototypeOf(pp) === Array.prototype, pp instanceof Array, Array.prototype.isPrototypeOf(pp));
var child = Object.create(new Proxy({ inherited: "yes" }, logging()));
print(child.inherited, "inherited" in child, child.nothing);
flush("as a prototype");
child.fresh = 1;
flush("set through a prototype");
print(Object.keys(child).join(","));

// invariants: a trap may not lie about what the target fixes
var fixed = {};
Object.defineProperty(fixed, "k", { value: 1, writable: false, configurable: false });
show("get: another value for a fixed property", () => new Proxy(fixed, { get: () => 2 }).k);
show("get: the same value", () => new Proxy(fixed, { get: () => 1 }).k);
show("has: hiding a fixed property", () => "k" in new Proxy(fixed, { has: () => false }));
show("delete: a fixed property", () => delete new Proxy(fixed, { deleteProperty: () => true }).k);
show("ownKeys: leaving a fixed key out", () => Object.keys(new Proxy(fixed, { ownKeys: () => [] })));
show("ownKeys: a number", () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => [1] })));
show("ownKeys: twice the same", () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => ["a", "a"] })));
show("ownKeys: not an object", () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => "ab" })));
show("ownKeys: symbols", () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => ["a", Symbol.iterator] })).length);
var sealed = Object.preventExtensions({ only: 1 });
show("ownKeys: an extra key on a non-extensible target", () => Reflect.ownKeys(new Proxy(sealed, { ownKeys: () => ["only", "extra"] })));
show("ownKeys: the target's keys exactly", () => Reflect.ownKeys(new Proxy(sealed, { ownKeys: () => ["only"] })).join());
show("gopd: undefined for a fixed property", () => Object.getOwnPropertyDescriptor(new Proxy(fixed, { getOwnPropertyDescriptor: () => undefined }), "k"));
show("gopd: non-configurable when the target's is not", () => Object.getOwnPropertyDescriptor(new Proxy({ c: 1 }, { getOwnPropertyDescriptor: () => ({ value: 1, configurable: false }) }), "c"));
show("gopd: a number", () => Object.getOwnPropertyDescriptor(new Proxy({}, { getOwnPropertyDescriptor: () => 5 }), "x"));
show("gopd: made up for an extensible target", () => JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy({}, { getOwnPropertyDescriptor: () => ({ value: 7, configurable: true }) }), "x")));
show("define: true without defining on a non-extensible target", () => Object.defineProperty(new Proxy(Object.preventExtensions({}), { defineProperty: () => true }), "n", { value: 1 }));
show("define: a trap saying no", () => Object.defineProperty(new Proxy({}, { defineProperty: () => false }), "n", { value: 1 }));
show("Reflect.defineProperty: a trap saying no", () => Reflect.defineProperty(new Proxy({}, { defineProperty: () => false }), "n", { value: 1 }));
show("isExtensible: a lie", () => Object.isExtensible(new Proxy({}, { isExtensible: () => false })));
show("preventExtensions: true while extensible", () => Object.preventExtensions(new Proxy({}, { preventExtensions: () => true })));
show("preventExtensions: false", () => Object.preventExtensions(new Proxy({}, { preventExtensions: () => false })));
show("Reflect.preventExtensions: false", () => Reflect.preventExtensions(new Proxy({}, { preventExtensions: () => false })));
show("getPrototypeOf: a number", () => Object.getPrototypeOf(new Proxy({}, { getPrototypeOf: () => 3 })));
show("getPrototypeOf: another for a non-extensible target", () => Object.getPrototypeOf(new Proxy(Object.preventExtensions({}), { getPrototypeOf: () => Array.prototype })));
show("setPrototypeOf: a trap saying no", () => Object.setPrototypeOf(new Proxy({}, { setPrototypeOf: () => false }), null));
show("Reflect.setPrototypeOf: a trap saying no", () => Reflect.setPrototypeOf(new Proxy({}, { setPrototypeOf: () => false }), null));
show("set: a fixed property", () => { "use strict"; new Proxy(fixed, { set: () => true }).k = 5; return "set"; });
show("set: a trap saying no, sloppy", () => { var q = new Proxy({}, { set: () => false }); q.x = 1; return q.x; });
show("set: a trap saying no, strict", () => { "use strict"; new Proxy({}, { set: () => false }).x = 1; });
show("Reflect.set: a trap saying no", () => Reflect.set(new Proxy({}, { set: () => false }), "x", 1));
show("delete: a trap saying no, strict", () => { "use strict"; delete new Proxy({ a: 1 }, { deleteProperty: () => false }).a; });
show("delete: a trap saying no, sloppy", () => delete new Proxy({ a: 1 }, { deleteProperty: () => false }).a);
show("a trap that is not a function", () => new Proxy({}, { get: 5 }).x);
show("a trap that throws", () => new Proxy({}, { get() { throw new RangeError("no"); } }).x);
show("Proxy without new", () => Proxy({}, {}));
show("Proxy of a number", () => new Proxy(1, {}));
show("Proxy with a null handler", () => new Proxy({}, null));
print("Proxy", typeof Proxy, Proxy.length, Proxy.name, "prototype" in Proxy, typeof Proxy.revocable);

// freeze and seal through the traps
var frozen = new Proxy({ f: 1 }, logging());
Object.freeze(frozen);
flush("freeze");
print(Object.isFrozen(frozen), Object.isSealed(frozen));
flush("isFrozen");

// revocation
var r = Proxy.revocable({ v: 1 }, {});
print(r.proxy.v, Object.keys(r).join(","), typeof r.revoke, r.revoke.length);
r.revoke();
r.revoke();
show("get after revoke", () => r.proxy.v);
show("in after revoke", () => "v" in r.proxy);
show("keys after revoke", () => Object.keys(r.proxy));
show("isArray after revoke", () => Array.isArray(r.proxy));
show("typeof after revoke", () => typeof r.proxy);
show("Proxy of a revoked proxy", () => typeof new Proxy(r.proxy, {}));
var rf = Proxy.revocable(function () { return 1; }, {});
rf.revoke();
show("call after revoke", () => rf.proxy());

// a proxy in its own target's prototype chain ends in a RangeError
var loopTarget = {};
var loop = new Proxy(loopTarget, {});
Object.setPrototypeOf(loopTarget, loop);
show("a lookup around a loop", () => loop.missing);
show("Object.setPrototypeOf around a loop through a proxy", () => Object.getPrototypeOf(loopTarget) === loop);

// private fields stay on the proxy itself
class Secret { #s = 1; static read(o) { return o.#s; } static has(o) { return #s in o; } }
var sec = new Secret();
show("a private field through a proxy", () => Secret.read(new Proxy(sec, {})));
print("private in", Secret.has(sec), Secret.has(new Proxy(sec, {})));

// the handler's traps are looked up each time
var live = {};
var lp = new Proxy({ x: 1 }, live);
print(lp.x);
live.get = () => "now trapped";
print(lp.x);
delete live.get;
print(lp.x);

// symbols as keys
var sk = Symbol("key");
var sp = new Proxy({ [sk]: "symbol value" }, logging());
print(sp[sk], sk in sp, Object.getOwnPropertySymbols(sp).length);
flush("symbols");

// a trap that throws inside the library's own loops
var badKeys = new Proxy({ a: 1 }, { ownKeys() { throw new SyntaxError("keys"); } });
var badDesc = new Proxy({ a: 1 }, { getOwnPropertyDescriptor() { throw new EvalError("desc"); } });
[badKeys, badDesc].forEach(function (bad, i) {
    show(i + " Object.keys", () => Object.keys(bad));
    show(i + " Object.assign", () => Object.assign({}, bad));
    show(i + " spread", () => ({ ...bad }));
    show(i + " JSON.stringify", () => JSON.stringify(bad));
    show(i + " JSON.stringify nested", () => JSON.stringify({ inner: [bad] }));
    show(i + " for-in", () => { for (var k in bad) ; return "done"; });
    show(i + " getOwnPropertyDescriptors", () => Object.getOwnPropertyDescriptors(bad));
    show(i + " defineProperties", () => Object.defineProperties({}, bad));
    show(i + " freeze", () => Object.freeze(bad));
    show(i + " isFrozen", () => Object.isFrozen(Object.preventExtensions(new Proxy({ a: 1 }, i ? { getOwnPropertyDescriptor() { throw new EvalError("d"); } } : { ownKeys() { throw new SyntaxError("k"); } }))));
    show(i + " entries", () => Object.entries(bad));
    show(i + " hasOwnProperty", () => bad.hasOwnProperty("a"));
    show(i + " JSON.parse reviver", () => JSON.parse('{"x":{"y":1}}', function (k, v) { return k === "x" ? bad : v; }));
});

// for-in: a non-enumerable own property hides the same name further up
// (until 64r the engine listed it from the prototype)
var hides = Object.create({ x: 1, y: 2 });
Object.defineProperty(hides, "x", { value: 3, enumerable: false });
var listed = [];
for (var name in hides)
    listed.push(name);
print("for-in hides", listed.join(","));

// a callable proxy's source is native code (the name is each engine's own)
var shown = Function.prototype.toString.call(new Proxy(function secret() { return "body"; }, logging()));
print("toString", shown.indexOf("[native code]") > 0, shown.indexOf("body") < 0);
flush("toString traps");
