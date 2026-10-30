import Foundation

/// 圆屏播报的声音打包：16 kHz 单声道 → 去掉首尾静音、音量拉满 → IMA ADPCM（四分之一大小）。
/// 格式与固件 VoiceClip.h 一致（小端）：'V' '1' 采样率(2) 采样数(4) 初始预测值(2) 初始步长序号(1) 保留(1) + 每字节两个采样，低 4 位在前。
enum ScreenVoicePacker {
    static let rate = 16000
    static let maxSamples = rate * 20                   // 圆屏一句最多收 20 秒
    static let header = 12

    /// 不到一点声音（全是静音）返回 nil。
    static func pack(_ pcm: [Int16]) -> Data? {
        let samples = prepare(pcm)
        guard !samples.isEmpty else { return nil }
        var out = Data([UInt8(ascii: "V"), UInt8(ascii: "1"), UInt8(rate & 0xFF), UInt8(rate >> 8)])
        let n = samples.count
        out.append(contentsOf: [UInt8(n & 0xFF), UInt8((n >> 8) & 0xFF), UInt8((n >> 16) & 0xFF), UInt8((n >> 24) & 0xFF), 0, 0, 0, 0])
        out.reserveCapacity(header + (n + 1) / 2)
        var predictor = 0, index = 0
        for (i, sample) in samples.enumerated() {
            let step = steps[index]
            var diff = Int(sample) - predictor
            var code = 0
            if diff < 0 { code = 8; diff = -diff }
            var delta = step >> 3
            if diff >= step { code |= 4; diff -= step; delta += step }
            if diff >= step >> 1 { code |= 2; diff -= step >> 1; delta += step >> 1 }
            if diff >= step >> 2 { code |= 1; delta += step >> 2 }
            predictor = min(32767, max(-32768, predictor + ((code & 8) != 0 ? -delta : delta)))
            index = min(88, max(0, index + moves[code & 7]))
            if i & 1 == 1 { out[out.count - 1] |= UInt8(code << 4) } else { out.append(UInt8(code)) }
        }
        return out
    }

    /// 播放时长（秒），手机据此估计圆屏什么时候念完。
    static func duration(_ clip: Data) -> Double {
        guard clip.count >= header else { return 0 }
        let n = Int(clip[4]) | Int(clip[5]) << 8 | Int(clip[6]) << 16 | Int(clip[7]) << 24
        return Double(n) / Double(rate)
    }

    /// 首尾静音只留 40 毫秒，省传输时间；整句放大到接近满幅，圆屏小喇叭听得清。最多 20 秒。
    static func prepare(_ pcm: [Int16]) -> [Int16] {
        let quiet = 300, margin = rate / 25
        guard let first = pcm.firstIndex(where: { abs(Int($0)) > quiet }),
              let last = pcm.lastIndex(where: { abs(Int($0)) > quiet }) else { return [] }
        let lo = max(0, first - margin), hi = min(pcm.count, last + margin + 1, lo + maxSamples)
        let body = pcm[lo..<hi]
        let peak = body.reduce(1) { max($0, abs(Int($1))) }
        let gain = min(4.0, 29000.0 / Double(peak))
        return body.map { Int16(clamping: Int((Double($0) * gain).rounded())) }
    }

    private static let steps = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
    private static let moves = [-1, -1, -1, -1, 2, 4, 6, 8]
}
