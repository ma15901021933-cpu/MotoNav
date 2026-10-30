import Foundation

var checks = 0
func check(_ value: @autoclosure () -> Bool, _ label: String) { precondition(value(), label); checks += 1 }
let silence = [Int16](repeating: 0, count: 8000)
let tone = (0..<16000).map { Int16(6000 * sin(2 * Double.pi * 440 * Double($0) / 16000)) }
check(ScreenVoicePacker.pack(silence) == nil, "Silence alone is not sent")
let clip = ScreenVoicePacker.pack(silence + tone + silence)!
let n = Int(clip[4]) | Int(clip[5]) << 8 | Int(clip[6]) << 16 | Int(clip[7]) << 24
check(clip[0] == UInt8(ascii: "V") && clip[1] == UInt8(ascii: "1") && Int(clip[2]) | Int(clip[3]) << 8 == 16000, "Header matches firmware VoiceClip")
check(clip.count == ScreenVoicePacker.header + (n + 1) / 2, "Four bits per sample")
check(n < 16000 + 2 * 640 + 2 && n > 15900, "Leading/trailing silence trimmed to 40 ms margins")
check(abs(ScreenVoicePacker.duration(clip) - Double(n) / 16000) < 1e-9, "Duration from header")
let loud = ScreenVoicePacker.prepare(tone)
check(loud.map { abs(Int($0)) }.max()! > 23000 && loud.map { abs(Int($0)) }.max()! <= 29001, "Quiet speech normalised near full scale")
let full = (0..<1000).map { Int16($0 % 2 == 0 ? 32767 : -32768) }
check(ScreenVoicePacker.prepare(full).allSatisfy { abs(Int($0)) <= 32768 }, "No overflow when already loud")
let long = [Int16](repeating: 5000, count: 16000 * 25)
check(ScreenVoicePacker.prepare(long).count == ScreenVoicePacker.maxSamples, "Clipped to firmware 20 s limit")
if CommandLine.arguments.count > 1 { try! clip.write(to: URL(fileURLWithPath: CommandLine.arguments[1])) }
print("Screen voice packer: \(checks) checks passed")
