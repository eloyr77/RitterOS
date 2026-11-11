// fontref.swift TTFTEST FONT...: RitterOS's font reader against macOS's
// CoreText (41c). TTFTEST is user/test/ttftest built on the Mac; its
// `dump FONT` gives what our reader (user/lib/ttf.c) makes of a font: its
// sizes, every glyph's advance and outline (moves, lines, quadratic and
// cubic curves), every character's glyph. CoreText reads the same file at
// a size of one font unit a unit, and the two must agree: the units per em,
// the glyph count, hhea's ascender, descender and line gap, every advance,
// every outline (each contour closed, compared from any starting point,
// lines of no length left out; points within 0.01 unit), and the glyph of
// every character from U+0000 to U+10FFFF. Two things are CoreText's own,
// not the font's, and allowed: a contour beginning with a control point
// starts at the midpoint of its last and first points rounded down to
// whole units (TrueType's rule, and ours, is the midpoint itself: half a
// unit apart there), and a character the font does not map may be given
// the glyph of one it does (tab, line feed and no-break space the space's,
// U+2011 the hyphen's, U+FEFF U+200B's...). CoreText also leaves out
// contours of fewer than three distinct points, and turns some contours
// round (overlapping ones in some fonts): those are counted and named, and
// FreeFont has none. Build with
// `swiftc -O -o fontref scripts/fontref.swift`.
import CoreText
import Foundation

struct Segment {
    var kind: Character                 // L, Q or C
    var points: [CGPoint]               // control points, then the end
}

func near(_ a: CGPoint, _ b: CGPoint) -> Bool {
    return abs(a.x - b.x) <= 0.01 && abs(a.y - b.y) <= 0.01
}

// A contour as segments from its start round to it again, without
// segments of no length; none for a contour of fewer than 3 distinct
// points (no area: CoreText leaves those out).
func closed(start: CGPoint, _ segments: [Segment]) -> [Segment] {
    var out: [Segment] = []
    var at = start
    var all = segments
    if let last = segments.last?.points.last, !near(last, start) {
        all.append(Segment(kind: "L", points: [start]))
    }
    var distinct: [CGPoint] = [start]
    for s in all {
        if s.points.allSatisfy({ near($0, at) }) { continue }
        out.append(s)
        at = s.points.last!
        for p in s.points where !distinct.contains(where: { near($0, p) }) { distinct.append(p) }
    }
    return distinct.count < 3 ? [] : out
}

// The same contour gone round the other way (from the same start).
func turned(_ c: [Segment]) -> [Segment] {
    guard let start = c.last?.points.last else { return c }
    var froms: [CGPoint] = []
    var at = start
    for s in c {
        froms.append(at)
        at = s.points.last!
    }
    var out: [Segment] = []
    for (s, from) in zip(c, froms).reversed() {
        out.append(Segment(kind: s.kind, points: Array(s.points.dropLast().reversed()) + [from]))
    }
    return out
}

// Are two closed contours the same, from some starting segment? b's start
// may be its rounded midpoint (CoreText's): half a unit off there.
func same(_ a: [Segment], _ b: [Segment], bStart: CGPoint) -> Bool {
    if a.count != b.count { return false }
    if a.isEmpty { return true }
    func close(_ p: CGPoint, _ q: CGPoint) -> Bool {
        return near(p, q) || (q == bStart && abs(p.x - q.x) <= 0.5 && abs(p.y - q.y) <= 0.5)
    }
    for r in 0..<a.count {
        var ok = true
        for i in 0..<a.count {
            let x = a[(i + r) % a.count], y = b[i]
            if x.kind != y.kind || x.points.count != y.points.count ||
               !zip(x.points, y.points).allSatisfy({ close($0, $1) }) {
                ok = false
                break
            }
        }
        if ok { return true }
    }
    return false
}

struct Glyph {
    var advance = 0.0
    var contours: [[Segment]] = []
    var starts: [CGPoint] = []          // (CoreText's: each contour's start)
}

struct Font {
    var upem = 0, glyphs = 0, ascender = 0, descender = 0, gap = 0
    var glyph: [Glyph] = []
    var chars: [UInt32: Int] = [:]
}

func ours(_ ttftest: String, _ path: String) -> Font? {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: ttftest)
    p.arguments = ["dump", path]
    let pipe = Pipe()
    p.standardOutput = pipe
    do { try p.run() } catch { return nil }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    guard let text = String(data: data, encoding: .utf8) else { return nil }
    var f = Font()
    var start = CGPoint.zero
    var segments: [Segment] = []
    for line in text.split(separator: "\n") {
        let w = line.split(separator: " ")
        let n = w.dropFirst().compactMap { Double($0) }
        func pts() -> [CGPoint] { stride(from: 0, to: n.count - 1, by: 2).map { CGPoint(x: n[$0], y: n[$0 + 1]) } }
        switch w[0] {
        case "font":
            f.upem = Int(w[2])!; f.glyphs = Int(w[4])!; f.ascender = Int(w[6])!
            f.descender = Int(w[8])!; f.gap = Int(w[10])!
        case "glyph":
            f.glyph.append(Glyph(advance: Double(w[3])!, contours: []))
        case "M":
            start = pts()[0]; segments = []
        case "L", "Q", "C":
            segments.append(Segment(kind: Character(String(w[0])), points: pts()))
        case "Z":
            let c = closed(start: start, segments)
            if !c.isEmpty { f.glyph[f.glyph.count - 1].contours.append(c) }
        case "char":
            f.chars[UInt32(w[1])!] = Int(w[2])!
        default:
            break
        }
    }
    return f
}

func coreText(_ path: String) -> (Font, Double, Double, Double)? {
    let url = URL(fileURLWithPath: path) as CFURL
    guard let descs = CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor], !descs.isEmpty
    else { return nil }
    let probe = CTFontCreateWithFontDescriptor(descs[0], 1000, nil)
    let upem = Int(CTFontGetUnitsPerEm(probe))
    let font = CTFontCreateWithFontDescriptor(descs[0], CGFloat(upem), nil)
    var f = Font()
    f.upem = upem
    f.glyphs = CTFontGetGlyphCount(font)
    for g in 0..<f.glyphs {
        var gg = CGGlyph(g)
        var adv = CGSize()
        CTFontGetAdvancesForGlyphs(font, .horizontal, &gg, &adv, 1)
        var glyph = Glyph(advance: Double(adv.width), contours: [])
        if let path = CTFontCreatePathForGlyph(font, gg, nil) {
            var start = CGPoint.zero
            var segments: [Segment] = []
            var open = false
            path.applyWithBlock { e in
                let p = e.pointee.points
                switch e.pointee.type {
                case .moveToPoint:
                    if open {
                        let c = closed(start: start, segments)
                        if !c.isEmpty { glyph.contours.append(c); glyph.starts.append(start) }
                    }
                    start = p[0]; segments = []; open = true
                case .addLineToPoint: segments.append(Segment(kind: "L", points: [p[0]]))
                case .addQuadCurveToPoint: segments.append(Segment(kind: "Q", points: [p[0], p[1]]))
                case .addCurveToPoint: segments.append(Segment(kind: "C", points: [p[0], p[1], p[2]]))
                case .closeSubpath:
                    let c = closed(start: start, segments)
                    if !c.isEmpty { glyph.contours.append(c); glyph.starts.append(start) }
                    open = false
                @unknown default: break
                }
            }
            if open {
                let c = closed(start: start, segments)
                if !c.isEmpty { glyph.contours.append(c); glyph.starts.append(start) }
            }
        }
        f.glyph.append(glyph)
    }
    for c in UInt32(0)...0x10FFFF {
        if c >= 0xD800 && c <= 0xDFFF { continue }
        var units = Array(String(UnicodeScalar(c)!).utf16)
        var glyphs = [CGGlyph](repeating: 0, count: units.count)
        if CTFontGetGlyphsForCharacters(font, &units, &glyphs, units.count) && glyphs[0] != 0 {
            f.chars[c] = Int(glyphs[0])
        }
    }
    return (f, Double(CTFontGetAscent(font)), Double(CTFontGetDescent(font)), Double(CTFontGetLeading(font)))
}

// (--system) The Mac's own TrueType and OpenType fonts (.ttf, .otf in
// /System/Library/Fonts and its Supplemental folder): those our reader
// opens, except variable ones
// (an fvar table: CoreText draws an instance of them, our reader the
// outlines as stored). They cover what FreeFont does not use, such as
// format 4's indirect glyph lists. Each one only its summary line.
func systemFonts(_ ttftest: String) -> ([String], Int, Int) {
    var paths: [String] = []
    var variable = 0, refused = 0
    for dir in ["/System/Library/Fonts", "/System/Library/Fonts/Supplemental"] {
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).sorted()
        for n in names where n.hasSuffix(".ttf") || n.hasSuffix(".otf") {
            let path = dir + "/" + n
            let url = URL(fileURLWithPath: path) as CFURL
            guard let d = (CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor])?.first
            else { continue }
            let font = CTFontCreateWithFontDescriptor(d, 1000, nil)
            if CTFontCopyTable(font, CTFontTableTag(kCTFontTableFvar), []) != nil { variable += 1; continue }
            let p = Process()
            p.executableURL = URL(fileURLWithPath: ttftest)
            p.arguments = ["dump", path]
            p.standardOutput = FileHandle.nullDevice
            if (try? p.run()) == nil { continue }
            p.waitUntilExit()
            if p.terminationStatus == 1 { refused += 1; continue }
            paths.append(path)
        }
    }
    return (paths, variable, refused)
}

let args = CommandLine.arguments
if args.count < 3 {
    print("usage: fontref TTFTEST FONT... | fontref TTFTEST --system")
    exit(2)
}
var failed = false
var fonts = Array(args[2...])
var quiet = false
if fonts == ["--system"] {
    let (paths, variable, refused) = systemFonts(args[1])
    print("fontref: the Mac's fonts: \(paths.count) fonts (\(variable) variable ones and \(refused) our reader does not open left out)")
    fonts = paths
    quiet = true
}
var agreed = 0
for path in fonts {
    let name = (path as NSString).lastPathComponent
    guard let a = ours(args[1], path), let (b, ascent, descent, leading) = coreText(path) else {
        print("fontref: \(name): cannot read it"); failed = true; continue
    }
    var problems: [String] = []
    if a.upem != b.upem || a.glyphs != b.glyphs || a.glyph.count != b.glyphs {
        problems.append("units per em \(a.upem) / \(b.upem), glyphs \(a.glyphs) (\(a.glyph.count)) / \(b.glyphs)")
    }
    if abs(Double(a.ascender) - ascent) > 0.5 || abs(Double(-a.descender) - descent) > 0.5 ||
       abs(Double(a.gap) - leading) > 0.5 {
        problems.append("sizes \(a.ascender) \(a.descender) \(a.gap) / \(ascent) \(-descent) \(leading)")
    }
    var outlines = 0, advances = 0, turnedRound = 0
    for g in 0..<min(a.glyph.count, b.glyph.count) {
        if abs(a.glyph[g].advance - b.glyph[g].advance) > 0.01 {
            advances += 1
            if problems.count < 8 { problems.append("glyph \(g)'s advance \(a.glyph[g].advance) / \(b.glyph[g].advance)") }
        }
        let x = a.glyph[g].contours, y = b.glyph[g].contours, st = b.glyph[g].starts
        var ok = x.count == y.count
        for i in 0..<(ok ? x.count : 0) where !same(x[i], y[i], bStart: st[i]) {
            if same(turned(x[i]), y[i], bStart: st[i]) { turnedRound += 1 } else { ok = false }
        }
        if !ok {
            outlines += 1
            if problems.count < 8 { problems.append("glyph \(g)'s outline: \(x.count) contours / \(y.count)") }
        }
    }
    var chars = 0
    let mapped = Set(a.chars.values)                    // (CoreText's own substitutes: glyphs of others)
    for (c, g) in b.chars where a.chars[c] != g {
        if a.chars[c] == nil && mapped.contains(g) { continue }
        chars += 1
        if problems.count < 20 { problems.append(String(format: "U+%04X: glyph \(a.chars[c] ?? 0) / \(g)", c)) }
    }
    for (c, g) in a.chars where b.chars[c] == nil {
        chars += 1
        if problems.count < 20 { problems.append(String(format: "U+%04X: glyph \(g) / none", c)) }
    }
    if problems.isEmpty {
        let round = turnedRound > 0 ? " (\(turnedRound) contours CoreText turned round)" : ""
        agreed += 1
        if !quiet {
            print("fontref: \(name): \(a.glyphs) glyphs' outlines and advances, \(a.chars.count) characters, the sizes: as CoreText's\(round)")
        }
    } else {
        failed = true
        print("fontref: \(name): \(outlines) outlines, \(advances) advances, \(chars) characters differ")
        for p in problems { print("fontref:   \(p)") }
    }
}
if quiet {
    print("fontref: the Mac's fonts: \(agreed) of \(fonts.count) as CoreText reads them")
}
exit(failed ? 1 : 0)
