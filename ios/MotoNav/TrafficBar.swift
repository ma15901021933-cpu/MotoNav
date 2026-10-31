import Foundation

/// 路况光柱 → 圆屏消息 "#u,起点米,终点米,状态,..."（米 = 离路线起点）。
/// 1 畅通、2 缓行、3 拥堵、4 严重拥堵、6 极其畅通；未知、内部道路不发。
enum TrafficBar {
    static let maxChars = 480

    /// segments：高德给的一段段（长度米, 状态），按路线顺序。
    /// 如果加起来比全程短，说明是从当前位置往后算的：整体挪到路线末尾对齐。
    /// 已经走过的不发，留给前方（一条消息最多 480 字）。
    static func message(_ segments: [(length: Int, status: Int)], routeLength: Int, travelled: Int) -> String {
        let sum = segments.reduce(0) { $0 + max(0, $1.length) }
        var pos = routeLength > 0 && sum < routeLength - 30 ? routeLength - sum : 0
        var segs: [[Int]] = []
        for s in segments {
            let len = max(0, s.length), st = s.status
            if (st >= 1 && st <= 4 || st == 6) && len > 0 && pos + len > travelled {
                if let last = segs.last, last[2] == st, last[1] >= pos - 5 { segs[segs.count - 1][1] = pos + len }
                else { segs.append([pos, pos + len, st]) }
            }
            pos += len
        }
        var m = "#u"
        for sg in segs {
            let part = ",\(sg[0]),\(sg[1]),\(sg[2])"
            if m.count + part.count > maxChars { break }
            m += part
        }
        return m
    }
}
