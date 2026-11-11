// Classes (27o)
class Animal {
    static count = 0;
    #secret = "hidden";
    name;
    legs = 4;
    constructor(name) { this.name = name; Animal.count++; }
    speak() { return `${this.name} makes a sound`; }
    get description() { return `${this.name} with ${this.legs} legs`; }
    set nickname(v) { this._nick = v.toUpperCase(); }
    static create(n) { return new this(n); }
    reveal() { return this.#secret; }
    #privateMethod() { return "pm:" + this.name; }
    callPrivate() { return this.#privateMethod(); }
    static #sp = 42;
    static getSp() { return Animal.#sp; }
    has(o) { return #secret in o; }
    toString() { return "Animal(" + this.name + ")"; }
}
class Dog extends Animal {
    constructor(name) { super(name); this.legs = 4; this.tricks = []; }
    speak() { return super.speak() + " (woof)"; }
    get description() { return "Dog: " + super.description; }
    static create(n) { const d = super.create(n); d.tricks.push("sit"); return d; }
}
var a = new Animal("cat"), d = Dog.create("rex");
print(a.speak(), d.speak(), a.description, d.description, d.tricks, Animal.count, d instanceof Animal, d instanceof Dog);
a.nickname = "kitty"; print(a._nick, a.reveal(), d.reveal(), a.callPrivate(), Animal.getSp(), a.has(d), a.has({}));
print(typeof Animal, Animal.name, Dog.name, Object.getPrototypeOf(Dog) === Animal, Dog.prototype.constructor === Dog, String(a));
print(Object.keys(a), Object.getOwnPropertyNames(Animal.prototype), JSON.stringify(d));
try { Animal(); } catch (e) { print("call:", e.name); }
try { a.reveal.call({}); } catch (e) { print("brand:", e.name); }
class Point { constructor(x, y) { this.x = x; this.y = y; } static from([x, y]) { return new Point(x, y); }
    add(o) { return new Point(this.x + o.x, this.y + o.y); } [Symbol.iterator]() { return [this.x, this.y][Symbol.iterator](); }
    get ["len" + "gth"]() { return Math.hypot(this.x, this.y); } }
var p = Point.from([1, 2]).add(new Point(2, 2)); print(p.x, p.y, [...p], p.length);
class Temp { static #instances = 0; static make() { Temp.#instances++; return Temp.#instances; } }
Temp.make(); print(Temp.make());
var Anon = class { m() { return "anon"; } }; print(Anon.name, new Anon().m(), (class Named {}).name);
class Base { constructor() { this.kind = new.target.name; } } class Derived extends Base {} print(new Derived().kind, new Base().kind);
class WithReturn { constructor() { return { custom: true }; } } print(JSON.stringify(new WithReturn()));
class Counter { #n = 0; inc() { this.#n++; return this; } get value() { return this.#n; } } print(new Counter().inc().inc().value);
class Fields { a = 1; b = this.a + 1; ["c" + 1] = 3; static s = "static"; arrow = () => this.a; }
var f = new Fields(); print(f.a, f.b, f.c1, Fields.s, f.arrow.call(null), Object.keys(f));
class ExtendsArray extends Array { sum() { return this.reduce((a, b) => a + b, 0); } }
var ea = new ExtendsArray(); ea.push(1, 2, 3); print(ea.sum(), ea.length, ea instanceof Array, Array.isArray(ea), ea.map(x => x * 2) instanceof ExtendsArray);
class MyError extends Error { constructor(m) { super(m); this.name = "MyError"; } }
try { throw new MyError("bad"); } catch (e) { print(e.name, e.message, e instanceof Error, e instanceof MyError, String(e)); }
class Accessors { static get sg() { return "sg"; } get g() { return 1; } set g(v) { this.v = v; } }
var acc = new Accessors(); acc.g = 5; print(Accessors.sg, acc.g, acc.v, Object.getOwnPropertyDescriptor(Accessors.prototype, "g").set !== undefined);
class StaticBlock { static x; static { StaticBlock.x = "init"; } } print(StaticBlock.x);
class Parent { m() { return "parent"; } } class Child extends Parent { m() { return "child>" + super.m(); } }
var obj = { __proto__: new Child(), m() { return "obj>" + super.m(); } }; print(obj.m());
class Chain extends Child { m() { return "chain>" + super.m(); } } print(new Chain().m());
try { class NoSuper extends Parent { constructor() { this.x = 1; } } new NoSuper(); } catch (e) { print("no super:", e.name); }
class Getter { constructor() { this._v = 1; } get v() { return this._v; } static compare(a, b) { return a.v - b.v; } }
print([new Getter(), new Getter()].sort(Getter.compare).length, typeof Getter.prototype.v, Object.keys(Getter.prototype));
class NullBase extends null { constructor() { return Object.create(NullBase.prototype); } } print(Object.getPrototypeOf(NullBase.prototype));
print(Object.getOwnPropertyNames(class { static a() {} static b = 1; }).sort().join());
class SuperProp { static base() { return "B"; } } class SubProp extends SuperProp { static sub() { return super.base() + "!"; } } print(SubProp.sub());
