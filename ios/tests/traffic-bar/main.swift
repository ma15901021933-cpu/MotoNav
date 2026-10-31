import Foundation

var checks = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); exit(1) }
    checks += 1
}

// Whole-route list: positions start at the route start; unknown (0) and internal roads (5) are skipped.
let whole = TrafficBar.message([(1000, 1), (500, 3), (200, 0), (300, 6), (100, 5)], routeLength: 2100, travelled: 0)
check(whole == "#u,0,1000,1,1000,1500,3,1700,2000,6", "whole route keeps positions from the route start: \(whole)")

// Same statuses next to each other merge into one section.
check(TrafficBar.message([(400, 1), (600, 1)], routeLength: 1000, travelled: 0) == "#u,0,1000,1", "adjacent equal sections merge")

// List that only covers the road ahead is moved so it ends at the route end.
let ahead = TrafficBar.message([(300, 2), (700, 1)], routeLength: 5000, travelled: 4000)
check(ahead == "#u,4000,4300,2,4300,5000,1", "remaining-only list aligns to the route end: \(ahead)")

// Sections already passed are not sent.
let passed = TrafficBar.message([(1000, 3), (1000, 1), (1000, 2)], routeLength: 3000, travelled: 1500)
check(passed == "#u,1000,2000,1,2000,3000,2", "passed sections are dropped: \(passed)")

// Long lists stay within one message and keep what is nearest ahead.
var many: [(length: Int, status: Int)] = []
for i in 0..<200 { many.append((100, i % 2 == 0 ? 1 : 2)) }
let long = TrafficBar.message(many, routeLength: 20000, travelled: 10000)
check(long.count <= TrafficBar.maxChars, "message fits in one BLE write")
check(long.hasPrefix("#u,10000,10100,1"), "nearest section ahead comes first: \(long.prefix(30))")

let small = TrafficBar.message(many, routeLength: 20000, travelled: 10000, maxChars: 182)
check(small.count <= 182 && small.hasPrefix("#u,10000,10100,1"), "message fits a smaller BLE packet: \(small.count)")

// Nothing usable clears the screen's traffic.
check(TrafficBar.message([(500, 0)], routeLength: 500, travelled: 0) == "#u", "unknown only clears traffic")
print("Traffic bar: \(checks) checks passed")
