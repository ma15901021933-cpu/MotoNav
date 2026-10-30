import AVFoundation

/// 版本：跟随 App 版本（见 Config.swift）
///
/// 导航播报：高德把播报文字交给我们，用 iPhone 自带的语音念出来。
/// 播报时把音乐压低（“压低音乐”开关），念完 0.4 秒后恢复；连着蓝牙耳机/头盔时从耳机里出声。
/// 打开“从圆屏喇叭播报”后改由 ScreenVoice 合成成声音数据发给圆屏，手机喇叭和音频通道完全不用。
final class Speech: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = Speech()
    private let synth = AVSpeechSynthesizer()
    private var automaticVoice: AVSpeechSynthesisVoice?
    private var busy = 0
    private var releaseWork: DispatchWorkItem?
    private var phoneSpeaking = false
    /// 高德问“还在念吗”：手机或圆屏任一边还没念完都算。
    var isSpeaking: Bool { phoneSpeaking || ScreenVoice.shared.isSpeaking }

    override init() {
        super.init()
        synth.delegate = self
        automaticVoice = Self.preferredChineseVoice()
    }

    /// 只自动选择普通话。用 identifier 稳定排序，避免同级音色随枚举顺序切换。
    private static func preferredChineseVoice() -> AVSpeechSynthesisVoice? {
        let voices = AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.language == "zh-CN" }
            .sorted { $0.identifier < $1.identifier }
        if #available(iOS 16.0, *) {
            if let premium = voices.first(where: { $0.quality == .premium }) {
                return premium
            }
        }
        return voices.first(where: { $0.quality == .enhanced })
            ?? AVSpeechSynthesisVoice(language: "zh-CN")
    }

    func selectedVoice() -> AVSpeechSynthesisVoice? {
        let id = UserDefaults.standard.string(forKey: "voice_id") ?? ""
        if !id.isEmpty, let selected = AVSpeechSynthesisVoice(identifier: id) {
            return selected
        }
        // 每次自动播报前刷新，下载／删除系统音色后无需重装或修改设置。
        // 不把自动结果写入 voice_id，以便以后继续自动升级音色。
        automaticVoice = Self.preferredChineseVoice()
        return automaticVoice
    }

    private func configureSession(active: Bool) {
        let s = AVAudioSession.sharedInstance()
        do {
            if active {
                let opts: AVAudioSession.CategoryOptions = Settings.shared.duck
                    ? [.duckOthers, .interruptSpokenAudioAndMixWithOthers]
                    : [.mixWithOthers]
                try s.setCategory(.playback, mode: .voicePrompt, options: opts)
                try s.setActive(true)
            } else {
                try s.setActive(false, options: .notifyOthersOnDeactivation)   // 让音乐恢复音量
            }
        } catch {
            print("音频设置失败：\(error)")
        }
    }

    func speak(_ text: String) {
        if Settings.shared.voiceMuted { return }        // iOS v0.6 导航画面上点了静音
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.isEmpty { return }
        if Settings.shared.screenVoice {                // 只从圆屏出声；圆屏不可用时不退回手机喇叭
            ScreenVoice.shared.speak(utterance(t))
            return
        }
        releaseWork?.cancel()
        if busy == 0 { configureSession(active: true) }
        busy += 1
        phoneSpeaking = true
        synth.speak(utterance(t))
    }

    func utterance(_ text: String) -> AVSpeechUtterance {
        let u = AVSpeechUtterance(string: text)
        u.voice = selectedVoice()
        u.rate = Float(UserDefaults.standard.object(forKey: "voice_rate") as? Double ?? Double(AVSpeechUtteranceDefaultSpeechRate))
        u.pitchMultiplier = Float(UserDefaults.standard.object(forKey: "voice_pitch") as? Double ?? 1.0)
        return u
    }

    private func finished() {
        busy = max(0, busy - 1)
        if busy == 0 {
            let w = DispatchWorkItem { [weak self] in
                guard let self = self, self.busy == 0 else { return }
                self.phoneSpeaking = false
                self.configureSession(active: false)
            }
            releaseWork = w
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: w)
        }
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) { DispatchQueue.main.async { self.finished() } }
    func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) { DispatchQueue.main.async { self.finished() } }

    /// 马上停止正在念的（点了静音）
    func stopNow() { synth.stopSpeaking(at: .immediate); ScreenVoice.shared.stop() }

    /// 试听（不管静音开关）
    func test() {
        let m = Settings.shared.voiceMuted
        Settings.shared.voiceMuted = false
        speak("前方三百米右转，进入人民南路。注意，前方有测速，限速六十。")
        Settings.shared.voiceMuted = m
    }
}

/// 圆屏喇叭播报：用 iPhone 的语音“写”成声音数据（不经过手机喇叭，也不打开音频会话），
/// 转 16 kHz、压缩后经蓝牙大块数据类型 5 发给圆屏。圆屏最多存两句（正在放 + 下一句），收到 "VE,编号" 表示放完。
final class ScreenVoice: NSObject, ObservableObject, AVSpeechSynthesizerDelegate {
    static let shared = ScreenVoice()
    static let bulkType = 5
    private let synth = AVSpeechSynthesizer()
    private let target = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(ScreenVoicePacker.rate), channels: 1, interleaved: false)!
    private var waiting: [AVSpeechUtterance] = []       // 还没合成
    private var rendering: AVSpeechUtterance?
    private var renderGeneration = 0
    /// 一句合成中的声音；合成回调和主线程的结束兜底共用。
    private final class Render { var pcm: [Int16] = []; var converter: AVAudioConverter?; var finished = false }
    private var current = Render()
    private var clips: [Data] = []                      // 合成好，还没发
    private var transferId: Int?                        // 正在传，等圆屏回 K
    private var onScreen: Set<Int> = []                 // 圆屏收下了还没放完
    private var nextId = 0
    @Published private(set) var lastStatus = ""

    override init() { super.init(); synth.delegate = self }

    var available: Bool { BleLink.shared.isReady && BleLink.shared.voiceSupported }
    var isSpeaking: Bool { rendering != nil || !waiting.isEmpty || !clips.isEmpty || transferId != nil || !onScreen.isEmpty }

    func speak(_ u: AVSpeechUtterance) {
        guard available else { lastStatus = "圆屏未连接或固件不支持，这句没有播报"; return }
        waiting.append(u)
        renderNext()
    }

    /// 静音：丢掉排队的，并让圆屏停下正在放的。
    func stop() {
        waiting.removeAll(); clips.removeAll()
        if rendering != nil { renderGeneration += 1; rendering = nil; synth.stopSpeaking(at: .immediate) }
        if available, !onScreen.isEmpty || transferId != nil { BleLink.shared.sendOnce("#VX") }
        onScreen.removeAll()
    }

    func linkLost() {
        waiting.removeAll(); clips.removeAll(); transferId = nil; onScreen.removeAll()
    }

    func acknowledged(id: Int) {
        guard transferId == id else { return }
        transferId = nil
        pump()
    }

    func ended(id: Int) {
        onScreen.remove(id)
        pump()
    }

    // MARK: 合成

    private func renderNext() {
        guard rendering == nil, !waiting.isEmpty else { return }
        let u = waiting.removeFirst()
        rendering = u
        renderGeneration += 1
        let generation = renderGeneration
        let job = Render()
        current = job
        let target = self.target
        synth.write(u) { [weak self] buffer in
            guard let pcmBuffer = buffer as? AVAudioPCMBuffer, !job.finished else { return }
            if pcmBuffer.frameLength == 0 {             // 合成结束
                Self.finish(job, to: target)
                DispatchQueue.main.async { self?.rendered(job, generation: generation) }
                return
            }
            if job.converter == nil || job.converter!.inputFormat != pcmBuffer.format {
                if let c = job.converter { job.pcm += Self.convert(c, nil, to: target) }
                job.converter = AVAudioConverter(from: pcmBuffer.format, to: target)
            }
            if let c = job.converter { job.pcm += Self.convert(c, pcmBuffer, to: target) }
        }
    }

    private static func finish(_ job: Render, to target: AVAudioFormat) {
        if let c = job.converter { job.pcm += convert(c, nil, to: target) }
        job.converter = nil; job.finished = true
    }

    /// 有的系统版本不给最后那个空缓冲，只回调 didFinish：0.5 秒后用已收到的声音兜底（重复的会被 generation 挡掉）。
    func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self = self, self.rendering === u else { return }
            let job = self.current
            if !job.finished { Self.finish(job, to: self.target) }
            self.rendered(job, generation: self.renderGeneration)
        }
    }

    private func rendered(_ job: Render, generation: Int) {
        guard generation == renderGeneration, rendering != nil else { return }
        rendering = nil
        if let clip = ScreenVoicePacker.pack(job.pcm) { clips.append(clip) } else { lastStatus = "这句没有合成出声音" }
        renderNext()
        pump()
    }

    /// 送进一段（nil = 输入结束，把重采样器里剩下的取出来）。
    private static func convert(_ converter: AVAudioConverter, _ input: AVAudioPCMBuffer?, to format: AVAudioFormat) -> [Int16] {
        let frames = AVAudioFrameCount(Double(input?.frameLength ?? 0) * format.sampleRate / converter.inputFormat.sampleRate) + 256
        guard let out = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frames) else { return [] }
        var fed = false
        var error: NSError?
        converter.convert(to: out, error: &error) { _, status in
            guard let input = input else { status.pointee = .endOfStream; return nil }
            if fed { status.pointee = .noDataNow; return nil }
            fed = true; status.pointee = .haveData
            return input
        }
        guard error == nil, let channel = out.int16ChannelData else { return [] }
        return Array(UnsafeBufferPointer(start: channel[0], count: Int(out.frameLength)))
    }

    // MARK: 发送

    private func pump() {
        guard transferId == nil, onScreen.count < 2, !clips.isEmpty else { return }
        guard available else { clips.removeAll(); lastStatus = "圆屏断开，没播报的已丢弃"; return }
        let clip = clips.removeFirst()
        let id = nextId
        nextId = (nextId + 1) & 0xFF
        guard BleLink.shared.sendBulk(type: Self.bulkType, id: id, data: clip) else {
            lastStatus = "蓝牙数据包太小，无法传声音"; return
        }
        transferId = id
        onScreen.insert(id)
        lastStatus = String(format: "已发给圆屏（%.1f 秒，%d KB）", ScreenVoicePacker.duration(clip), clip.count / 1024)
        // 兜底：确认或“放完”丢了也不能一直算“正在播报”。
        DispatchQueue.main.asyncAfter(deadline: .now() + 8) { [weak self] in
            guard let self = self, self.transferId == id else { return }
            self.transferId = nil; self.onScreen.remove(id); self.pump()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + ScreenVoicePacker.duration(clip) + 15) { [weak self] in
            guard let self = self, self.onScreen.contains(id) else { return }
            self.onScreen.remove(id); self.pump()
        }
    }
}
